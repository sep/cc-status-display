# Spike: SSH remote-forward tunnel (multi-bridge, no direct connectivity)

**Status: spike.** Proves that the ESP32 can hold an SSH session to a
server, request a remote port forward, and accept several bridges through
it — each as its own channel, each getting its own replies. If this works
on hardware, the remaining work is productisation (NVS-stored identity,
`provision` message, bridge-side transport); if it doesn't, we learn that
before touching the bridge.

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
<https://github.com/mike-rogers/libssh2> branch `mbedtls-4-psa`: upstream
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
ed25519.

## Server setup (once)

```sh
# on the VM
sudo useradd -m -s /usr/sbin/nologin claudepanel     # no shell needed; forwarding only
sudo -u claudepanel mkdir -p ~claudepanel/.ssh
sudo -u claudepanel chmod 700 ~claudepanel/.ssh
```

`/etc/ssh/sshd_config` defaults are fine (`AllowTcpForwarding yes`,
`GatewayPorts no`). If the VM's sshd was hardened, make sure forwarding is
allowed for this user. Optional but nice: `ClientAliveInterval 30`.

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
ssh-keygen -t ecdsa -b 256 -m PEM -f main/keys/id_ecdsa -N "" -C claudepanel-esp32
cat main/keys/id_ecdsa.pub     # → paste into authorized_keys (line 1 above)

idf.py set-target esp32s3
idf.py menuconfig              # ClaudePanel SSH tunnel (spike) → enable, SSID, PSK, host, user
idf.py build flash monitor     # add -DBOARD_NAME=... for the Lonely Binary
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

The bottom-left status row: pixel 2 = bridge heartbeat (existing), pixel
4 = tunnel link (blue up / purple down).

## Success criteria

- [x] Builds for both boards on ESP-IDF v6.0 with the fork's mbedTLS 4 backend
      (CI run 6, 2026-11-03; zero warnings from libssh2). Image 0xddab0 (≈887 KB)
      with the tunnel linked in vs 0x66cd0 (≈421 KB) without; 71 % of the 3 MB
      app partition free. (On v5.5 / mbedTLS 3.6 it had been ≈920 KB vs ≈446 KB.)
- [ ] Session comes up, listener binds, survives 10+ minutes idle (keepalive).
- [ ] Two concurrent connections, pongs don't cross-talk.
- [ ] Session recovers after `sudo systemctl restart ssh` on the VM and after a Wi-Fi drop.
- [ ] Free heap after connect is comfortable (`esp_get_free_heap_size()` ≳ 100 KB with HUB75 running).
- [ ] USB serial path still works unchanged.

## Known gaps (by design, for the spike)

- Identity and config are compile-time (Kconfig + embedded key). Production: NVS + a `provision` NDJSON message pushed from the bridge over USB, which also reads back the public key.
- Host key is trust-on-first-use unless pinned. Production: pin via the same provision flow.
- `configure` sent by two bridges with different layouts will fight; last writer wins. Decide: NVS wins over the tunnel, or require matching `panel_layout.json`.
- Cross-bridge slot collisions aren't flagged. The firmware now knows which channel each line came from, so it *can* — later.
- Spontaneous (non-reply) device→host messages would need broadcasting to every channel; nothing emits them today.
- Stack size (48 KB) copied from upstream's example floor; tune down once real high-water marks are known.
- libssh2 comes from a personal fork branch until the mbedTLS 4 backend is upstreamed and released; see "ESP-IDF version and libssh2 provenance".
