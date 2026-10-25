/*
 * update.c — GitHub update checker / VPK downloader
 *
 *   1. GET https://api.github.com/repos/<owner>/<repo>/releases/latest
 *   2. Read tag_name and the first *.vpk asset (name, url, size, sha256).
 *   3. Compare the tag with APP_VERSION.
 *   4. On request, download the VPK to ux0:downloads/<name>, hashing it as
 *      it arrives, and compare with the SHA-256 that GitHub publishes for the
 *      asset ("digest" field) when there is one.
 *
 * The VPK is never installed or executed by this code.
 */

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/ctrl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdint.h>

#include "update.h"
#include "ftp_server.h"     /* net_ensure_init() */
#include "https.h"
#include "display.h"
#include "draw.h"
#include "menu.h"

/* ── Config ──────────────────────────────────────────────────────────────── */
#define GH_OWNER        "DrinkingSubset"
#define GH_REPO         "PS-Vita-Recovery-Menu-Project"
#define GH_API_URL      "https://api.github.com/repos/" GH_OWNER "/" GH_REPO "/releases/latest"
#define GH_PAGE_URL     "github.com/" GH_OWNER "/" GH_REPO "/releases"
#define USER_AGENT      "PSVitaRecoveryMenu/" APP_VERSION
#define DOWNLOAD_DIR    "ux0:downloads"
#define JSON_MAX        (128 * 1024)
#define CHUNK_SIZE      (64 * 1024)

#define ERR_CANCELLED   (-2)

static int g_insecure = 0;       /* user allowed skipping certificate checks */


/* ── SHA-256 (public-domain style, compact) ──────────────────────────────── */
typedef struct {
    uint32_t h[8];
    uint8_t  buf[64];
    uint64_t total;
    int      fill;
} Sha256;

static const uint32_t K256[64] = {
 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
 0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
 0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
 0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
 0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
 0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
 0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
 0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(Sha256 *s, const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_init(Sha256 *s) {
    static const uint32_t iv[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    memcpy(s->h, iv, sizeof(iv));
    s->total = 0;
    s->fill  = 0;
}

static void sha_update(Sha256 *s, const uint8_t *d, size_t n) {
    s->total += n;
    while (n > 0) {
        size_t take = 64 - s->fill;
        if (take > n) take = n;
        memcpy(s->buf + s->fill, d, take);
        s->fill += (int)take; d += take; n -= take;
        if (s->fill == 64) { sha_block(s, s->buf); s->fill = 0; }
    }
}

static void sha_final_hex(Sha256 *s, char out[65]) {
    uint64_t bits = s->total * 8;
    uint8_t pad = 0x80;
    sha_update(s, &pad, 1);
    pad = 0;
    while (s->fill != 56) sha_update(s, &pad, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, len, 8);
    for (int i = 0; i < 8; i++)
        snprintf(out + i * 8, 9, "%08x", (unsigned)s->h[i]);
    out[64] = '\0';
}

/* ── Tiny JSON helpers (enough for the GitHub release object) ────────────── */

/* Find "key": "value" between from..end and copy value into out. */
static int json_str(const char *from, const char *end, const char *key,
                    char *out, int outsz) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = from;
    size_t plen = strlen(pat);
    while (p < end) {
        const char *hit = strstr(p, pat);
        if (!hit || hit >= end) return 0;
        const char *q = hit + plen;
        while (q < end && (*q == ' ' || *q == ':' || *q == '\n' || *q == '\t')) q++;
        if (q < end && *q == '"') {
            q++;
            int n = 0;
            while (q < end && *q && *q != '"' && n < outsz - 1) {
                if (*q == '\\' && q + 1 < end) q++;
                out[n++] = *q++;
            }
            out[n] = '\0';
            return 1;
        }
        p = hit + plen;           /* key matched but value isn't a string */
    }
    return 0;
}

static int json_num(const char *from, const char *end, const char *key,
                    uint64_t *out) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *hit = strstr(from, pat);
    if (!hit || hit >= end) return 0;
    const char *q = hit + strlen(pat);
    while (q < end && (*q == ' ' || *q == ':')) q++;
    if (q >= end || *q < '0' || *q > '9') return 0;
    uint64_t v = 0;
    while (q < end && *q >= '0' && *q <= '9') v = v * 10 + (uint64_t)(*q++ - '0');
    *out = v;
    return 1;
}

/* ── Release info ────────────────────────────────────────────────────────── */
typedef struct {
    char     tag[40];
    char     name[96];
    char     url[512];
    char     digest[65];      /* hex sha256 from GitHub, "" if not provided */
    uint64_t size;
    int      have_asset;
} Release;

static int ends_with_vpk(const char *s) {
    size_t n = strlen(s);
    if (n < 4) return 0;
    const char *e = s + n - 4;
    return e[0] == '.' && (e[1] | 32) == 'v' && (e[2] | 32) == 'p' && (e[3] | 32) == 'k';
}

static int parse_release(const char *json, Release *r) {
    memset(r, 0, sizeof(*r));
    const char *end = json + strlen(json);
    if (!json_str(json, end, "tag_name", r->tag, sizeof(r->tag))) return -1;

    const char *p = json;
    while ((p = strstr(p, "\"browser_download_url\"")) != NULL) {
        char url[512];
        if (!json_str(p, end, "browser_download_url", url, sizeof(url))) break;
        if (ends_with_vpk(url)) {
            /* Asset object starts at its API url ".../releases/assets/<id>" */
            const char *seg = json, *s2 = json;
            while ((s2 = strstr(s2, "/releases/assets/")) != NULL && s2 < p) {
                seg = s2; s2 += 16;
            }
            const char *seg_end = strstr(p, "/releases/assets/");
            if (!seg_end) seg_end = end;

            snprintf(r->url, sizeof(r->url), "%s", url);
            json_str(seg, seg_end, "name", r->name, sizeof(r->name));
            json_num(seg, seg_end, "size", &r->size);
            char dg[96];
            if (json_str(seg, seg_end, "digest", dg, sizeof(dg)) &&
                strncmp(dg, "sha256:", 7) == 0 && strlen(dg + 7) == 64)
                snprintf(r->digest, sizeof(r->digest), "%s", dg + 7);
            if (!r->name[0]) {
                const char *slash = strrchr(url, '/');
                snprintf(r->name, sizeof(r->name), "%s", slash ? slash + 1 : "update.vpk");
            }
            r->have_asset = 1;
            break;
        }
        p += 20;
    }
    return 0;
}

/* "v1.2.3" / "1.2.3" -> 1*10000 + 2*100 + 3 ; -1 on parse failure */
static long version_value(const char *s) {
    if (*s == 'v' || *s == 'V') s++;
    int a = 0, b = 0, c = 0;
    if (sscanf(s, "%d.%d.%d", &a, &b, &c) < 2) return -1;
    return (long)a * 10000 + (long)b * 100 + (long)c;
}

/* ── HTTP (HTTPS via our own mbedTLS client, see https.c) ────────────────── */
typedef int (*ProgressFn)(uint64_t done, uint64_t total);

typedef struct {
    char        *mem;              /* sink 1: memory buffer */
    int          mem_cap;
    int          mem_len;
    int          fd;               /* sink 2: file (-1 = none) */
    Sha256      *sha;              /* hashed while writing to the file */
    ProgressFn   progress;
    uint64_t     done;
    int          io_err;
} Sink;

static int sink_cb(void *ctx, const uint8_t *d, size_t n, uint64_t total) {
    Sink *sk = (Sink *)ctx;
    if (sk->mem) {
        int room = sk->mem_cap - 1 - sk->mem_len;
        int take = (int)n < room ? (int)n : room;
        if (take > 0) { memcpy(sk->mem + sk->mem_len, d, (size_t)take); sk->mem_len += take; }
        sk->mem[sk->mem_len] = '\0';
    }
    if (sk->fd >= 0) {
        int w = sceIoWrite(sk->fd, d, (SceSize)n);
        if (w != (int)n) { sk->io_err = (w < 0) ? w : -4; return 1; }
        if (sk->sha) sha_update(sk->sha, d, n);
    }
    sk->done += n;
    if (sk->progress && sk->progress(sk->done, total)) return 1;
    return 0;
}

/* Returns 0 on success, -HTTP_STATUS for a bad status, or a 0xE1xxxxxx code. */
static int http_fetch(const char *url, Sink *sk, int accept_json) {
    https_set_insecure(g_insecure);
    sk->done = 0;
    sk->io_err = 0;
    int r = https_get(url, accept_json ? "application/vnd.github+json" : "*/*",
                      sink_cb, sk);
    if (r == ERR_CANCELLED && sk->io_err) return sk->io_err;
    return r;
}

/* ── Screen state ────────────────────────────────────────────────────────── */
typedef enum {
    PH_CHECKING, PH_RESULT, PH_DOWNLOADING, PH_DONE, PH_ERROR
} Phase;

static Phase   g_phase;
static Release g_rel;
static int     g_newer;            /* 1 = newer, 0 = same, -1 = older/unknown */
static char    g_err[256];
static char    g_saved_path[160];
static char    g_hash_msg[96];
static int     g_hash_ok;          /* 1 verified, 0 not verified, -1 mismatch */
static uint64_t g_dl_done, g_dl_total;
static SceCtrlData g_old;

static void fmt_err(int r, const char *what) {
    if (r == ERR_CANCELLED)
        snprintf(g_err, sizeof(g_err), "%s cancelled.", what);
    else if (r < 0 && r > -1000)
        snprintf(g_err, sizeof(g_err), "%s failed: HTTP %d", what, -r);
    else
        snprintf(g_err, sizeof(g_err), "%s failed: 0x%08X", what, (unsigned)r);
}

static void frame_begin(const char *subtitle, uint32_t subcolor) {
    display_start();
    display_clear(COLOR_BG);
    display_rect(0, 0, SCREEN_W, TITLE_LINE, COLOR_TITLE_BG);
    display_app_title(6);
    display_text(MENU_X, 26, subcolor, subtitle);
    display_hline(0, TITLE_LINE, SCREEN_W, COLOR_GREEN);
}

static void frame_end(const char *footer) {
    display_rect(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, COLOR_TITLE_BG);
    display_hline(0, FOOTER_Y, SCREEN_W, COLOR_GREEN);
    display_text(MENU_X, FOOTER_Y + 7, COLOR_DIM, footer);
    display_end();
}

static void draw_screen(void) {
    int y = TITLE_H + 20;
    char buf[160];

    switch (g_phase) {
    case PH_CHECKING:
        frame_begin("Check for Updates", COLOR_YELLOW);
        display_text(MENU_X, y, COLOR_TEXT, "Contacting GitHub...");
        frame_end("Please wait");
        return;

    case PH_RESULT: {
        frame_begin("Check for Updates", COLOR_YELLOW);
        display_text(MENU_X, y, COLOR_DIM, "Installed:");
        display_text(VAL_X, y, COLOR_TEXT, "v" APP_VERSION);
        y += LINE_H;
        display_text(MENU_X, y, COLOR_DIM, "Latest release:");
        display_text(VAL_X, y, COLOR_TEXT, g_rel.tag);
        y += LINE_H + 10;

        if (https_clock_warning())
            display_text(MENU_X, y + LINE_H * 9, COLOR_YELLOW,
                         "Note: the Vita clock looks wrong; date check skipped.");
        if (g_newer > 0)
            display_text(MENU_X, y, COLOR_GREEN,  "A newer version is available!");
        else if (g_newer == 0)
            display_text(MENU_X, y, COLOR_GREEN,  "You are up to date.");
        else
            display_text(MENU_X, y, COLOR_YELLOW, "Installed version is newer than the release.");
        y += LINE_H + 10;

        if (g_rel.have_asset) {
            display_text(MENU_X, y, COLOR_DIM, "File:");
            display_text(VAL_X, y, COLOR_TEXT, g_rel.name);
            y += LINE_H;
            if (g_rel.size) {
                snprintf(buf, sizeof(buf), "%llu KB", (unsigned long long)(g_rel.size / 1024));
                display_text(MENU_X, y, COLOR_DIM, "Size:");
                display_text(VAL_X, y, COLOR_TEXT, buf);
                y += LINE_H;
            }
            display_text(MENU_X, y, COLOR_DIM, "SHA-256:");
            display_text(VAL_X, y, g_rel.digest[0] ? COLOR_GREEN : COLOR_YELLOW,
                         g_rel.digest[0] ? "published by GitHub" : "not published");
            y += LINE_H + 10;
            display_text(MENU_X, y, COLOR_DIM, "Saves to: " DOWNLOAD_DIR "/");
            y += LINE_H;
            display_text(MENU_X, y, COLOR_DIM, "You install it yourself afterwards.");
        } else {
            display_text(MENU_X, y, COLOR_YELLOW, "This release has no .vpk file attached.");
            y += LINE_H;
            display_text(MENU_X, y, COLOR_DIM, GH_PAGE_URL);
        }
        frame_end(g_rel.have_asset
                  ? (g_newer > 0 ? "[X] Download   [O] Back"
                                 : "[X] Download anyway   [O] Back")
                  : "[O] Back");
        return;
    }

    case PH_DOWNLOADING: {
        frame_begin("Check for Updates  [ DOWNLOADING ]", COLOR_GREEN);
        display_text(MENU_X, y, COLOR_TEXT, g_rel.name);
        y += LINE_H + 12;
        int bx = MENU_X, bw = SCREEN_W - MENU_X * 2, bh = 22;
        display_rect(bx, y, bw, bh, RGBA8(40, 40, 40, 255));
        uint64_t tot = g_dl_total ? g_dl_total : g_rel.size;
        if (tot) {
            int fill = (int)((g_dl_done * (uint64_t)bw) / tot);
            if (fill > bw) fill = bw;
            display_rect(bx, y, fill, bh, COLOR_GREEN);
            snprintf(buf, sizeof(buf), "%llu / %llu KB  (%d%%)",
                     (unsigned long long)(g_dl_done / 1024),
                     (unsigned long long)(tot / 1024),
                     (int)((g_dl_done * 100) / tot));
        } else {
            snprintf(buf, sizeof(buf), "%llu KB", (unsigned long long)(g_dl_done / 1024));
        }
        y += bh + 12;
        display_text(MENU_X, y, COLOR_TEXT, buf);
        frame_end("[O] Cancel");
        return;
    }

    case PH_DONE:
        frame_begin("Check for Updates  [ DOWNLOADED ]", COLOR_GREEN);
        display_text(MENU_X, y, COLOR_GREEN, "Download complete.");
        y += LINE_H + 10;
        display_text(MENU_X, y, COLOR_DIM, "Saved to:");
        y += LINE_H;
        display_text(MENU_X, y, COLOR_TEXT, g_saved_path);
        y += LINE_H + 10;
        display_text(MENU_X, y,
                     g_hash_ok > 0 ? COLOR_GREEN : COLOR_YELLOW, g_hash_msg);
        y += LINE_H + 16;
        display_text(MENU_X, y, COLOR_DIM, "To install: exit to LiveArea, open VitaShell,");
        y += LINE_H;
        display_text(MENU_X, y, COLOR_DIM, "go to ux0:downloads/ and press X on the VPK.");
        frame_end("[O] Back");
        return;

    case PH_ERROR:
    default:
        frame_begin("Check for Updates", COLOR_YELLOW);
        display_text(MENU_X, y, COLOR_RED, g_err);
        y += LINE_H;
        {
            /* readable detail from the HTTPS client, wrapped to the screen */
            const char *dt = https_detail();
            size_t dl = strlen(dt);
            for (size_t off = 0; off < dl && y < FOOTER_Y - 70; off += 52) {
                char line[56];
                snprintf(line, sizeof(line), "%.52s", dt + off);
                display_text(MENU_X, y, COLOR_YELLOW, line);
                y += LINE_H;
            }
        }
        y += 10;
        if (strstr(g_err, "HTTP 404"))
            display_text(MENU_X, y, COLOR_DIM, "No release has been published yet.");
        else if (strstr(g_err, "HTTP 403"))
            display_text(MENU_X, y, COLOR_DIM, "GitHub refused (rate limit?). Try later.");
        else if (!g_insecure && strstr(g_err, "0xE108"))
            display_text(MENU_X, y, COLOR_DIM, "The server certificate was not trusted.");
        frame_end(g_insecure ? "[X] Retry   [O] Back"
                             : "[X] Retry   [/\\] Retry w/o certificate check   [O] Back");
        return;
    }
}

/* Called repeatedly during a download: redraw and watch for O to cancel. */
static int download_progress(uint64_t done, uint64_t total) {
    g_dl_done = done;
    if (total) g_dl_total = total;
    draw_screen();
    SceCtrlData c;
    sceCtrlPeekBufferPositive(0, &c, 1);
    return (c.buttons & SCE_CTRL_CIRCLE) ? 1 : 0;
}

/* ── Actions ─────────────────────────────────────────────────────────────── */
static void do_check(void) {
    g_phase = PH_CHECKING;
    draw_screen();

    char *json = (char *)malloc(JSON_MAX);
    if (!json) { snprintf(g_err, sizeof(g_err), "Out of memory."); g_phase = PH_ERROR; return; }
    json[0] = '\0';

    Sink sk = { .mem = json, .mem_cap = JSON_MAX, .mem_len = 0, .fd = -1,
                .sha = NULL, .progress = NULL };
    int r = http_fetch(GH_API_URL, &sk, 1);
    if (r < 0) {
        fmt_err(r, "Update check");
        g_phase = PH_ERROR;
        free(json);
        return;
    }
    if (parse_release(json, &g_rel) < 0) {
        snprintf(g_err, sizeof(g_err), "Could not read the release information.");
        g_phase = PH_ERROR;
        free(json);
        return;
    }
    free(json);

    long cur = version_value(APP_VERSION);
    long lat = version_value(g_rel.tag);
    if (cur < 0 || lat < 0) g_newer = -1;
    else g_newer = (lat > cur) ? 1 : (lat == cur ? 0 : -1);
    if (cur >= 0 && lat >= 0 && lat < cur) g_newer = -1;
    g_phase = PH_RESULT;
}

static void do_download(void) {
    /* sanitise the file name: keep letters, digits, . _ - */
    char safe[96];
    int n = 0;
    for (const char *p = g_rel.name; *p && n < (int)sizeof(safe) - 1; p++) {
        char ch = *p;
        int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                 (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
        safe[n++] = ok ? ch : '_';
    }
    safe[n] = '\0';
    if (!safe[0]) snprintf(safe, sizeof(safe), "update.vpk");

    char final_path[160], part_path[170];
    snprintf(final_path, sizeof(final_path), DOWNLOAD_DIR "/%s", safe);
    snprintf(part_path,  sizeof(part_path),  "%s.part", final_path);

    sceIoMkdir(DOWNLOAD_DIR, 0777);          /* fine if it already exists */

    int fd = sceIoOpen(part_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        snprintf(g_err, sizeof(g_err), "Cannot create %s (0x%08X)", part_path, (unsigned)fd);
        g_phase = PH_ERROR;
        return;
    }

    Sha256 sha;
    sha_init(&sha);
    Sink sk = { .mem = NULL, .mem_cap = 0, .mem_len = 0, .fd = fd,
                .sha = &sha, .progress = download_progress };

    g_dl_done = 0;
    g_dl_total = 0;
    g_phase = PH_DOWNLOADING;
    int r = http_fetch(g_rel.url, &sk, 0);
    sceIoClose(fd);

    if (r < 0) {
        sceIoRemove(part_path);
        fmt_err(r, "Download");
        g_phase = PH_ERROR;
        return;
    }

    char got[65];
    sha_final_hex(&sha, got);

    if (g_rel.digest[0]) {
        if (strcasecmp(got, g_rel.digest) != 0) {
            sceIoRemove(part_path);
            snprintf(g_err, sizeof(g_err),
                     "Download corrupt: SHA-256 does not match. File deleted.");
            g_phase = PH_ERROR;
            return;
        }
        g_hash_ok = 1;
        snprintf(g_hash_msg, sizeof(g_hash_msg), "SHA-256 verified.");
    } else {
        g_hash_ok = 0;
        snprintf(g_hash_msg, sizeof(g_hash_msg),
                 "SHA-256 %.16s... (GitHub gave no hash to compare)", got);
    }

    sceIoRemove(final_path);                 /* replace an older copy */
    if (sceIoRename(part_path, final_path) < 0) {
        snprintf(g_err, sizeof(g_err), "Could not rename the finished download.");
        g_phase = PH_ERROR;
        return;
    }
    snprintf(g_saved_path, sizeof(g_saved_path), "%s", final_path);
    g_phase = PH_DONE;
}

/* ── Entry point ─────────────────────────────────────────────────────────── */
void update_screen_run(void) {
    SceCtrlData ctrl;
    memset(&g_old, 0, sizeof(g_old));
    do { sceCtrlPeekBufferPositive(0, &g_old, 1); } while (g_old.buttons);

    memset(&g_rel, 0, sizeof(g_rel));
    g_insecure = 0;

    net_ensure_init();
    SceNetCtlInfo info;
    int wifi_ok = (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0);
    int hr = 0;

    if (!wifi_ok) {
        snprintf(g_err, sizeof(g_err), "WiFi is not connected.");
        g_phase = PH_ERROR;
    } else if (hr < 0) {
        snprintf(g_err, sizeof(g_err), "Could not start HTTP: 0x%08X", (unsigned)hr);
        g_phase = PH_ERROR;
    } else {
        do_check();
    }

    while (1) {
        draw_screen();
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        unsigned pressed = ctrl.buttons & ~g_old.buttons;

        if (pressed & SCE_CTRL_CIRCLE) break;

        if (g_phase == PH_RESULT && (pressed & SCE_CTRL_CROSS) && g_rel.have_asset) {
            do_download();
            memset(&g_old, 0xFF, sizeof(g_old));   /* swallow held buttons */
        } else if (g_phase == PH_ERROR && wifi_ok && hr >= 0) {
            if (pressed & SCE_CTRL_CROSS) {
                do_check();
            } else if ((pressed & SCE_CTRL_TRIANGLE) && !g_insecure) {
                g_insecure = 1;
                do_check();
            }
        }
        g_old = ctrl;
    }

    /* leave HTTP initialised for the session; it is cheap and reusable */
}
