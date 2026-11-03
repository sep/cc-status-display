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
#include "sdkconfig.h"

#include <esp_event.h>
#include <esp_log.h>
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

// Device identity, embedded by main/CMakeLists.txt (EMBED_TXTFILES appends a
// NUL, so usable length is end - start - 1). Empty placeholders when the
// files are absent → "no key embedded" at runtime.
extern const char s_priv_start[] asm("_binary_id_ecdsa_start");
extern const char s_priv_end[]   asm("_binary_id_ecdsa_end");
extern const char s_pub_start[]  asm("_binary_id_ecdsa_pub_start");
extern const char s_pub_end[]    asm("_binary_id_ecdsa_pub_end");

// ----------------------------------------------------------------------------
// Wi-Fi station
// ----------------------------------------------------------------------------

static EventGroupHandle_t s_wifi_eg = nullptr;
static constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;

static void wifi_event_handler(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    ESP_LOGW(TAG, "wifi disconnected; reconnecting");
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const auto *e = static_cast<ip_event_got_ip_t *>(data);
    ESP_LOGI(TAG, "wifi up, ip " IPSTR, IP2STR(&e->ip_info.ip));
    xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
  }
}

[[maybe_unused]] static void wifi_start() {
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

  wifi_config_t wc = {};
  strncpy(reinterpret_cast<char *>(wc.sta.ssid), CONFIG_CC_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
  strncpy(reinterpret_cast<char *>(wc.sta.password), CONFIG_CC_WIFI_PASSWORD,
          sizeof(wc.sta.password) - 1);
  wc.sta.threshold.authmode =
      (strlen(CONFIG_CC_WIFI_PASSWORD) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_LOGI(TAG, "wifi: joining '%s'", CONFIG_CC_WIFI_SSID);
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
static bool session_up() {
  const size_t privlen = static_cast<size_t>(s_priv_end - s_priv_start) - 1;
  const size_t publen  = static_cast<size_t>(s_pub_end - s_pub_start) - 1;
  if (privlen == 0 || publen == 0) {
    ESP_LOGE(TAG, "no key embedded — create main/keys/id_ecdsa(.pub) and rebuild (see SPIKE-SSH.md)");
    return false;
  }

  s_sock = tcp_connect(CONFIG_CC_SSH_HOST, CONFIG_CC_SSH_PORT);
  if (s_sock < 0) return false;

  s_session = libssh2_session_init();
  if (!s_session) {
    ESP_LOGE(TAG, "libssh2_session_init failed (out of memory?)");
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
    teardown();
    return false;
  }

  const char *hash = libssh2_hostkey_hash(s_session, LIBSSH2_HOSTKEY_HASH_SHA256);
  char fp[48] = {0};
  if (hash) fingerprint_b64(hash, fp, sizeof(fp));
  ESP_LOGI(TAG, "server host key SHA256:%s", fp);
  if (strlen(CONFIG_CC_SSH_HOSTKEY_SHA256) > 0) {
    if (!hash || !fingerprint_matches(fp, CONFIG_CC_SSH_HOSTKEY_SHA256)) {
      ESP_LOGE(TAG, "HOST KEY MISMATCH — expected %s; refusing to authenticate",
               CONFIG_CC_SSH_HOSTKEY_SHA256);
      teardown();
      return false;
    }
  } else {
    ESP_LOGW(TAG, "host key not pinned (spike): accepting. Set CC_SSH_HOSTKEY_SHA256 to pin it.");
  }

  if (libssh2_userauth_publickey_frommemory(s_session, CONFIG_CC_SSH_USER,
                                            strlen(CONFIG_CC_SSH_USER),
                                            s_pub_start, publen,
                                            s_priv_start, privlen, nullptr) != 0) {
    ESP_LOGE(TAG, "publickey auth failed for '%s': %s", CONFIG_CC_SSH_USER, last_error());
    teardown();
    return false;
  }
  ESP_LOGI(TAG, "authenticated as '%s'", CONFIG_CC_SSH_USER);

  // Server-side keepalive every 15 s with a reply requested, so a dead TCP
  // path surfaces as a send error instead of silence.
  libssh2_keepalive_config(s_session, 1, 15);

  int bound = 0;
  s_listener = libssh2_channel_forward_listen_ex(s_session, "localhost",
                                                 CONFIG_CC_SSH_LISTEN_PORT, &bound, MAX_CLIENTS);
  if (!s_listener) {
    ESP_LOGE(TAG, "remote forward on localhost:%d refused: %s "
                  "(check permitlisten= in authorized_keys / AllowTcpForwarding)",
             CONFIG_CC_SSH_LISTEN_PORT, last_error());
    teardown();
    return false;
  }
  ESP_LOGI(TAG, "remote forward listening on %s:localhost:%d", CONFIG_CC_SSH_HOST, bound);

  libssh2_session_set_blocking(s_session, 0);
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

  // 4) Sleep until the socket has something, or 100 ms, whichever first.
  fd_set rf;
  FD_ZERO(&rf);
  FD_SET(s_sock, &rf);
  timeval tv = {0, 100 * 1000};
  select(s_sock + 1, &rf, nullptr, nullptr, &tv);
  return true;
}

[[maybe_unused]] static void ssh_task(void *) {
  libssh2_init(0);
  for (;;) {
    xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "connecting %s@%s:%d", CONFIG_CC_SSH_USER, CONFIG_CC_SSH_HOST, CONFIG_CC_SSH_PORT);
    if (session_up()) {
      while (service_once()) {
        if (!(xEventGroupGetBits(s_wifi_eg) & WIFI_CONNECTED_BIT)) {
          ESP_LOGW(TAG, "wifi dropped; abandoning session");
          break;
        }
      }
      teardown();
    }
    ESP_LOGW(TAG, "session down; retry in %d ms", CONFIG_CC_SSH_RECONNECT_MS);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_CC_SSH_RECONNECT_MS));
  }
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

void ssh_tunnel_start(SshLineHandler handler) {
#ifdef CONFIG_CC_SSH_ENABLE
  s_handler = handler;
  wifi_start();
  if (xTaskCreate(ssh_task, "ssh_tunnel", CONFIG_CC_SSH_TASK_STACK, nullptr, 4, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "failed to create ssh task (stack %d bytes)", CONFIG_CC_SSH_TASK_STACK);
  }
#else
  (void)handler;
  ESP_LOGI(TAG, "disabled (CONFIG_CC_SSH_ENABLE=n)");
#endif
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

bool ssh_tunnel_is_connected() { return s_connected; }
int  ssh_tunnel_client_count() { return s_nclients; }
