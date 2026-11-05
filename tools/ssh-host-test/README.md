# ssh-host-test — exercise the firmware's SSH path without hardware

`fwtest.c` is a host-side mirror of `main/ssh_tunnel.cpp`: the same libssh2
call sequence (handshake → host-key hash → publickey auth from memory →
keepalive → `forward_listen` → non-blocking accept/read/write loop with
per-channel replies), built against the **same libssh2 fork and an mbedTLS 4
release**, so it validates the crypto backend and our API usage. What it
cannot validate is ESP-IDF's mbedTLS configuration, memory, or Wi-Fi — that
is what the board is for.

## Build (Linux / WSL; needs gcc, cmake, curl)

```sh
S=$PWD/.ssh-host-test; mkdir -p $S && cd $S

# mbedTLS 4.2.0 (the version the fork's own CI targets)
curl -sSL -o mbedtls.tar.bz2 https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-4.2.0/mbedtls-4.2.0.tar.bz2
echo "2bed9d713b4668f76553b097e72b8aa30bc8f112a940d7ae228d524bbde6ffea  mbedtls.tar.bz2" | sha256sum -c
tar xjf mbedtls.tar.bz2
cmake -S mbedtls-4.2.0 -B b-mbedtls -DCMAKE_INSTALL_PREFIX=$S/prefix -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF
cmake --build b-mbedtls -j && cmake --install b-mbedtls

# the fork, same commit as the firmware submodule
F=$(git -C "$(git rev-parse --show-toplevel)" rev-parse --show-toplevel)/components/libssh2/libssh2
cmake -S $F -B b-libssh2 -DCRYPTO_BACKEND=mbedTLS -DCMAKE_PREFIX_PATH=$S/prefix -DBUILD_SHARED_LIBS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=ON
cmake --build b-libssh2 -j

gcc -O1 -Wall -o fwtest "$(git rev-parse --show-toplevel)"/tools/ssh-host-test/fwtest.c \
    -I$F/include -I$S/prefix/include \
    b-libssh2/src/libssh2.a $S/prefix/lib/libmbedcrypto.a $S/prefix/lib/libtfpsacrypto.a $S/prefix/lib/libmbedcrypto.a
```

## Key format matters (read before generating a key)

Generate the device key **without** `-m PEM`:

```sh
ssh-keygen -t ecdsa -b 256 -f id_ecdsa -N "" -C fwtest
```

libssh2's mbedTLS backend (upstream and the fork alike) reads ECDSA keys in
the OpenSSH container format on every platform. The other formats are a
minefield, and a key that parses as a *public* key is accepted by the server
and then dies inside the signing callback with the unhelpful

```
publickey auth failed: Callback returned error
```

Tested 2026-11-05 against mbedTLS 4.2.0 with the fork, from memory and from file:

| Private key | Result |
|---|---|
| ECDSA P-256, OpenSSH format (`ssh-keygen` default) | ✅ works everywhere |
| ECDSA P-256, PEM with **named curve** (Linux `ssh-keygen -m PEM`) | ✅ |
| ECDSA P-256, PEM with **explicit parameters** (what `ssh-keygen -m PEM` emits on macOS, per field report) | ❌ "Unable to initialize private key" |
| RSA, PEM (`-m PEM`) | ✅ |
| RSA, OpenSSH format | ❌ the backend has no OpenSSH-format RSA reader |
| ed25519, any format | ❌ the backend has no ed25519 |

**macOS note.** `-m PEM` there produces an explicit-parameter EC key, which
mbedTLS refuses. Either regenerate without `-m PEM` (preferred), or convert
the key you have into named-curve form:

```sh
openssl ec -in id_ecdsa -param_enc named_curve -out id_ecdsa.named && mv id_ecdsa.named id_ecdsa
```

The public key is unchanged by the conversion, so `authorized_keys` needs no
edit. To see what you have: `head -1 id_ecdsa`, and for an `EC PRIVATE KEY`
file, `openssl ec -in id_ecdsa -text -noout | head` prints `ASN1 OID:
prime256v1` for named-curve keys and a long parameter dump for explicit ones.
An OpenSSH-format RSA key converts with `ssh-keygen -p -m PEM -f id_rsa -N ""`.

`FWTEST_FROMFILE=1` switches `fwtest` to the from-file auth API (what
libssh2's own tests use) if you ever need to split a key problem from a
from-memory problem; `FWTEST_TRACE=1` prints libssh2's auth trace when the
library was built with `-DENABLE_DEBUG_LOGGING=ON`.

## Run against your real VM (closest to the firmware's situation)

```sh
ssh-keygen -t ecdsa -b 256 -f id_ecdsa -N "" -C fwtest
# add to the VM's authorized_keys:
#   restrict,port-forwarding,permitlisten="7777" <contents of id_ecdsa.pub>
./fwtest <vm-host> 22 claudepanel id_ecdsa.pub id_ecdsa 7777 60
# in another terminal, on a laptop:
ssh -N -L 7777:localhost:7777 claudepanel@<vm-host> &
printf '{"type":"ping","seq":1}\n' | nc -q 2 localhost 7777      # expect a pong
```

Open two such tunnels at once: each `nc` must get its own pongs, tagged with a
different `via_channel`.

## Run libssh2's own test suite without docker

libssh2's harness normally starts an OpenSSH server in docker. Without docker
access, run an unprivileged sshd on a high port with the harness's keys (the
`openssh-server` package can be downloaded and unpacked with `dpkg-deb -x`
without installing it), then:

```sh
OPENSSH_NO_DOCKER=1 OPENSSH_SERVER_HOST=127.0.0.1 OPENSSH_SERVER_PORT=4711 ctest --test-dir b-libssh2 -j4
```

Expect four failures that are the environment, not crypto — password and
keyboard-interactive (no `my test password` for your user), agent forwarding,
and one certificate test whose principal is the docker image's `libssh2`
user. Everything else, including every cipher/MAC/host-key/cert variant and
all RSA + ECDSA key formats, should pass. See SPIKE-SSH.md for the run
record.
