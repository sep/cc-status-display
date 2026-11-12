#pragma once

// ============================================================================
// Remote link — persistent configuration and device identity (wire v1.4).
//
// Everything the SSH tunnel needs to run lives in NVS namespace "tunnel",
// written over USB by the bridge's provisioning flow (FIRMWARE.md §8,
// "Remote link over SSH"). The private key is generated here, on the
// device, and never leaves it.
// ============================================================================

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

struct RemoteLinkConfig {
  char     ssid[33]        = {0};
  char     psk[65]         = {0};   // never echoed back
  char     host[254]       = {0};
  uint16_t port            = 22;
  char     user[33]        = {0};
  uint16_t listen_port     = 7777;
  bool     enabled         = false;
  char     hostkey_fp[64]  = {0};   // "SHA256:<b64>" or empty = unpinned

  bool wifi_complete() const { return ssid[0] != '\0'; }
  bool ssh_complete() const  { return host[0] && user[0] && port && listen_port; }
};

// Load the stored configuration. Fields never written keep their defaults.
void      remote_link_load(RemoteLinkConfig &out);
esp_err_t remote_link_save(const RemoteLinkConfig &cfg);

// Device identity (ECDSA P-256).
bool      remote_link_has_key();
// Generate (or, with replace=false, keep) the identity. *generated reports
// whether a new key was made.
esp_err_t remote_link_keygen(bool replace, bool *generated);
// Private key PEM (SEC1, named curve) for libssh2. Returns bytes written
// excluding NUL, or 0 if none stored.
size_t    remote_link_key_pem(char *out, size_t cap);
// "ecdsa-sha2-nistp256 AAAA… claudepanel-<mac>" and "SHA256:<b64>".
size_t    remote_link_pubkey_line(char *out, size_t cap);
size_t    remote_link_fingerprint(char *out, size_t cap);

// JSON helpers (write a full line incl. '\n'; return length or 0 on overflow).
int remote_link_json_provisioned(char *out, size_t cap, bool ok, const char *error);
int remote_link_json_pubkey(char *out, size_t cap, bool generated);

// Escape a string for embedding inside JSON quotes. Returns chars written.
size_t json_escape(const char *in, char *out, size_t cap);
