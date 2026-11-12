#include "remote_link.h"

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <nvs.h>
#include <psa/crypto.h>

#include <cstdio>
#include <cstring>

static const char *TAG = "remote-link";
static const char *NS  = "tunnel";

// NVS keys (<= 15 chars).
static const char *K_SSID = "wifi_ssid", *K_PSK = "wifi_psk", *K_HOST = "ssh_host",
                  *K_PORT = "ssh_port", *K_USER = "ssh_user", *K_LPORT = "listen_port",
                  *K_EN = "enabled", *K_FP = "hostkey_fp",
                  *K_PEM = "id_key", *K_PUB = "id_pub";   // id_pub = 65-byte SEC1 point

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

static void get_str(nvs_handle_t h, const char *key, char *out, size_t cap) {
  size_t len = cap;
  if (nvs_get_str(h, key, out, &len) != ESP_OK) out[0] = '\0';
}

void remote_link_load(RemoteLinkConfig &c) {
  c = RemoteLinkConfig{};
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
  get_str(h, K_SSID, c.ssid, sizeof(c.ssid));
  get_str(h, K_PSK,  c.psk,  sizeof(c.psk));
  get_str(h, K_HOST, c.host, sizeof(c.host));
  get_str(h, K_USER, c.user, sizeof(c.user));
  get_str(h, K_FP,   c.hostkey_fp, sizeof(c.hostkey_fp));
  uint16_t u16;
  if (nvs_get_u16(h, K_PORT, &u16) == ESP_OK)  c.port = u16;
  if (nvs_get_u16(h, K_LPORT, &u16) == ESP_OK) c.listen_port = u16;
  uint8_t u8;
  if (nvs_get_u8(h, K_EN, &u8) == ESP_OK) c.enabled = (u8 != 0);
  nvs_close(h);
}

esp_err_t remote_link_save(const RemoteLinkConfig &c) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  if (err == ESP_OK) err = nvs_set_str(h, K_SSID, c.ssid);
  if (err == ESP_OK) err = nvs_set_str(h, K_PSK,  c.psk);
  if (err == ESP_OK) err = nvs_set_str(h, K_HOST, c.host);
  if (err == ESP_OK) err = nvs_set_str(h, K_USER, c.user);
  if (err == ESP_OK) err = nvs_set_str(h, K_FP,   c.hostkey_fp);
  if (err == ESP_OK) err = nvs_set_u16(h, K_PORT,  c.port);
  if (err == ESP_OK) err = nvs_set_u16(h, K_LPORT, c.listen_port);
  if (err == ESP_OK) err = nvs_set_u8 (h, K_EN,    c.enabled ? 1 : 0);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  if (err != ESP_OK) ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
  return err;
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

static size_t blob_len(const char *key) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
  size_t len = 0;
  const esp_err_t e = nvs_get_blob(h, key, nullptr, &len);
  nvs_close(h);
  return e == ESP_OK ? len : 0;
}

static size_t blob_get(const char *key, void *out, size_t cap) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
  size_t len = cap;
  const esp_err_t e = nvs_get_blob(h, key, out, &len);
  nvs_close(h);
  return e == ESP_OK ? len : 0;
}

bool remote_link_has_key() { return blob_len(K_PEM) > 0 && blob_len(K_PUB) == 65; }

esp_err_t remote_link_keygen(bool replace, bool *generated) {
  if (generated) *generated = false;
  if (!replace && remote_link_has_key()) return ESP_OK;

  // Generate in PSA, export the public point, and render the private key
  // as SEC1 PEM (named curve) — the one on-disk form libssh2's mbedTLS
  // backend reads back reliably. The PSA key is volatile and destroyed
  // once copied out.
  if (psa_crypto_init() != PSA_SUCCESS) return ESP_FAIL;
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
  psa_set_key_bits(&attr, 256);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_HASH);
  psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
  psa_key_id_t kid = 0;
  if (psa_generate_key(&attr, &kid) != PSA_SUCCESS) {
    ESP_LOGE(TAG, "psa_generate_key failed");
    return ESP_FAIL;
  }

  uint8_t pub[65];
  size_t publen = 0;
  esp_err_t result = ESP_FAIL;
  unsigned char pem[512] = {0};
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);

  if (psa_export_public_key(kid, pub, sizeof(pub), &publen) != PSA_SUCCESS || publen != 65) {
    ESP_LOGE(TAG, "psa_export_public_key failed");
  } else if (mbedtls_pk_copy_from_psa(kid, &pk) != 0) {
    ESP_LOGE(TAG, "mbedtls_pk_copy_from_psa failed");
  } else if (mbedtls_pk_write_key_pem(&pk, pem, sizeof(pem)) != 0) {
    ESP_LOGE(TAG, "mbedtls_pk_write_key_pem failed");
  } else {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) err = nvs_set_blob(h, K_PEM, pem, strlen(reinterpret_cast<char *>(pem)) + 1);
    if (err == ESP_OK) err = nvs_set_blob(h, K_PUB, pub, publen);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
      result = ESP_OK;
      if (generated) *generated = true;
      ESP_LOGI(TAG, "generated new ECDSA P-256 identity");
    } else {
      ESP_LOGE(TAG, "storing identity failed: %s", esp_err_to_name(err));
    }
  }
  mbedtls_pk_free(&pk);
  memset(pem, 0, sizeof(pem));
  psa_destroy_key(kid);
  return result;
}

size_t remote_link_key_pem(char *out, size_t cap) {
  const size_t n = blob_get(K_PEM, out, cap);
  if (n == 0) { if (cap) out[0] = '\0'; return 0; }
  out[n < cap ? n : cap - 1] = '\0';
  return strlen(out);
}

// SSH wire blob for an ecdsa-sha2-nistp256 public key:
//   string "ecdsa-sha2-nistp256" | string "nistp256" | string <65-byte point>
static size_t build_pub_blob(const uint8_t *pt, uint8_t *out) {
  size_t n = 0;
  auto put_str = [&](const void *s, uint32_t len) {
    out[n++] = (len >> 24) & 0xff; out[n++] = (len >> 16) & 0xff;
    out[n++] = (len >> 8) & 0xff;  out[n++] = len & 0xff;
    memcpy(out + n, s, len); n += len;
  };
  put_str("ecdsa-sha2-nistp256", 19);
  put_str("nistp256", 8);
  put_str(pt, 65);
  return n;  // 4+19 + 4+8 + 4+65 = 104
}

static void mac_suffix(char *out, size_t cap) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(out, cap, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

size_t remote_link_pubkey_line(char *out, size_t cap) {
  uint8_t pt[65];
  if (blob_get(K_PUB, pt, sizeof(pt)) != 65) { if (cap) out[0] = '\0'; return 0; }
  uint8_t blob[104];
  const size_t bl = build_pub_blob(pt, blob);
  char b64[160];
  size_t olen = 0;
  if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(b64), sizeof(b64), &olen, blob, bl) != 0)
    { if (cap) out[0] = '\0'; return 0; }
  b64[olen] = '\0';
  char mac[13];
  mac_suffix(mac, sizeof(mac));
  const int n = snprintf(out, cap, "ecdsa-sha2-nistp256 %s claudepanel-%s", b64, mac);
  return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

size_t remote_link_fingerprint(char *out, size_t cap) {
  uint8_t pt[65];
  if (blob_get(K_PUB, pt, sizeof(pt)) != 65) { if (cap) out[0] = '\0'; return 0; }
  uint8_t blob[104];
  const size_t bl = build_pub_blob(pt, blob);
  uint8_t hash[32];
  size_t hl = 0;
  if (psa_crypto_init() != PSA_SUCCESS ||
      psa_hash_compute(PSA_ALG_SHA_256, blob, bl, hash, sizeof(hash), &hl) != PSA_SUCCESS)
    { if (cap) out[0] = '\0'; return 0; }
  char b64[48];
  size_t olen = 0;
  if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(b64), sizeof(b64), &olen, hash, 32) != 0)
    { if (cap) out[0] = '\0'; return 0; }
  while (olen > 0 && b64[olen - 1] == '=') olen--;
  b64[olen] = '\0';
  const int n = snprintf(out, cap, "SHA256:%s", b64);
  return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

size_t json_escape(const char *in, char *out, size_t cap) {
  size_t n = 0;
  for (const char *p = in; *p && n + 7 < cap; p++) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = static_cast<char>(c); }
    else if (c < 0x20) { n += snprintf(out + n, cap - n, "\\u%04x", c); }
    else out[n++] = static_cast<char>(c);
  }
  out[n] = '\0';
  return n;
}

int remote_link_json_provisioned(char *out, size_t cap, bool ok, const char *error) {
  const long long up = esp_timer_get_time() / 1000;
  int n;
  if (!ok) {
    n = snprintf(out, cap, "{\"type\":\"provisioned\",\"ok\":false,\"error\":\"%s\",\"uptime_ms\":%lld}\n",
                 error ? error : "unknown", up);
  } else {
    RemoteLinkConfig c;
    remote_link_load(c);
    char ssid[80], host[520], user[80], fp[140];
    json_escape(c.ssid, ssid, sizeof(ssid));
    json_escape(c.host, host, sizeof(host));
    json_escape(c.user, user, sizeof(user));
    json_escape(c.hostkey_fp, fp, sizeof(fp));
    n = snprintf(out, cap,
        "{\"type\":\"provisioned\",\"ok\":true,"
        "\"wifi\":{\"ssid\":\"%s\"},"
        "\"ssh\":{\"host\":\"%s\",\"port\":%u,\"user\":\"%s\",\"listen_port\":%u},"
        "\"enabled\":%s,\"has_key\":%s,\"hostkey_pinned\":%s,\"hostkey_fp\":\"%s\","
        "\"uptime_ms\":%lld}\n",
        ssid, host, c.port, user, c.listen_port,
        c.enabled ? "true" : "false", remote_link_has_key() ? "true" : "false",
        c.hostkey_fp[0] ? "true" : "false", fp, up);
  }
  return (n > 0 && static_cast<size_t>(n) < cap) ? n : 0;
}

int remote_link_json_pubkey(char *out, size_t cap, bool generated) {
  char line[200], fp[64];
  if (remote_link_pubkey_line(line, sizeof(line)) == 0 || remote_link_fingerprint(fp, sizeof(fp)) == 0) {
    const int n = snprintf(out, cap, "{\"type\":\"pubkey\",\"error\":\"no_key\",\"uptime_ms\":%lld}\n",
                           static_cast<long long>(esp_timer_get_time() / 1000));
    return (n > 0 && static_cast<size_t>(n) < cap) ? n : 0;
  }
  RemoteLinkConfig c;
  remote_link_load(c);
  char ak[260];
  if (c.listen_port)
    snprintf(ak, sizeof(ak), "restrict,port-forwarding,permitlisten=\\\"%u\\\" %s", c.listen_port, line);
  else
    snprintf(ak, sizeof(ak), "restrict,port-forwarding %s", line);
  const int n = snprintf(out, cap,
      "{\"type\":\"pubkey\",\"key\":\"%s\",\"fingerprint\":\"%s\","
      "\"authorized_keys_line\":\"%s\",\"generated\":%s%s,\"uptime_ms\":%lld}\n",
      line, fp, ak, generated ? "true" : "false",
      c.listen_port ? "" : ",\"warning\":\"listen_port not provisioned yet\"",
      static_cast<long long>(esp_timer_get_time() / 1000));
  return (n > 0 && static_cast<size_t>(n) < cap) ? n : 0;
}
