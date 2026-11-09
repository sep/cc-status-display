/*
 * fwtest — host-side mirror of cc-status-display's ssh_tunnel.cpp.
 *
 * Same libssh2 call sequence as the firmware, built against the same
 * libssh2 fork + mbedTLS 4 the firmware uses, so a pass here means the
 * crypto backend and our API usage are sound; what's left for hardware is
 * ESP-IDF's mbedTLS config, memory, and Wi-Fi.
 *
 *   fwtest <host> <port> <user> <pubkey-file> <privkey-file> <listen-port> <seconds>
 *
 * Sequence: TCP connect → handshake → log SHA256 host-key fingerprint →
 * publickey auth from memory → keepalive(15s, want reply) →
 * forward_listen("localhost", listen-port) → non-blocking service loop:
 * accept up to 4 channels, split each into lines, answer {"type":"ping"}
 * with a pong carrying the channel index, echo everything else back with a
 * "via" tag. Exits 0 after <seconds>, non-zero on any session failure.
 */
#include <libssh2.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 4
#define LINE_CAP 512

struct client {
    LIBSSH2_CHANNEL *ch;
    size_t len;
    char line[LINE_CAP];
};

static LIBSSH2_SESSION *g_session;
static LIBSSH2_LISTENER *g_listener;
static int g_sock = -1;
static struct client g_clients[MAX_CLIENTS];
static long g_lines = 0, g_pongs = 0, g_accepts = 0;

static const char *last_error(void)
{
    char *msg = NULL;
    int len = 0;
    if (!g_session) return "(no session)";
    libssh2_session_last_error(g_session, &msg, &len, 0);
    return msg ? msg : "(no message)";
}

static int is_fatal(int err)
{
    switch (err) {
    case LIBSSH2_ERROR_SOCKET_RECV:
    case LIBSSH2_ERROR_SOCKET_SEND:
    case LIBSSH2_ERROR_SOCKET_DISCONNECT:
    case LIBSSH2_ERROR_SOCKET_TIMEOUT:
    case LIBSSH2_ERROR_BAD_SOCKET:
    case LIBSSH2_ERROR_PROTO:
    case LIBSSH2_ERROR_DECRYPT:
    case LIBSSH2_ERROR_KEY_EXCHANGE_FAILURE:
        return 1;
    default:
        return 0;
    }
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { perror(path); exit(2); }
    buf[n] = '\0';
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static int tcp_connect(const char *host, const char *port)
{
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        fprintf(stderr, "dns: cannot resolve %s\n", host);
        return -1;
    }
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0 || connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        perror("connect");
        if (s >= 0) close(s);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return s;
}

static void fingerprint_hex(const char *hash, char *out)
{
    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hex[(hash[i] >> 4) & 0xf];
        out[i * 2 + 1] = hex[hash[i] & 0xf];
    }
    out[64] = '\0';
}

static int write_all(struct client *c, const char *buf, size_t n)
{
    size_t off = 0;
    int spins = 0;
    while (off < n) {
        ssize_t w = libssh2_channel_write(c->ch, buf + off, n - off);
        if (w == LIBSSH2_ERROR_EAGAIN) {
            if (++spins > 100) return -1;
            usleep(5000);
            continue;
        }
        if (w < 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

static void handle_line(struct client *c)
{
    int idx = (int)(c - g_clients);
    char out[LINE_CAP + 64];
    g_lines++;
    if (strstr(c->line, "\"type\":\"ping\"") || strstr(c->line, "\"type\": \"ping\"")) {
        g_pongs++;
        snprintf(out, sizeof(out), "{\"type\":\"pong\",\"via_channel\":%d,\"n\":%ld}\n", idx, g_pongs);
    } else {
        snprintf(out, sizeof(out), "{\"type\":\"echo\",\"via_channel\":%d,\"line\":\"%s\"}\n", idx, c->line);
    }
    if (write_all(c, out, strlen(out)) != 0)
        fprintf(stderr, "client %d: write failed: %s\n", idx, last_error());
}

static void close_client(struct client *c, const char *why)
{
    int idx = (int)(c - g_clients);
    if (!c->ch) return;
    fprintf(stderr, "client %d closed (%s)\n", idx, why);
    libssh2_session_set_blocking(g_session, 1);
    libssh2_session_set_timeout(g_session, 2000);
    libssh2_channel_free(c->ch);
    libssh2_session_set_blocking(g_session, 0);
    c->ch = NULL;
    c->len = 0;
}

static int service_once(void)
{
    LIBSSH2_CHANNEL *ch = libssh2_channel_forward_accept(g_listener);
    if (ch) {
        struct client *slot = NULL;
        for (int i = 0; i < MAX_CLIENTS; i++) if (!g_clients[i].ch) { slot = &g_clients[i]; break; }
        if (!slot) {
            fprintf(stderr, "refusing client: full\n");
            libssh2_session_set_blocking(g_session, 1);
            libssh2_channel_free(ch);
            libssh2_session_set_blocking(g_session, 0);
        } else {
            slot->ch = ch;
            slot->len = 0;
            g_accepts++;
            fprintf(stderr, "client %d connected\n", (int)(slot - g_clients));
        }
    } else {
        int err = libssh2_session_last_errno(g_session);
        if (is_fatal(err)) { fprintf(stderr, "accept: fatal %d: %s\n", err, last_error()); return 0; }
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = &g_clients[i];
        if (!c->ch) continue;
        char buf[256];
        for (;;) {
            ssize_t n = libssh2_channel_read(c->ch, buf, sizeof(buf));
            if (n > 0) {
                for (ssize_t k = 0; k < n; k++) {
                    char x = buf[k];
                    if (x == '\r') continue;
                    if (x == '\n') { c->line[c->len] = '\0'; handle_line(c); c->len = 0; }
                    else if (c->len < LINE_CAP - 1) c->line[c->len++] = x;
                    else c->len = 0;
                }
                continue;
            }
            if (n == LIBSSH2_ERROR_EAGAIN) break;
            if (n == 0) { if (libssh2_channel_eof(c->ch)) close_client(c, "eof"); break; }
            if (is_fatal((int)n)) { fprintf(stderr, "read: fatal %d: %s\n", (int)n, last_error()); return 0; }
            close_client(c, last_error());
            break;
        }
    }

    int next = 0;
    if (libssh2_keepalive_send(g_session, &next) != 0) {
        int err = libssh2_session_last_errno(g_session);
        if (is_fatal(err)) { fprintf(stderr, "keepalive: fatal %d: %s\n", err, last_error()); return 0; }
    }

    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(g_sock, &rf);
    struct timeval tv = {0, 100 * 1000};
    select(g_sock + 1, &rf, NULL, NULL, &tv);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 8) {
        fprintf(stderr, "usage: %s host port user pubkey privkey listen-port seconds\n", argv[0]);
        return 2;
    }
    const char *host = argv[1], *port = argv[2], *user = argv[3];
    int listen_port = atoi(argv[6]);
    int seconds = atoi(argv[7]);

    size_t publen, privlen;
    char *pub = slurp(argv[4], &publen);
    char *priv = slurp(argv[5], &privlen);

    if (libssh2_init(0) != 0) { fprintf(stderr, "libssh2_init failed\n"); return 1; }
    printf("libssh2 %s\n", libssh2_version(0));

    g_sock = tcp_connect(host, port);
    if (g_sock < 0) return 1;

    g_session = libssh2_session_init();
    if (!g_session) { fprintf(stderr, "session_init failed\n"); return 1; }
    if (getenv("FWTEST_TRACE"))
        libssh2_trace(g_session, LIBSSH2_TRACE_KEX | LIBSSH2_TRACE_AUTH | LIBSSH2_TRACE_CONN | LIBSSH2_TRACE_ERROR);
    libssh2_session_set_blocking(g_session, 1);
    libssh2_session_set_timeout(g_session, 20000);

    if (libssh2_session_handshake(g_session, g_sock) != 0) {
        fprintf(stderr, "handshake failed: %s\n", last_error());
        return 1;
    }
    {
        int hk_len = 0, hk_type = 0;
        libssh2_session_hostkey(g_session, (size_t *)&hk_len, &hk_type);
        const char *hash = libssh2_hostkey_hash(g_session, LIBSSH2_HOSTKEY_HASH_SHA256);
        char hex[65] = {0};
        if (hash) fingerprint_hex(hash, hex);
        printf("handshake OK; kex=%s hostkey=%s cipher=%s mac=%s\n",
               libssh2_session_methods(g_session, LIBSSH2_METHOD_KEX),
               libssh2_session_methods(g_session, LIBSSH2_METHOD_HOSTKEY),
               libssh2_session_methods(g_session, LIBSSH2_METHOD_CRYPT_CS),
               libssh2_session_methods(g_session, LIBSSH2_METHOD_MAC_CS));
        printf("host key SHA256 (hex) %s\n", hex);
    }

    /* FWTEST_FROMFILE=1 switches to the from-file API, which is what
       libssh2's own test-suite uses; the firmware uses from-memory. */
    int auth_rc;
    if (getenv("FWTEST_FROMFILE"))
        auth_rc = libssh2_userauth_publickey_fromfile(g_session, user, argv[4], argv[5], NULL);
    else
        auth_rc = libssh2_userauth_publickey_frommemory(g_session, user, strlen(user),
                                                        pub, publen, priv, privlen, NULL);
    if (auth_rc != 0) {
        fprintf(stderr, "publickey auth failed (%s): %s\n",
                getenv("FWTEST_FROMFILE") ? "fromfile" : "frommemory", last_error());
        return 1;
    }
    printf("authenticated as %s (%s)\n", user, getenv("FWTEST_FROMFILE") ? "fromfile" : "frommemory");

    libssh2_keepalive_config(g_session, 1, 15);

    int bound = 0;
    g_listener = libssh2_channel_forward_listen_ex(g_session, "localhost", listen_port, &bound, MAX_CLIENTS);
    if (!g_listener) {
        fprintf(stderr, "forward_listen failed: %s\n"
                        "  (sshd refused the request: most often localhost:%d is already held by "
                        "another session — a running or half-dead ESP32? `ss -ltnp | grep %d` (Linux) or "
                        "`sockstat -4 -l | grep %d` (FreeBSD) on the server. Otherwise check "
                        "permitlisten= / AllowTcpForwarding.)\n",
                last_error(), listen_port, listen_port, listen_port);
        return 1;
    }
    printf("remote forward listening on localhost:%d\n", bound);
    fflush(stdout);

    libssh2_session_set_blocking(g_session, 0);
    time_t deadline = time(NULL) + seconds;
    int ok = 1;
    while (time(NULL) < deadline) {
        if (!service_once()) { ok = 0; break; }
    }

    printf("summary: accepts=%ld lines=%ld pongs=%ld session_ok=%d\n", g_accepts, g_lines, g_pongs, ok);
    libssh2_session_set_blocking(g_session, 1);
    libssh2_session_set_timeout(g_session, 2000);
    for (int i = 0; i < MAX_CLIENTS; i++) if (g_clients[i].ch) libssh2_channel_free(g_clients[i].ch);
    libssh2_channel_forward_cancel(g_listener);
    libssh2_session_disconnect(g_session, "fwtest done");
    libssh2_session_free(g_session);
    close(g_sock);
    libssh2_exit();
    return ok ? 0 : 1;
}
