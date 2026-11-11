#pragma once

// ============================================================================
// SSH remote-forward tunnel — second ingress for the NDJSON wire protocol.
//
// The ESP32 opens ONE SSH session to a server and requests ONE remote port
// forward on the server's loopback. Every bridge that connects to that port
// (via its own `ssh -L`) lands here as a separate SSH channel; each channel
// is a full-duplex byte stream, exactly like USB-Serial-JTAG. Lines read from
// a channel are handed to the firmware's line handler together with an
// opaque SshClient* so replies (pong, configured, resync state) go back on
// the channel they came from — per-bridge, no cross-talk.
//
// Threading: all libssh2 I/O happens on the tunnel's own task. The line
// handler runs synchronously on that task, so calling ssh_tunnel_write()
// from inside the handler is safe; calling it from any other task is not.
// ============================================================================

#include <stddef.h>

struct SshClient;  // opaque; valid only for the duration of the handler call

using SshLineHandler = void (*)(const char *line, size_t len, SshClient *reply_to);

// Starts Wi-Fi + the tunnel task. No-op (logs once) when CONFIG_CC_SSH_ENABLE=n.
void ssh_tunnel_start(SshLineHandler handler);

// Write raw bytes back to the client a line arrived from. Only valid from
// inside the SshLineHandler callback (same task). Returns false if the
// channel is gone or the write could not complete within ~500 ms.
bool ssh_tunnel_write(SshClient *client, const char *buf, size_t n);

// True while the SSH session is up and the remote listener is bound.
bool ssh_tunnel_is_connected();

// Wi-Fi station state, for the status pixels.
enum class TunnelWifi { Off, Down, Associated, Up };
TunnelWifi ssh_tunnel_wifi_state();

// Number of bridges currently connected through the tunnel.
int ssh_tunnel_client_count();
