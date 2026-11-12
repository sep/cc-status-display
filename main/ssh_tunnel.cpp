// ============================================================================
// SSH remote-forward tunnel — see ssh_tunnel.h for the design summary.
//
// Lifecycle (one FreeRTOS task):
//
//   wait for Wi-Fi ─▶ TCP connect ─▶ SSH handshake ─▶ host-key check
//     ─▶ publickey auth ─▶ keepalive on ─▶ forward_listen(localhost:PORT)
//     ─▶ [non-blocking service loop]: accept / read channels / keepalive
//     ─▶ on any fatal error: tear down, sleep, start over.
//
// libssh2 is not thread-safe per session, so every libssh2 call — including
// the replies the line handler writes via ssh_tunnel_write() — happens on
// this task. The handler is invoked synchronously from the read loop.
// ============================================================================

#include "ssh_tunnel.h"
#include "remote_link.h"
#include "sdkconfig.h"

#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <mbedtls/base64.h>
#include <sys/select.h>

#include <libssh2.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

static const char *TAG = "ssh-tunnel";

const char *tunnel_wifi_name(TunnelWifi s) {
  switch (s) {
    case TunnelWifi::Off:        return "off";
    case TunnelWifi::Down:       return "down";
    case TunnelWifi::Associated: return "associated";
    case TunnelWifi::Up:         return "up";
  }
  return "?";
}

const char *tunnel_ssh_name(TunnelSsh s) {
  switch (s) {
    case TunnelSsh::Off:             return "off";
    case TunnelSsh::WaitingWifi:     return "waiting_wifi";
    case TunnelSsh::Connecting:      return "connecting";
    case TunnelSsh::HostkeyMismatch: return "hostkey_mismatch";
    case TunnelSsh::AuthFailed:      return "auth_failed";
    case TunnelSsh::ForwardRefused:  return "forward_refused";
    case TunnelSsh::Listening:       return "listening";
    case TunnelSsh::Down:            return "down";
  }
  return "?";
}

#ifdef CONFIG_CC_REMOTE_LINK

// ----------------------------------------------------------------------------
// Heap visibility. "session_init failed" on hardware told us nothing; this
// logs the shape of the heap at the interesting moments, and the custom
// allocator below reports the exact size of any request libssh2 makes that
// fails. PSRAM is preferred when the board has it (Lonely Binary N16R8:
// 8 MB; SparkFun Thing Plus: none) so libssh2's session/channel/kex buffers
// don't compete with Wi-Fi, lwIP and the HUB75 DMA buffers for internal RAM.
// ----------------------------------------------------------------------------

static void log_heap(const char *where) {
  ESP_LOGI(TAG, "heap @%s: free=%u largest=%u internal=%u spiram=%u", where,
           (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void *ssh2_alloc(size_t count, void **abstract) {
  (void)abstract;
  void *p = heap_caps_malloc_prefer(count, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
  if (!p) {
    ESP_LOGE(TAG, "libssh2 alloc(%u) FAILED", (unsigned)count);
    log_heap("alloc-fail");
  }
  return p;
}

static void *ssh2_realloc(void *ptr, size_t count, void **abstract) {
  (void)abstract;
  void *p = heap_caps_realloc_prefer(ptr, count, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
  if (!p && count) {
    ESP_LOGE(TAG, "libssh2 realloc(%u) FAILED", (unsigned)count);
    log_heap("realloc-fail");
  }
  return p;
}

static void ssh2_free(void *ptr, void **abstract) {
  (void)abstract;
  heap_caps_free(ptr);
}

// ----------------------------------------------------------------------------
// Runtime state exposed to tunnel_status / the status pixels
// ----------------------------------------------------------------------------

static volatile TunnelSsh s_ssh_state      = TunnelSsh::Off;
static volatile uint32_t  s_cfg_gen        = 0;      // bumped by ssh_tunnel_apply_config()
static char               s_last_error[96] = {0};
static char               s_ip[16]         = {0};
static char               s_hostkey_fp[64] = {0};
static volatile int       s_hostkey_match  = -1;
static SshEventSink       s_events         = nullptr;

static void set_error(const char *msg) {
  strncpy(s_last_error, msg ? msg : "", sizeof(s_last_error) - 1);
  s_last_error[sizeof(s_last_error) - 1] = '\0';
}

// ----------------------------------------------------------------------------
// Wi-Fi station
// ----------------------------------------------------------------------------

static EventGroupHandle_t s_wifi_eg = nullptr;
static volatile TunnelWifi s_wifi_state = TunnelWifi::Off;
static volatile bool       s_wifi_started = false;
static constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;

static void wifi_event_handler(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    s_wifi_state = TunnelWifi::Down;
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    s_wifi_state = TunnelWifi::Down;
    // The reason code is the single most useful RF-debugging datum: e.g.
    // 200 BEACON_TIMEOUT / 201 NO_AP_FOUND (weak or detuned antenna),
    // 15 4WAY_HANDSHAKE_TIMEOUT (uplink frames lost / wrong PSK),
    // 2 AUTH_EXPIRE, 8 ASSOC_LEAVE. Full table: esp_wifi_types.h.
    const auto *e = static_cast<wifi_event_sta_disconnected_t *>(data);
    s_ip[0] = '\0';
    if (s_wifi_started) {
      ESP_LOGW(TAG, "wifi disconnected: reason %d, rssi %d; reconnecting", (int)e->reason, (int)e->rssi);
      esp_wifi_connect();
    } else {
      s_wifi_state = TunnelWifi::Off;
    }
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
    s_wifi_state = TunnelWifi::Associated;
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
      ESP_LOGI(TAG, "wifi associated: rssi %d dBm, channel %d, phy %s%s%s; waiting for a DHCP lease",
               (int)ap.rssi, (int)ap.primary, ap.phy_11b ? "b" : "", ap.phy_11g ? "g" : "", ap.phy_11n ? "n" : "");
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const auto *e = static_cast<ip_event_got_ip_t *>(data);
    ESP_LOGI(TAG, "wifi up, ip " IPSTR, IP2STR(&e->ip_info.ip));
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
    s_wifi_state = TunnelWifi::Up;
    xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
  }
}

static void wifi_init_once() {
  s_wifi_eg = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
}

// Bring the station in line with the stored config: join, re-join with new
// credentials, or leave. Safe from any task (esp_wifi_* are thread-safe).
static void wifi_apply(const RemoteLinkConfig &cfg) {
  if (!cfg.enabled || !cfg.wifi_complete()) {
    if (s_wifi_started) {
      s_wifi_started = false;
      esp_wifi_disconnect();
      esp_wifi_stop();
      ESP_LOGI(TAG, "wifi: stopped (link disabled or no SSID)");
    }
    s_wifi_state = TunnelWifi::Off;
    xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    return;
  }
  wifi_config_t wc = {};
  strncpy(reinterpret_cast<char *>(wc.sta.ssid), cfg.ssid, sizeof(wc.sta.ssid) - 1);
  strncpy(reinterpret_cast<char *>(wc.sta.password), cfg.psk, sizeof(wc.sta.password) - 1);
  wc.sta.threshold.authmode = cfg.psk[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  if (!s_wifi_started) {
    s_wifi_started = true;
    ESP_ERROR_CHECK(esp_wifi_start());
    // Mains-powered display: no reason to modem-sleep, and power save on
    // some APs makes the station miss DHCP replies for minutes at a time.
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "wifi: joining '%s' (power save off)", cfg.ssid);
  } else {
    ESP_LOGI(TAG, "wifi: re-joining '%s' with new settings", cfg.ssid);
    esp_wifi_disconnect();  // handler reconnects with the new config
  }
}

// ----------------------------------------------------------------------------
// Session state
// ----------------------------------------------------------------------------

static constexpr int    MAX_CLIENTS = 4;
static constexpr size_t SSH_LINE_MAX = 512;  // mirrors serial_reader_task

struct SshClient {
  LIBSSH2_CHANNEL *ch  = nullptr;
  size_t           len = 0;
  char             line[SSH_LINE_MAX];
};

static SshClient         s_clients[MAX_CLIENTS];
static LIBSSH2_SESSION  *s_session   = nullptr;
static LIBSSH2_LISTENER *s_listener  = nullptr;
static int               s_sock      = -1;
static volatile bool     s_connected = false;
static volatile int      s_nclients  = 0;
static SshLineHandler    s_handler   = nullptr;

static const char *last_error() {
  static char fallback[] = "(no session)";
  if (!s_session) return fallback;
  char *msg = nullptr;
  int   len = 0;
  libssh2_session_last_error(s_session, &msg, &len, 0);
  return msg ? msg : "(no message)";
}

// Errors that mean "this session is dead; reconnect". Everything else is
// per-channel or transient (EAGAIN, channel-unknown from a benign accept poll).
static bool is_fatal(int err) {
  switch (err) {
    case LIBSSH2_ERROR_SOCKET_RECV:
    case LIBSSH2_ERROR_SOCKET_SEND:
    case LIBSSH2_ERROR_SOCKET_DISCONNECT:
    case LIBSSH2_ERROR_SOCKET_TIMEOUT:
    case LIBSSH2_ERROR_BAD_SOCKET:
    case LIBSSH2_ERROR_PROTO:
    case LIBSSH2_ERROR_DECRYPT:
    case LIBSSH2_ERROR_KEY_EXCHANGE_FAILURE:
      return true;
    default:
      return false;
  }
}

// Base64 the 32-byte SHA256 host-key hash the way ssh-keygen -lf prints it
// (no padding). `out` needs >= 45 bytes.
static void fingerprint_b64(const char *hash, char *out, size_t cap) {
  size_t olen = 0;
  if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(out), cap, &olen,
                            reinterpret_cast<const unsigned char *>(hash), 32) != 0) {
    snprintf(out, cap, "(encode failed)");
    return;
  }
  while (olen > 0 && out[olen - 1] == '=') out[--olen] = '\0';
}

static bool fingerprint_matches(const char *actual_b64, const char *expected) {
  if (strncmp(expected, "SHA256:", 7) == 0) expected += 7;
  const size_t n = strlen(expected);
  size_t m = n;
  while (m > 0 && expected[m - 1] == '=') m--;  // tolerate padded form
  return strlen(actual_b64) == m && strncmp(actual_b64, expected, m) == 0;
}

static int tcp_connect(const char *host, int port) {
  char portstr[8];
  snprintf(portstr, sizeof(portstr), "%d", port);
  addrinfo hints = {};
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *res = nullptr;
  const int rc = getaddrinfo(host, portstr, &hints, &res);
  if (rc != 0 || !res) {
    ESP_LOGE(TAG, "dns: cannot resolve '%s' (%d)", host, rc);
    return -1;
  }
  int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (s < 0) {
    ESP_LOGE(TAG, "socket() failed: errno %d", errno);
    freeaddrinfo(res);
    return -1;
  }
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
  if (connect(s, res->ai_addr, res->ai_addrlen) != 0) {
    ESP_LOGE(TAG, "connect(%s:%d) failed: errno %d", host, port, errno);
    close(s);
    freeaddrinfo(res);
    return -1;
  }
  freeaddrinfo(res);
  return s;
}

// Free one channel. libssh2_channel_free can EAGAIN in non-blocking mode and
// then NOT free, so flip to blocking with a short timeout for the call.
static void close_client(SshClient &c, const char *why) {
  if (!c.ch) return;
  const int idx = static_cast<int>(&c - s_clients);
  ESP_LOGI(TAG, "client %d closed (%s)", idx, why);
  libssh2_session_set_blocking(s_session, 1);
  libssh2_session_set_timeout(s_session, 2000);
  libssh2_channel_free(c.ch);
  libssh2_session_set_blocking(s_session, 0);
  c.ch  = nullptr;
  c.len = 0;
  s_nclients = s_nclients - 1;
}

static void teardown() {
  s_connected = false;
  if (s_session) {
    libssh2_session_set_blocking(s_session, 1);
    libssh2_session_set_timeout(s_session, 2000);
    for (auto &c : s_clients) {
      if (c.ch) { libssh2_channel_free(c.ch); c.ch = nullptr; c.len = 0; }
    }
    if (s_listener) { libssh2_channel_forward_cancel(s_listener); s_listener = nullptr; }
    libssh2_session_disconnect(s_session, "claudepanel: reconnecting");
    libssh2_session_free(s_session);
    s_session = nullptr;
  }
  if (s_sock >= 0) { close(s_sock); s_sock = -1; }
  s_nclients = 0;
}

// Bring a session all the way up to "listener bound, non-blocking". On any
// failure the partial state is torn down and false is returned.
static bool session_up(const RemoteLinkConfig &cfg) {
  s_ssh_state = TunnelSsh::Connecting;
  s_sock = tcp_connect(cfg.host, cfg.port);
  if (s_sock < 0) { set_error("tcp connect failed"); s_ssh_state = TunnelSsh::Down; return false; }

  log_heap("before-session");
  s_session = libssh2_session_init_ex(ssh2_alloc, ssh2_free, ssh2_realloc, nullptr);
  if (!s_session) {
    ESP_LOGE(TAG, "libssh2_session_init failed (see alloc log above)");
    set_error("session alloc failed");
    s_ssh_state = TunnelSsh::Down;
    teardown();
    return false;
  }
#ifdef CONFIG_CC_SSH_TRACE
  libssh2_trace(s_session, LIBSSH2_TRACE_TRANS | LIBSSH2_TRACE_KEX | LIBSSH2_TRACE_AUTH |
                               LIBSSH2_TRACE_CONN | LIBSSH2_TRACE_ERROR | LIBSSH2_TRACE_SOCKET);
#endif
  libssh2_session_set_blocking(s_session, 1);
  libssh2_session_set_timeout(s_session, 20000);

  if (libssh2_session_handshake(s_session, s_sock) != 0) {
    ESP_LOGE(TAG, "handshake failed: %s", last_error());
    set_error(last_error());
    s_ssh_state = TunnelSsh::Down;
    teardown();
    return false;
  }

  // Host key: record what we saw, tell the host (hostkey_seen), enforce the
  // pin if there is one.
  const char *hash = libssh2_hostkey_hash(s_session, LIBSSH2_HOSTKEY_HASH_SHA256);
  char fp[48] = {0};
  if (hash) fingerprint_b64(hash, fp, sizeof(fp));
  snprintf(s_hostkey_fp, sizeof(s_hostkey_fp), "SHA256:%s", fp);
  const bool pinned = cfg.hostkey_fp[0] != '\0';
  const bool match  = pinned && hash && fingerprint_matches(fp, cfg.hostkey_fp);
  s_hostkey_match = pinned ? (match ? 1 : 0) : -1;
  ESP_LOGI(TAG, "server host key %s (%s)", s_hostkey_fp,
           pinned ? (match ? "pinned, match" : "PINNED, MISMATCH") : "not pinned");
  if (s_events) {
    char ev[256];
    const char *kt = libssh2_session_methods(s_session, LIBSSH2_METHOD_HOSTKEY);
    const int n = snprintf(ev, sizeof(ev),
        "{\"type\":\"hostkey_seen\",\"fingerprint\":\"%s\",\"keytype\":\"%s\","
        "\"pinned\":%s,\"match\":%s,\"uptime_ms\":%lld}\n",
        s_hostkey_fp, kt ? kt : "?", pinned ? "true" : "false",
        pinned ? (match ? "true" : "false") : "null",
        static_cast<long long>(esp_timer_get_time() / 1000));
    if (n > 0 && n < static_cast<int>(sizeof(ev))) s_events(ev, static_cast<size_t>(n));
  }
  if (pinned && !match) {
    ESP_LOGE(TAG, "HOST KEY MISMATCH — expected %s; refusing to authenticate", cfg.hostkey_fp);
    set_error("host key mismatch");
    s_ssh_state = TunnelSsh::HostkeyMismatch;
    teardown();
    return false;
  }

  // Identity from NVS. Both halves are handed to libssh2 as text; the public
  // line is parsed by libssh2 itself, the PEM by the mbedTLS backend.
  char pem[512];
  char pub[200];
  const size_t pemlen = remote_link_key_pem(pem, sizeof(pem));
  const size_t publen = remote_link_pubkey_line(pub, sizeof(pub));
  int auth_rc = -1;
  if (pemlen && publen)
    auth_rc = libssh2_userauth_publickey_frommemory(s_session, cfg.user, strlen(cfg.user),
                                                    pub, publen, pem, pemlen, nullptr);
  memset(pem, 0, sizeof(pem));
  if (auth_rc != 0) {
    ESP_LOGE(TAG, "publickey auth failed for '%s': %s", cfg.user,
             (pemlen && publen) ? last_error() : "no identity stored (send keygen)");
    set_error((pemlen && publen) ? last_error() : "no identity stored");
    s_ssh_state = TunnelSsh::AuthFailed;
    teardown();
    return false;
  }
  ESP_LOGI(TAG, "authenticated as '%s'", cfg.user);

  // Server-side keepalive every 15 s with a reply requested, so a dead TCP
  // path surfaces as a send error instead of silence.
  libssh2_keepalive_config(s_session, 1, 15);

  int bound = 0;
  s_listener = libssh2_channel_forward_listen_ex(s_session, "localhost",
                                                 cfg.listen_port, &bound, MAX_CLIENTS);
  if (!s_listener) {
    ESP_LOGE(TAG, "remote forward on localhost:%d refused: %s — usually the port is "
                  "already held by another session (a previous instance of this device "
                  "that dropped off Wi-Fi? set ClientAliveInterval on the server), else "
                  "check permitlisten= in authorized_keys / AllowTcpForwarding",
             cfg.listen_port, last_error());
    set_error("forward refused (port in use?)");
    s_ssh_state = TunnelSsh::ForwardRefused;
    teardown();
    return false;
  }
  ESP_LOGI(TAG, "remote forward listening on %s:localhost:%d", cfg.host, bound);
  log_heap("listening");
  ESP_LOGI(TAG, "ssh task stack high-water mark: %u bytes free of %d",
           (unsigned)uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t), CONFIG_CC_SSH_TASK_STACK);

  libssh2_session_set_blocking(s_session, 0);
  set_error("");
  s_ssh_state = TunnelSsh::Listening;
  s_connected = true;
  return true;
}

static void feed_bytes(SshClient &c, const char *buf, ssize_t n) {
  for (ssize_t i = 0; i < n; i++) {
    const char ch = buf[i];
    if (ch == '\r') continue;
    if (ch == '\n') {
      c.line[c.len] = '\0';
      if (s_handler) s_handler(c.line, c.len, &c);
      c.len = 0;
    } else if (c.len < SSH_LINE_MAX - 1) {
      c.line[c.len++] = ch;
    } else {
      c.len = 0;  // over-long line; resync on next '\n'
    }
  }
}

// One pass of the non-blocking service loop. Returns false when the session
// is dead and must be rebuilt.
static bool service_once() {
  // 1) Accept any newly-forwarded connection.
  if (LIBSSH2_CHANNEL *ch = libssh2_channel_forward_accept(s_listener)) {
    SshClient *slot = nullptr;
    for (auto &c : s_clients) if (!c.ch) { slot = &c; break; }
    if (!slot) {
      ESP_LOGW(TAG, "refusing client: all %d slots busy", MAX_CLIENTS);
      libssh2_session_set_blocking(s_session, 1);
      libssh2_session_set_timeout(s_session, 2000);
      libssh2_channel_free(ch);
      libssh2_session_set_blocking(s_session, 0);
    } else {
      slot->ch  = ch;
      slot->len = 0;
      s_nclients = s_nclients + 1;
      ESP_LOGI(TAG, "client %d connected (%d active)",
               static_cast<int>(slot - s_clients), static_cast<int>(s_nclients));
    }
  } else {
    const int err = libssh2_session_last_errno(s_session);
    if (is_fatal(err)) {
      ESP_LOGW(TAG, "accept: session error %d: %s", err, last_error());
      return false;
    }
  }

  // 2) Drain every channel.
  for (auto &c : s_clients) {
    if (!c.ch) continue;
    char buf[256];
    for (;;) {
      const ssize_t n = libssh2_channel_read(c.ch, buf, sizeof(buf));
      if (n > 0) {
        feed_bytes(c, buf, n);
        continue;
      }
      if (n == LIBSSH2_ERROR_EAGAIN) break;
      if (n == 0) {
        if (libssh2_channel_eof(c.ch)) close_client(c, "eof");
        break;
      }
      const int err = static_cast<int>(n);
      if (is_fatal(err)) {
        ESP_LOGW(TAG, "read: session error %d: %s", err, last_error());
        return false;
      }
      close_client(c, last_error());
      break;
    }
  }

  // 3) Keepalive (libssh2 rate-limits internally to the configured interval).
  int next_s = 0;
  if (libssh2_keepalive_send(s_session, &next_s) != 0) {
    const int err = libssh2_session_last_errno(s_session);
    if (is_fatal(err)) {
      ESP_LOGW(TAG, "keepalive: session error %d: %s", err, last_error());
      return false;
    }
  }

  // 4) While bridges are connected, re-check the stack high-water mark once
  //    a minute — the reply path (handle_line → send_pong) runs on this task
  //    and is the deepest thing it does. Guards the 16 KB default.
  static int64_t s_last_hwm_log = 0;
  const int64_t now = esp_timer_get_time();
  if (s_nclients > 0 && now - s_last_hwm_log > 60LL * 1000 * 1000) {
    s_last_hwm_log = now;
    ESP_LOGI(TAG, "ssh task stack high-water mark: %u bytes free of %d (%d client%s)",
             (unsigned)uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t),
             CONFIG_CC_SSH_TASK_STACK, (int)s_nclients, s_nclients == 1 ? "" : "s");
  }

  // 5) Sleep until the socket has something, or 100 ms, whichever first.
  fd_set rf;
  FD_ZERO(&rf);
  FD_SET(s_sock, &rf);
  timeval tv = {0, 100 * 1000};
  select(s_sock + 1, &rf, nullptr, nullptr, &tv);
  return true;
}

static void ssh_task(void *) {
  libssh2_init(0);
  for (;;) {
    const uint32_t gen = s_cfg_gen;
    RemoteLinkConfig cfg;
    remote_link_load(cfg);
    const bool ready = cfg.enabled && cfg.wifi_complete() && cfg.ssh_complete() && remote_link_has_key();
    if (!ready) {
      s_ssh_state = TunnelSsh::Off;
      if (cfg.enabled)
        ESP_LOGW(TAG, "link enabled but incomplete (%s%s%s) — waiting for provisioning",
                 cfg.wifi_complete() ? "" : "no wifi; ",
                 cfg.ssh_complete() ? "" : "no ssh settings; ",
                 remote_link_has_key() ? "" : "no key");
      while (gen == s_cfg_gen) vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    s_ssh_state = TunnelSsh::WaitingWifi;
    int64_t last_nag = esp_timer_get_time();
    while (gen == s_cfg_gen &&
           !(xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                 pdMS_TO_TICKS(1000)) & WIFI_CONNECTED_BIT)) {
      if (esp_timer_get_time() - last_nag > 15LL * 1000 * 1000) {
        last_nag = esp_timer_get_time();
        ESP_LOGW(TAG, "still no IP address; if this persists, check the AP's DHCP lease table for this MAC");
      }
    }
    if (gen != s_cfg_gen) continue;

    ESP_LOGI(TAG, "connecting %s@%s:%u", cfg.user, cfg.host, cfg.port);
    if (session_up(cfg)) {
      while (gen == s_cfg_gen && service_once()) {
        if (!(xEventGroupGetBits(s_wifi_eg) & WIFI_CONNECTED_BIT)) {
          ESP_LOGW(TAG, "wifi dropped; abandoning session");
          break;
        }
      }
      teardown();
      if (s_ssh_state == TunnelSsh::Listening) s_ssh_state = TunnelSsh::Down;
    }
    if (gen != s_cfg_gen) { ESP_LOGI(TAG, "reconfigured; restarting link"); continue; }
    ESP_LOGW(TAG, "session down (%s); retry in %d ms", tunnel_ssh_name(s_ssh_state), CONFIG_CC_SSH_RECONNECT_MS);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_CC_SSH_RECONNECT_MS));
  }
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

void ssh_tunnel_start(SshLineHandler handler, SshEventSink events) {
  s_handler = handler;
  s_events  = events;
  log_heap("boot");
#ifdef CONFIG_SPIRAM
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
    ESP_LOGW(TAG, "built with PSRAM support but no PSRAM pool found. Stale sdkconfig? "
                  "(`idf.py fullclean`, rebuild) — or the module's PSRAM isn't octal. "
                  "libssh2 needs ~80 KB contiguous; internal RAM alone may not have it.");
  }
#else
  ESP_LOGW(TAG, "built WITHOUT PSRAM support; libssh2 needs ~80 KB contiguous RAM");
#endif
  wifi_init_once();
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  ESP_LOGI(TAG, "sta mac %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  RemoteLinkConfig cfg;
  remote_link_load(cfg);
  ESP_LOGI(TAG, "remote link: %s%s", cfg.enabled ? "enabled" : "disabled",
           cfg.enabled && !(cfg.wifi_complete() && cfg.ssh_complete() && remote_link_has_key())
               ? " (incomplete — provision over USB)" : "");
  wifi_apply(cfg);

  if (xTaskCreate(ssh_task, "ssh_tunnel", CONFIG_CC_SSH_TASK_STACK, nullptr, 4, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "failed to create ssh task (stack %d bytes)", CONFIG_CC_SSH_TASK_STACK);
  }
}

void ssh_tunnel_apply_config() {
  RemoteLinkConfig cfg;
  remote_link_load(cfg);
  wifi_apply(cfg);
  s_cfg_gen = s_cfg_gen + 1;   // the tunnel task notices and restarts
}

bool ssh_tunnel_write(SshClient *client, const char *buf, size_t n) {
  if (!client || !client->ch) return false;
  size_t        off      = 0;
  const int64_t deadline = esp_timer_get_time() + 500 * 1000;
  while (off < n) {
    const ssize_t w = libssh2_channel_write(client->ch, buf + off, n - off);
    if (w == LIBSSH2_ERROR_EAGAIN) {
      if (esp_timer_get_time() > deadline) return false;
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    if (w < 0) return false;
    off += static_cast<size_t>(w);
  }
  return true;
}

bool       ssh_tunnel_is_connected() { return s_connected; }
TunnelWifi ssh_tunnel_wifi_state()   { return s_wifi_state; }
TunnelSsh  ssh_tunnel_ssh_state()    { return s_ssh_state; }
int        ssh_tunnel_client_count() { return s_nclients; }

void ssh_tunnel_status(TunnelStatus &o) {
  o = TunnelStatus{};
  o.wifi    = s_wifi_state;
  o.ssh     = s_ssh_state;
  o.clients = s_nclients;
  o.rssi    = 0;
  if (o.wifi == TunnelWifi::Up || o.wifi == TunnelWifi::Associated) {
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) o.rssi = ap.rssi;
  }
  strncpy(o.ip, s_ip, sizeof(o.ip) - 1);
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(o.mac, sizeof(o.mac), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  strncpy(o.last_error, s_last_error, sizeof(o.last_error) - 1);
  strncpy(o.hostkey_fp, s_hostkey_fp, sizeof(o.hostkey_fp) - 1);
  RemoteLinkConfig cfg;
  remote_link_load(cfg);
  o.hostkey_pinned = cfg.hostkey_fp[0] != '\0';
  o.hostkey_match  = s_hostkey_match;
}

int ssh_tunnel_json_status(char *out, size_t cap) {
  TunnelStatus st;
  ssh_tunnel_status(st);
  RemoteLinkConfig cfg;
  remote_link_load(cfg);
  char ssid[80], host[520], user[80], err[220];
  json_escape(cfg.ssid, ssid, sizeof(ssid));
  json_escape(cfg.host, host, sizeof(host));
  json_escape(cfg.user, user, sizeof(user));
  json_escape(st.last_error, err, sizeof(err));
  const bool provisioned = cfg.wifi_complete() && cfg.ssh_complete();
  const int n = snprintf(out, cap,
      "{\"type\":\"tunnel_status\","
      "\"enabled\":%s,\"provisioned\":%s,\"has_key\":%s,\"hostkey_pinned\":%s,"
      "\"wifi\":{\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"mac\":\"%s\"},"
      "\"ssh\":{\"state\":\"%s\",\"host\":\"%s\",\"port\":%u,\"user\":\"%s\",\"listen_port\":%u,"
      "\"clients\":%d,\"hostkey_seen\":\"%s\",\"last_error\":%s%s%s},"
      "\"uptime_ms\":%lld}\n",
      cfg.enabled ? "true" : "false", provisioned ? "true" : "false",
      remote_link_has_key() ? "true" : "false", st.hostkey_pinned ? "true" : "false",
      tunnel_wifi_name(st.wifi), ssid, st.ip, st.rssi, st.mac,
      tunnel_ssh_name(st.ssh), host, cfg.port, user, cfg.listen_port,
      st.clients, st.hostkey_fp,
      st.last_error[0] ? "\"" : "null", err, st.last_error[0] ? "\"" : "",
      static_cast<long long>(esp_timer_get_time() / 1000));
  return (n > 0 && static_cast<size_t>(n) < cap) ? n : 0;
}

int ssh_tunnel_json_summary(char *out, size_t cap) {
  const int n = snprintf(out, cap, "\"tunnel\":{\"wifi\":\"%s\",\"ssh\":\"%s\",\"clients\":%d},",
                         tunnel_wifi_name(s_wifi_state), tunnel_ssh_name(s_ssh_state), (int)s_nclients);
  return (n > 0 && static_cast<size_t>(n) < cap) ? n : 0;
}

#else  // !CONFIG_CC_REMOTE_LINK — USB-only build: every entry point is a no-op

void ssh_tunnel_start(SshLineHandler, SshEventSink) {
  ESP_LOGI(TAG, "remote link not compiled in (CONFIG_CC_REMOTE_LINK=n)");
}
void       ssh_tunnel_apply_config() {}
bool       ssh_tunnel_write(SshClient *, const char *, size_t) { return false; }
bool       ssh_tunnel_is_connected() { return false; }
TunnelWifi ssh_tunnel_wifi_state()   { return TunnelWifi::Off; }
TunnelSsh  ssh_tunnel_ssh_state()    { return TunnelSsh::Off; }
int        ssh_tunnel_client_count() { return 0; }
void       ssh_tunnel_status(TunnelStatus &o) { o = TunnelStatus{}; }
int        ssh_tunnel_json_status(char *, size_t) { return 0; }
int        ssh_tunnel_json_summary(char *, size_t) { return 0; }

#endif  // CONFIG_CC_REMOTE_LINK
