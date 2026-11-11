# Spike: SSH remote-forward tunnel (multi-bridge, no direct connectivity)

**Status: SPIKE CLOSED 2026-11-11 — thesis proven on hardware; productisation next.**
Hardware note: the PCB-antenna DevKitC clone could not hold the link from a
normal desk position in a metal-heavy office; an ESP32-S3-DevKitC-1U-N16R8
(same module, u.FL socket, external dipole) fixed it outright with no
firmware change. Recommend the 1U variant for any remote install.

**Original status line:** An ESP32-S3
(Lonely Binary N16R8) held an SSH session to the VM, bound a remote forward,
and served two laptops through it simultaneously, each as its own channel
with its own pong replies. What remains is productisation (NVS-stored
identity, `provision` message, bridge-side transport) plus the open checklist
items below.

## Why

Two laptops on a trusted VLAN, one display on an untrusted VLAN, firewall
blocks both directions. Both sides *can* dial out to a cloud VM over SSH.
One remote forward on that VM is a listener; a listener accepts any number
of connections; each becomes an SSH channel inside the ESP32's single
session. No MQTT, no certificates, no custom relay — "you need an SSH
account somewhere both things can reach".

```
 laptop A ──ssh -L 7777:localhost:7777──▶ ┐
                                          │ VM sshd  (loopback:7777) ◀──ssh -R (remote forward)── ESP32
 laptop B ──ssh -L 7777:localhost:7777──▶ ┘                  one session, N channels ───────────▶
```

## What changed

| Path | Change |
|---|---|
| `main/ssh_tunnel.{h,cpp}` | Wi-Fi station + one FreeRTOS task running libssh2: connect, host-key log/pin, publickey auth, keepalive, `forward_listen`, non-blocking accept/read loop. Lines go to the same `handle_line()` the serial reader uses. |
| `main/main.cpp` | `handle_line()` and every reply path now carry an `SshClient *to`; `write_json_line(to, …)` writes to that channel, or USB when `to` is null. One extra status pixel for the tunnel link. |
| `components/libssh2/` | libssh2 vendored as a git submodule (fork branch `mbedtls-4-psa`, see below), built on ESP-IDF's mbedTLS 4, BSD-3. Not the registry package. |
| `main/CMakeLists.txt` | Embeds `main/keys/id_ecdsa{,.pub}` at build time (empty placeholders if absent so CI builds). |
| `main/Kconfig.projbuild` | `ClaudePanel SSH tunnel (spike)` menu: enable flag, Wi-Fi, host/port/user, listen port, host-key pin, stack size. |
| `partitions.csv`, `sdkconfig.defaults` | 3 MB factory app (was 1 MB default). App offset unchanged, flasher manifest unaffected. |

## ESP-IDF version and libssh2 provenance

The branch builds on **ESP-IDF v6.0** (the repo's pin) with libssh2 from
<https://github.com/mike-rogers/libssh2> branch `mbedtls-4-psa` (pinned at
`3ae9b73`, 2026-11-05): upstream
master plus a PSA-based backend for mbedTLS 4 / TF-PSA-Crypto. History of
how we got here, each step confirmed by a CI run on this branch:

- libssh2 1.11.1 (the registry package) includes legacy mbedTLS headers
  that mbedTLS 4 removed → cannot compile on v6.0.
- Upstream libssh2 master's mbedTLS 4 path is unfinished and untested
  upstream (CI covers 3.1/3.6 only): compiled-out RNG context still passed
  at ~12 call sites, mbedTLS 3 `mbedtls_pk_parse_key` signature, and the
  `mbedtls_pk_rsa()` / `mbedtls_pk_ec()` accessors it uses no longer exist.
- Interim, the branch built on v5.5 (mbedTLS 3.6) to prove everything else.
- The fork branch replaces that with a real mbedTLS 4 backend; CI here is
  the first ESP-IDF build of it. Upstream tracking:
  <https://github.com/libssh2/libssh2/issues/2284> — once merged and
  released, move the submodule back to upstream and add it to MAINTENANCE.md.

## Key-type note

libssh2's mbedTLS backend does **RSA and ECDSA, not ed25519** — for both
the device identity and the server host key it will accept. OpenSSH
servers offer ECDSA and RSA host keys by default alongside ed25519, so
nothing to change server-side; just don't generate the device key as
ed25519. Generate it **without `-m PEM`**: the backend reads OpenSSH-format
ECDSA keys everywhere, whereas `-m PEM` on macOS emits explicit-parameter
EC keys that mbedTLS rejects, and OpenSSH-format RSA keys aren't readable
at all. Full matrix in `tools/ssh-host-test/README.md`.

## Server setup (once)

```sh
# on the VM
sudo useradd -m -s /usr/sbin/nologin claudepanel     # no shell needed; forwarding only
sudo -u claudepanel mkdir -p ~claudepanel/.ssh
sudo -u claudepanel chmod 700 ~claudepanel/.ssh
```

`/etc/ssh/sshd_config` defaults are fine (`AllowTcpForwarding yes`,
`GatewayPorts no`). If the VM's sshd was hardened, make sure forwarding is
allowed for this user. **Not optional:**

```
ClientAliveInterval 30
ClientAliveCountMax 3
```

A remote forward is a listener and sshd allows one per port. When the ESP32
drops off Wi-Fi without closing its session, sshd otherwise keeps the dead
session — and the port — until TCP gives up, which can be hours; every new
connection then fails with "Unable to complete request for forward-listen".
With ClientAlive the port frees within ~90 s. To see who holds it:
`ss -ltnp | grep 7777` (Linux) or `sockstat -4 -l | grep 7777` (FreeBSD);
`kill` that sshd child to free it at once. Reload sshd after editing the
config (`systemctl reload ssh` / `service sshd reload`).

`~claudepanel/.ssh/authorized_keys` gets one line per device, restricted
to exactly the forwarding it needs:

```
restrict,port-forwarding,permitlisten="7777" ecdsa-sha2-nistp256 AAAA...  claudepanel-esp32
restrict,port-forwarding,permitopen="localhost:7777" ssh-ed25519 AAAA...  laptop-a
restrict,port-forwarding,permitopen="localhost:7777" ssh-ed25519 AAAA...  laptop-b
```

(`restrict` disables pty/agent/X11/etc.; `port-forwarding` re-enables just
that; `permitlisten` limits what the ESP32 may bind, `permitopen` limits
what laptops may connect to.)

Note the server's host-key fingerprint for pinning later:

```sh
ssh-keygen -lf /etc/ssh/ssh_host_ecdsa_key.pub
```

## Device setup

```sh
# in the firmware checkout — key is gitignored and embedded at build time
mkdir -p main/keys
ssh-keygen -t ecdsa -b 256 -f main/keys/id_ecdsa -N "" -C claudepanel-esp32   # no -m PEM: see tools/ssh-host-test/README.md
cat main/keys/id_ecdsa.pub     # → paste into authorized_keys (line 1 above)

idf.py set-target esp32s3
# pick the board's overlay (PSRAM mode): sdkconfig.board.thing_plus or
# sdkconfig.board.lonely_binary_n16r8. Only read when sdkconfig is generated —
# `idf.py fullclean` first if you've built before.
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.board.lonely_binary_n16r8" menuconfig
#   → ClaudePanel SSH tunnel (spike): enable, SSID, PSK, host, user
idf.py -DBOARD_NAME=BOARD_LONELY_BINARY_N16R8_GOLD build flash monitor   # -D before the verb
```

Boot log to expect:

```
ssh-tunnel: wifi: joining '...'
ssh-tunnel: wifi up, ip 192.168.x.y
ssh-tunnel: connecting claudepanel@vm:22
ssh-tunnel: server host key SHA256:....
ssh-tunnel: host key not pinned (spike): accepting. Set CC_SSH_HOSTKEY_SHA256 to pin it.
ssh-tunnel: authenticated as 'claudepanel'
ssh-tunnel: remote forward listening on vm:localhost:7777
```

Once you've compared the fingerprint with the server's, put it in
`CC_SSH_HOSTKEY_SHA256` and rebuild; mismatches then refuse to auth.

## Laptop test (no bridge changes needed)

```sh
ssh -N -L 7777:localhost:7777 claudepanel@vm &      # tunnel A
nc localhost 7777
{"type":"ping","seq":1}
{"type":"state","client":"1","state":"working","subagent_count":2,"tasks_active":1,"tasks_completed":0}
```

Expect a `pong` back on **this** nc, and the panel to render *working*.
Open a second tunnel from the other laptop (or a second terminal) and
send `{"client":"2",...}` — it should drive panel 2 independently and
its pings should get pongs on its own connection only.

The bottom-left status row, left to right: x=2 bridge heartbeat (existing:
green = a line arrived in the last 10 s, over either transport), x=4 Wi-Fi
(green = IP acquired, amber = associated but no DHCP lease, red = down),
x=6 SSH (blue = listener bound, purple = connecting/retrying). The two
remote pixels are off when the tunnel is disabled.

## Host-side functional test of the SSH path (2026-11-03)

Done on a Linux host with `tools/ssh-host-test/` (see its README), against
OpenSSH 9.6 running unprivileged on localhost, with the fork built against
**mbedTLS 4.2.0** and, as an A/B control, mbedTLS 3.6.7:

- `fwtest` (mirror of `ssh_tunnel.cpp`): **pass**. ecdh-sha2-nistp256 kex,
  ecdsa-sha2-nistp256 certificate host key, chacha20-poly1305, hmac-sha2-256;
  ECDSA P-256 `-m PEM` key auth from memory; remote forward bound on 7777;
  two concurrent clients, 5 lines, 3 pongs, each reply on the originating
  channel; clean session teardown. Exit 0.
- libssh2 harness, 60 tests: **56 pass**, 8 of them self-skipped on
  algorithms this backend doesn't offer (3des, md5/sha1 MACs, ed25519/ssh-rsa
  cert host keys). 4 fail **identically on mbedTLS 3.6.7**, so they are the
  test environment: password + keyboard-interactive (docker image's
  password), agent forwarding, and an RSA cert whose only principal is the
  docker image's `libssh2` user (`sshd: Certificate invalid: name is not a
  listed principal`). RSA SHA2-512 signing itself passed in the plain-key
  tests.

Not covered by this, and still owed to hardware: ESP-IDF's mbedTLS
configuration (algorithm availability may differ from a desktop build),
memory with the HUB75 driver running, Wi-Fi, and keepalive behaviour over
hours.

## First hardware run (2026-11-05) and the memory budget

Observed on an ESP32-S3 on the SEP-IoT VLAN: association in < 1 s, **DHCP
lease after ~116 s** (the firmware was silent meanwhile; it now logs every
15 s while waiting, plus the STA MAC so the lease table can be checked),
TCP connect to the VM fine, then `libssh2_session_init()` returned NULL.

Cause: `LIBSSH2_SESSION` is ~80 KB in a single allocation (`buf[35000]`,
`outbuf[35000]`, `banner[8192]` — the SSH protocol requires handling
35 KB packets, so these aren't tunable). After Wi-Fi, lwIP, the HUB75 DMA
buffers and the 48 KB SSH task stack, internal RAM has no contiguous hole
that size.

Fix on the branch: PSRAM enabled in `sdkconfig.defaults` with the mode in a
per-board overlay — `sdkconfig.board.lonely_binary_n16r8` (8 MB octal) and
`sdkconfig.board.thing_plus` (the Thing Plus's ESP32-S3-MINI-1-N4R2 has
2 MB **quad** PSRAM, so this is *not* a Lonely-Binary-only feature) — and
libssh2 given an allocator
that prefers PSRAM and logs any failed request's size with the heap shape.
Also logged now: heap at boot / before session / after the listener binds,
and the SSH task's stack high-water mark, so the 48 KB stack can be trimmed
with data. **A board without PSRAM may not be able to run this feature at
all**; the heap log will say. Neither supported board is in that position.

Gotcha that cost a day: `SDKCONFIG_DEFAULTS` is only consulted when
`sdkconfig` is generated. An existing `sdkconfig` silently ignores new
defaults (the first hardware run had PSRAM "enabled" and `spiram=0`).
`idf.py fullclean` after pulling defaults changes or switching boards.

## Success criteria

- [x] Builds for both boards on ESP-IDF v6.0 with the fork's mbedTLS 4 backend
      (CI run 6, 2026-11-03; zero warnings from libssh2). Image 0xddab0 (≈887 KB)
      with the tunnel linked in vs 0x66cd0 (≈421 KB) without; 71 % of the 3 MB
      app partition free. (On v5.5 / mbedTLS 3.6 it had been ≈920 KB vs ≈446 KB.)
- [x] Session comes up, listener binds (hardware, Lonely Binary N16R8, 2026-11-09;
      a bridge-side ping came back as a pong through the tunnel). Idle-survival not yet timed.
- [x] Two concurrent connections, pongs don't cross-talk — **on hardware** (board as owner, two laptops via
      `ssh -L`, 2026-11-10). Also proven on host with `fwtest`.
- [ ] Session recovers after `sudo systemctl restart ssh` on the VM and after a Wi-Fi drop.
- [x] Free heap after connect: `heap @listening: internal=109971 spiram=8289628` on the N16R8 —
      the libssh2 session lives in PSRAM; internal RAM barely moved from boot.
- [ ] USB serial path still works unchanged.

## Known gaps (by design, for the spike)

- Identity and config are compile-time (Kconfig + embedded key). Production: NVS + a `provision` NDJSON message pushed from the bridge over USB, which also reads back the public key.
- Host key is trust-on-first-use unless pinned. Production: pin via the same provision flow.
- `configure` sent by two bridges with different layouts will fight; last writer wins. Decide: NVS wins over the tunnel, or require matching `panel_layout.json`.
- Cross-bridge slot collisions aren't flagged. The firmware now knows which channel each line came from, so it *can* — later.
- Spontaneous (non-reply) device→host messages would need broadcasting to every channel; nothing emits them today.
- Stack size: measured high-water mark on hardware was 45544 free of 49152 after handshake/auth/listen
  (< 4 KB used). Default trimmed to 16 KB; the firmware logs the mark once a minute while clients are
  connected. Thing Plus (2 MB quad PSRAM) still untested on hardware.
- libssh2 comes from a personal fork branch until the mbedTLS 4 backend is upstreamed and released; see "ESP-IDF version and libssh2 provenance".
