#pragma once

// ============================================================================
// SSH remote-forward tunnel — second ingress for the NDJSON wire protocol
// (FIRMWARE.md §8, "Remote link over SSH (v1.4)").
//
// The ESP32 opens ONE SSH session to the provisioned server and requests ONE
// remote port forward on the server's loopback. Every bridge that connects
// to that port (via its own `ssh -L`) lands here as a separate SSH channel;
// each channel is a full-duplex byte stream, exactly like USB-Serial-JTAG.
// Lines read from a channel go to the firmware's line handler together with
// an opaque SshClient* so replies go back on the channel they came from.
//
// Configuration and identity come from NVS (remote_link.h); nothing is
// compiled in. ssh_tunnel_apply_config() re-reads NVS and restarts whatever
// needs restarting.
//
// Threading: all libssh2 I/O happens on the tunnel's own task. The line
// handler runs synchronously on that task, so calling ssh_tunnel_write()
// from inside the handler is safe; calling it from any other task is not.
// Unsolicited events (hostkey_seen) are delivered through the event sink,
// also from the tunnel task — the sink must be thread-safe.
// ============================================================================

#include <stddef.h>

struct SshClient;  // opaque; valid only for the duration of the handler call

using SshLineHandler = void (*)(const char *line, size_t len, SshClient *reply_to);
using SshEventSink   = void (*)(const char *json_line, size_t len);

enum class TunnelWifi { Off, Down, Associated, Up };
enum class TunnelSsh  { Off, WaitingWifi, Connecting, HostkeyMismatch, AuthFailed,
                        ForwardRefused, Listening, Down };

struct TunnelStatus {
  TunnelWifi wifi;
  TunnelSsh  ssh;
  int        clients;
  int        rssi;             // valid when wifi == Up/Associated
  char       ip[16];           // "" until an address is held
  char       mac[18];
  char       last_error[96];   // "" when none
  char       hostkey_fp[64];   // last host key seen, "" if never
  bool       hostkey_pinned;
  int        hostkey_match;    // -1 unknown/unpinned, 0 mismatch, 1 match
};

// Initialise Wi-Fi driver + tunnel task. Does nothing visible until NVS says
// enabled. No-op (logs once) when CONFIG_CC_REMOTE_LINK=n.
void ssh_tunnel_start(SshLineHandler handler, SshEventSink events);

// Re-read NVS and apply: join/leave Wi-Fi, (re)start or stop the SSH link.
// Safe to call from any task.
void ssh_tunnel_apply_config();

// Write raw bytes back to the client a line arrived from. Only valid from
// inside the SshLineHandler callback (same task).
bool ssh_tunnel_write(SshClient *client, const char *buf, size_t n);

bool       ssh_tunnel_is_connected();   // session up + remote listener bound
TunnelWifi ssh_tunnel_wifi_state();
TunnelSsh  ssh_tunnel_ssh_state();
int        ssh_tunnel_client_count();
void       ssh_tunnel_status(TunnelStatus &out);

const char *tunnel_wifi_name(TunnelWifi s);
const char *tunnel_ssh_name(TunnelSsh s);

// Full `tunnel_status` reply line (with '\n'); returns length or 0.
int ssh_tunnel_json_status(char *out, size_t cap);
// `"tunnel":{"wifi":…,"ssh":…,"clients":N},` fragment for pong (trailing
// comma included); returns length or 0.
int ssh_tunnel_json_summary(char *out, size_t cap);
