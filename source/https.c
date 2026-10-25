/*
 * https.c — HTTPS GET over mbedTLS (see https.h)
 *
 * Supports: https:// URLs, redirects (up to 5), Content-Length and chunked
 * bodies, certificate checking against the bundled roots.
 * Not supported: http://, proxies, compression, keep-alive.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <stdint.h>

#if __has_include("mbedtls/build_info.h")
#include "mbedtls/build_info.h"
#else
#include "mbedtls/config.h"
#endif
#include "mbedtls/version.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#if MBEDTLS_VERSION_MAJOR >= 3 && defined(MBEDTLS_PSA_CRYPTO_C)
#include "psa/crypto.h"
#endif

#include "https.h"
#include "ca_roots.h"
#include "version.h"

/* ── Platform layer ──────────────────────────────────────────────────────── */
#ifdef HOST_TEST
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <time.h>
static int plat_resolve(const char *host, uint32_t *addr_be) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return -1;
    *addr_be = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return 0;
}
static int plat_connect(uint32_t addr_be, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = addr_be;
    struct timeval tv = { 15, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) { close(s); return -1; }
    return s;
}
static int plat_send(int s, const void *b, size_t n) { return (int)send(s, b, n, 0); }
static int plat_recv(int s, void *b, size_t n)       { return (int)recv(s, b, n, 0); }
static void plat_close(int s)                        { close(s); }
static int plat_random(unsigned char *out, size_t len) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t n = fread(out, 1, len, f); fclose(f);
    return n == len ? 0 : -1;
}
#else
#include <psp2/net/net.h>
#include <psp2/kernel/processmgr.h>
#include <time.h>
#ifdef HAVE_RNG
extern int sceKernelGetRandomNumber(void *output, unsigned int size);
#endif

static int plat_resolve(const char *host, uint32_t *addr_be) {
    int rid = sceNetResolverCreate("update_dns", NULL, 0);
    if (rid < 0) return rid;
    SceNetInAddr a;
    int r = sceNetResolverStartNtoa(rid, host, &a, 5 * 1000 * 1000, 3, 0);   /* timeout in microseconds */
    sceNetResolverDestroy(rid);
    if (r < 0) return r;
    *addr_be = a.s_addr;
    return 0;
}
static int plat_connect(uint32_t addr_be, int port) {
    int s = sceNetSocket("update_tls", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (s < 0) return s;
    int us = 20 * 1000 * 1000;                         /* 20 s, microseconds */
    sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &us, sizeof(us));
    sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &us, sizeof(us));
    SceNetSockaddrIn sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_len    = sizeof(sa);
    sa.sin_family = SCE_NET_AF_INET;
    sa.sin_port   = sceNetHtons((unsigned short)port);
    sa.sin_addr.s_addr = addr_be;
    int r = sceNetConnect(s, (SceNetSockaddr *)&sa, sizeof(sa));
    if (r < 0) { sceNetSocketClose(s); return r; }
    return s;
}
static int plat_send(int s, const void *b, size_t n) { return sceNetSend(s, b, n, 0); }
static int plat_recv(int s, void *b, size_t n)       { return sceNetRecv(s, b, n, 0); }
static void plat_close(int s)                        { sceNetSocketClose(s); }

/* Returns 0 when real hardware randomness was used, 1 for the weak fallback. */
static int plat_random(unsigned char *out, size_t len) {
#ifdef HAVE_RNG
    if (sceKernelGetRandomNumber(out, (unsigned int)len) >= 0) return 0;
#endif
    /* Fallback: mix clocks. Much weaker; only used if the SDK has no RNG. */
    static uint64_t x = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < len; i++) {
        x ^= sceKernelGetProcessTimeWide();
        x ^= (uint64_t)time(NULL) << 17;
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        out[i] = (unsigned char)(x >> 33);
    }
    return 1;
}
#endif

/* ── pthread stand-ins (only when the SDK has no libpthread) ───────────────
 * mbedTLS locks a mutex around some shared state. The update screen runs on
 * one thread, so these simply report success. */
#ifdef VITA_PTHREAD_STUBS
int pthread_mutex_init(void *m, const void *attr)  { (void)m; (void)attr; return 0; }
int pthread_mutex_lock(void *m)                    { (void)m; return 0; }
int pthread_mutex_unlock(void *m)                  { (void)m; return 0; }
int pthread_mutex_destroy(void *m)                 { (void)m; return 0; }
#endif

/* ── State ───────────────────────────────────────────────────────────────── */
#define ERR_CANCELLED     (-2)
#define MAX_REDIRECTS     5
#define HDR_MAX           8192
#define URL_MAX           1100

static char g_detail[220];
static int  g_insecure = 0;
static int  g_clock_warn = 0;

const char *https_detail(void)      { return g_detail; }
void https_set_insecure(int on)     { g_insecure = on; }
int  https_clock_warning(void)      { return g_clock_warn; }

static int make_err(int stage, int code) {
    unsigned c = (unsigned)(code < 0 ? -code : code) & 0xFFFFu;
    return (int)(0xE1000000u | ((unsigned)stage << 16) | c);
}

static void set_detail_mbed(int ret) {
#if defined(MBEDTLS_ERROR_C)
    char tmp[120];
    mbedtls_strerror(ret, tmp, sizeof(tmp));
    snprintf(g_detail, sizeof(g_detail), "%s", tmp);
#else
    snprintf(g_detail, sizeof(g_detail), "mbedTLS error -0x%04X", (unsigned)-ret);
#endif
}

/* ── URL parsing ─────────────────────────────────────────────────────────── */
typedef struct { char host[128]; int port; char path[URL_MAX]; } Url;

static int parse_url(const char *u, Url *out) {
    if (strncasecmp(u, "https://", 8) != 0) return -1;
    u += 8;
    const char *slash = u;
    while (*slash && *slash != '/' && *slash != '?') slash++;
    size_t hl = (size_t)(slash - u);
    if (hl == 0 || hl >= sizeof(out->host)) return -1;
    memcpy(out->host, u, hl); out->host[hl] = '\0';
    out->port = 443;
    char *colon = strchr(out->host, ':');
    if (colon) { *colon = '\0'; out->port = atoi(colon + 1); if (out->port <= 0) return -1; }
    if (*slash == '\0')        snprintf(out->path, sizeof(out->path), "/");
    else if (*slash == '?')    snprintf(out->path, sizeof(out->path), "/%s", slash);
    else                       snprintf(out->path, sizeof(out->path), "%s", slash);
    return 0;
}

/* ── mbedTLS plumbing ────────────────────────────────────────────────────── */
typedef struct {
    int sock;
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt         ca;
    int ssl_ready;
} Conn;

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int r = plat_send(*(int *)ctx, buf, len);
    return r < 0 ? -0x004E /* send failed / timeout */ : r;
}
static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    int r = plat_recv(*(int *)ctx, buf, len);
    if (r < 0) return -0x004C;                     /* recv failed / timeout */
    return r;                                      /* 0 = peer closed */
}

/* Entropy for the random generator comes straight from the Vita (not from
 * mbedTLS's own platform sources, which expect /dev/urandom). */
static int drbg_entropy(void *data, unsigned char *out, size_t len) {
    (void)data;
    plat_random(out, len);
    return 0;
}

static void conn_free(Conn *c) {
    if (c->ssl_ready) mbedtls_ssl_close_notify(&c->ssl);
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_ctr_drbg_free(&c->drbg);
    mbedtls_x509_crt_free(&c->ca);
    if (c->sock >= 0) plat_close(c->sock);
    free(c);
}

static void describe_cert_problem(Conn *c, uint32_t flags) {
    char vinfo[120] = "";
#if defined(MBEDTLS_X509_CRT_PARSE_C) && !defined(MBEDTLS_X509_REMOVE_INFO)
    mbedtls_x509_crt_verify_info(vinfo, sizeof(vinfo), "", flags);
    for (char *p = vinfo; *p; p++) if (*p == '\n') *p = ' ';
#else
    snprintf(vinfo, sizeof(vinfo), "verify flags 0x%X", (unsigned)flags);
#endif
    char iss[80] = "";
#if !defined(MBEDTLS_X509_REMOVE_INFO)
    const mbedtls_x509_crt *top = mbedtls_ssl_get_peer_cert(&c->ssl);
    while (top && top->next) top = top->next;
    if (top) mbedtls_x509_dn_gets(iss, sizeof(iss), &top->issuer);
#endif
    snprintf(g_detail, sizeof(g_detail), "%.100s | issuer: %.70s", vinfo, iss);
}

/* Opens the socket and finishes the TLS handshake. */
static int conn_open(Conn **out, const Url *u) {
    *out = NULL;
    Conn *c = (Conn *)calloc(1, sizeof(Conn));
    if (!c) return make_err(HTTPS_STAGE_TLS, 3);
    c->sock = -1;
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_ctr_drbg_init(&c->drbg);
#if MBEDTLS_VERSION_MAJOR >= 3 && defined(MBEDTLS_PSA_CRYPTO_C)
    psa_crypto_init();
#endif
    mbedtls_x509_crt_init(&c->ca);

    uint32_t addr = 0;
    int r = plat_resolve(u->host, &addr);
    if (r < 0) { snprintf(g_detail, sizeof(g_detail), "DNS lookup failed for %s (0x%08X)", u->host, (unsigned)r);
                 conn_free(c); return make_err(HTTPS_STAGE_DNS, r); }

    c->sock = plat_connect(addr, u->port);
    if (c->sock < 0) { snprintf(g_detail, sizeof(g_detail), "Could not connect to %s (0x%08X)", u->host, (unsigned)c->sock);
                       r = c->sock; c->sock = -1; conn_free(c); return make_err(HTTPS_STAGE_CONNECT, r); }

    {
        static const char pers[] = "vita_recovery_update";
        r = mbedtls_ctr_drbg_seed(&c->drbg, drbg_entropy, NULL,
                                  (const unsigned char *)pers, sizeof(pers) - 1);
    }
    if (r != 0) { set_detail_mbed(r); conn_free(c); return make_err(HTTPS_STAGE_RNG, r); }

    int bad = 0;
    for (size_t i = 0; i < CA_ROOT_COUNT; i++) {
        const char *pem = CA_ROOT_PEMS[i];
        if (mbedtls_x509_crt_parse(&c->ca, (const unsigned char *)pem, strlen(pem) + 1) != 0) bad++;
    }
    (void)bad;   /* a root we cannot parse is simply not trusted */
#ifdef HOST_TEST
    if (getenv("VITA_TEST_CA")) mbedtls_x509_crt_parse_file(&c->ca, getenv("VITA_TEST_CA"));
#endif

    r = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
    if (r != 0) { set_detail_mbed(r); conn_free(c); return make_err(HTTPS_STAGE_TLS, r); }
#if MBEDTLS_VERSION_MAJOR < 3
    mbedtls_ssl_conf_min_version(&c->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);   /* TLS 1.2+ */
#endif
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
    mbedtls_ssl_conf_authmode(&c->conf, g_insecure ? MBEDTLS_SSL_VERIFY_NONE
                                                   : MBEDTLS_SSL_VERIFY_OPTIONAL);
    r = mbedtls_ssl_setup(&c->ssl, &c->conf);
    if (r != 0) { set_detail_mbed(r); conn_free(c); return make_err(HTTPS_STAGE_TLS, r); }
    mbedtls_ssl_set_hostname(&c->ssl, u->host);
    mbedtls_ssl_set_bio(&c->ssl, &c->sock, bio_send, bio_recv, NULL);

    while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
            set_detail_mbed(r);
            if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED)
                describe_cert_problem(c, mbedtls_ssl_get_verify_result(&c->ssl));
            conn_free(c);
            return make_err(HTTPS_STAGE_TLS, r);
        }
    }
    c->ssl_ready = 1;

    if (!g_insecure) {
        uint32_t fl = mbedtls_ssl_get_verify_result(&c->ssl);
        uint32_t date_only = MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE;
        if (fl & date_only) g_clock_warn = 1;      /* Vita clock probably unset */
        if (fl & ~date_only) {
            describe_cert_problem(c, fl);
            conn_free(c);
            return make_err(HTTPS_STAGE_CERT, (int)(fl & 0xFFFF));
        }
    }
    *out = c;
    return 0;
}

static int conn_write_all(Conn *c, const char *s, size_t n) {
    size_t off = 0;
    while (off < n) {
        int r = mbedtls_ssl_write(&c->ssl, (const unsigned char *)s + off, n - off);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r < 0) { set_detail_mbed(r); return make_err(HTTPS_STAGE_SEND, r); }
        off += (size_t)r;
    }
    return 0;
}

/* Returns bytes read, 0 on clean end of data, or a negative mbedTLS code. */
static int conn_read(Conn *c, unsigned char *buf, size_t len) {
    for (;;) {
        int r = mbedtls_ssl_read(&c->ssl, buf, len);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF) return 0;
        return r;
    }
}

/* ── Chunked decoder ─────────────────────────────────────────────────────── */
typedef enum { CK_SIZE, CK_EXT, CK_SIZE_LF, CK_DATA, CK_DATA_CR, CK_DATA_LF, CK_TRAILER, CK_DONE } CkState;
typedef struct { CkState st; uint64_t left; int saw_digit; } Chunk;

/* Feeds raw bytes; delivers decoded data through cb. Returns 0, 1 = finished, <0 error */
static int chunk_feed(Chunk *k, const uint8_t *p, size_t n, https_data_fn cb, void *ctx, uint64_t total) {
    while (n > 0 && k->st != CK_DONE) {
        switch (k->st) {
        case CK_SIZE: {
            uint8_t ch = *p;
            int d = -1;
            if (ch >= '0' && ch <= '9') d = ch - '0';
            else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
            if (d >= 0) { k->left = k->left * 16 + (uint64_t)d; k->saw_digit = 1; p++; n--; }
            else if (ch == ';') { k->st = CK_EXT; p++; n--; }
            else if (ch == '\r') { k->st = CK_SIZE_LF; p++; n--; }
            else return -1;
            break; }
        case CK_EXT:
            if (*p == '\r') k->st = CK_SIZE_LF;
            p++; n--; break;
        case CK_SIZE_LF:
            if (*p != '\n' || !k->saw_digit) return -1;
            p++; n--;
            k->saw_digit = 0;
            k->st = (k->left == 0) ? CK_TRAILER : CK_DATA;
            break;
        case CK_DATA: {
            size_t take = (k->left < n) ? (size_t)k->left : n;
            if (cb(ctx, p, take, total)) return ERR_CANCELLED;
            k->left -= take; p += take; n -= take;
            if (k->left == 0) k->st = CK_DATA_CR;
            break; }
        case CK_DATA_CR:
            if (*p != '\r') return -1;
            p++; n--; k->st = CK_DATA_LF; break;
        case CK_DATA_LF:
            if (*p != '\n') return -1;
            p++; n--; k->st = CK_SIZE; k->left = 0; break;
        case CK_TRAILER:
            /* Ignore trailers; the stream is complete once the zero-size
             * chunk has been seen, so stop here. */
            k->st = CK_DONE; break;
        default: break;
        }
    }
    return (k->st == CK_DONE) ? 1 : 0;
}

/* ── Header helpers ──────────────────────────────────────────────────────── */
static int contains_ci(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, nl) == 0) return 1;
    return 0;
}

static int hdr_value(const char *hdrs, const char *name, char *out, size_t outsz) {
    size_t nl = strlen(name);
    const char *p = hdrs;
    while (*p) {
        const char *eol = strstr(p, "\r\n");
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        if (ll > nl && strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (v < p + ll && (*v == ' ' || *v == '\t')) v++;
            size_t vl = (size_t)(p + ll - v);
            if (vl >= outsz) vl = outsz - 1;
            memcpy(out, v, vl); out[vl] = '\0';
            return 1;
        }
        if (!eol) break;
        p = eol + 2;
    }
    return 0;
}

/* ── One request ─────────────────────────────────────────────────────────── */
/* Returns 0 ok, 1 = redirect (next_url filled), <0 error. */
static int do_request(const Url *u, const char *accept, https_data_fn cb, void *ctx,
                      char *next_url, size_t next_sz) {
    Conn *c = NULL;
    int r = conn_open(&c, u);
    if (r != 0) return r;

    char req[1600];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: PSVitaRecoveryMenu/" APP_VERSION "\r\n"
        "Accept: %s\r\n"
        "Accept-Encoding: identity\r\n"
        "Connection: close\r\n\r\n",
        u->path, u->host, accept ? accept : "*/*");
    if (n <= 0 || n >= (int)sizeof(req)) { conn_free(c); return make_err(HTTPS_STAGE_URL, 1); }
    r = conn_write_all(c, req, (size_t)n);
    if (r != 0) { conn_free(c); return r; }

    /* Read until the end of the headers */
    static char hdr[HDR_MAX + 1];
    size_t hlen = 0;
    char *end = NULL;
    while (!end) {
        if (hlen >= HDR_MAX) { snprintf(g_detail, sizeof(g_detail), "Response headers too large"); conn_free(c); return make_err(HTTPS_STAGE_PARSE, 2); }
        int got = conn_read(c, (unsigned char *)hdr + hlen, HDR_MAX - hlen);
        if (got < 0) { set_detail_mbed(got); conn_free(c); return make_err(HTTPS_STAGE_RECV, got); }
        if (got == 0) { snprintf(g_detail, sizeof(g_detail), "Connection closed before the response headers ended"); conn_free(c); return make_err(HTTPS_STAGE_RECV, 0); }
        hlen += (size_t)got;
        hdr[hlen] = '\0';
        end = strstr(hdr, "\r\n\r\n");
    }
    size_t head_len = (size_t)(end - hdr);
    const uint8_t *body0 = (const uint8_t *)end + 4;
    size_t body0_len = hlen - head_len - 4;
    *end = '\0';

    int status = 0;
    if (sscanf(hdr, "HTTP/%*d.%*d %d", &status) != 1) {
        snprintf(g_detail, sizeof(g_detail), "Bad HTTP status line"); conn_free(c); return make_err(HTTPS_STAGE_PARSE, 1);
    }

    char val[64];
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        char loc[URL_MAX];
        if (!hdr_value(hdr, "Location", loc, sizeof(loc))) {
            snprintf(g_detail, sizeof(g_detail), "Redirect without Location"); conn_free(c); return make_err(HTTPS_STAGE_PARSE, 3);
        }
        if (loc[0] == '/') {
            if (u->port == 443) snprintf(next_url, next_sz, "https://%s%s", u->host, loc);
            else                snprintf(next_url, next_sz, "https://%s:%d%s", u->host, u->port, loc);
        } else {
            snprintf(next_url, next_sz, "%s", loc);
        }
        conn_free(c);
        return 1;
    }
    if (status != 200) { conn_free(c); return -status; }

    int chunked = 0;
    uint64_t clen = 0; int have_len = 0;
    if (hdr_value(hdr, "Transfer-Encoding", val, sizeof(val)) && contains_ci(val, "chunked")) chunked = 1;
    if (hdr_value(hdr, "Content-Length", val, sizeof(val))) { clen = strtoull(val, NULL, 10); have_len = 1; }

    uint64_t total = (!chunked && have_len) ? clen : 0;
    uint64_t got_total = 0;
    Chunk ck = { CK_SIZE, 0, 0 };
    int result = 0;

    /* bytes that arrived together with the headers, then the rest */
    static unsigned char rb[8192];
    const uint8_t *cur = body0; size_t curn = body0_len;
    for (;;) {
        if (curn > 0) {
            if (chunked) {
                int k = chunk_feed(&ck, cur, curn, cb, ctx, 0);
                if (k == ERR_CANCELLED) { result = ERR_CANCELLED; break; }
                if (k < 0) { snprintf(g_detail, sizeof(g_detail), "Bad chunked encoding"); result = make_err(HTTPS_STAGE_PARSE, 4); break; }
                if (k == 1) break;
            } else {
                size_t take = curn;
                if (have_len && got_total + take > clen) take = (size_t)(clen - got_total);
                if (take > 0 && cb(ctx, cur, take, total)) { result = ERR_CANCELLED; break; }
                got_total += take;
                if (have_len && got_total >= clen) break;
            }
        }
        int got = conn_read(c, rb, sizeof(rb));
        if (got < 0) { set_detail_mbed(got); result = make_err(HTTPS_STAGE_RECV, got); break; }
        if (got == 0) {                                   /* end of stream */
            if (chunked || (have_len && got_total < clen)) {
                snprintf(g_detail, sizeof(g_detail), "Connection closed early (%llu of %llu bytes)",
                         (unsigned long long)got_total, (unsigned long long)clen);
                result = make_err(HTTPS_STAGE_RECV, 1);
            }
            break;
        }
        cur = rb; curn = (size_t)got;
    }
    conn_free(c);
    return result;
}

int https_get(const char *url, const char *accept, https_data_fn cb, void *ctx) {
    char cur[URL_MAX], next[URL_MAX];
    snprintf(cur, sizeof(cur), "%s", url);
    g_detail[0] = '\0';
    g_clock_warn = 0;

    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        Url u;
        if (parse_url(cur, &u) != 0) {
            snprintf(g_detail, sizeof(g_detail), "Only https:// links are supported: %.100s", cur);
            return make_err(HTTPS_STAGE_URL, 2);
        }
        int r = do_request(&u, accept, cb, ctx, next, sizeof(next));
        if (r == 1) { snprintf(cur, sizeof(cur), "%s", next); continue; }
        return r;
    }
    snprintf(g_detail, sizeof(g_detail), "Too many redirects");
    return make_err(HTTPS_STAGE_PARSE, 5);
}
