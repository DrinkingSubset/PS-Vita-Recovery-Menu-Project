/*
 * plugins.c — taiHEN Plugin Manager
 *
 * COMPATIBILITY FIX (was broken for PCH-2000 and PSTV):
 *
 * Previous version hardcoded "ux0:tai/config.txt". On PCH-2000 and PSTV,
 * taiHEN loads imc0:tai/config.txt FIRST (highest priority). Editing ux0:
 * while imc0: is active has NO effect.
 *
 * Fix: use g_compat.active_tai_config — the path compat.c determined is
 * actually being used by taiHEN on this hardware at startup.
 *
 * Also fixed: sceIoGetstatByFd() is broken on some firmware builds.
 * Replaced with sceIoLseek() for file size, which works on all supported FW.
 *
 * Also fixed: .suprx (user plugins) were ignored — now captured alongside .skprx.
 */

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "plugins.h"
#include "compat.h"

#define MAX_LINE     256
#define MAX_CFG_SIZE (512 * 1024)

/* v1.1.4: one entry per (section, plugin path). The old code merged every
 * line with the same FILE NAME across all sections into one entry, and the
 * save then DELETED the later lines (for example henkaku.suprx under
 * *NPXS10015 / *NPXS10016). Now each section keeps its own lines, and save
 * only flips the '#' on lines whose state really changed. */
typedef struct {
    char path[MAX_LINE];
    int  enabled;     /* 1 if any matching line is uncommented */
    int  essential;   /* 1 = protected, cannot be toggled */
    char section[64];
} PluginEntry;

static PluginEntry g_plugins[MAX_PLUGINS];
static int         g_plugin_count = 0;
static int         g_last_err     = 0;   /* detail code of the last failure */
static int         g_backup_done  = 0;   /* .bak written once per app run */

/* ── File helpers ────────────────────────────────────────────────────────── */

/* Returns just the filename portion of a path (after last '/') */
static const char *basename_of(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/* Plugins that are essential to HENkaku and must never be toggled off.
 * Matches AutoPlugin2's "Essential plugin, can not uninstall!" list. */
static int is_essential_plugin(const char *path) {
    const char *name = basename_of(path);
    /* henkaku.suprx is required in *main, *NPXS10015, *NPXS10016 for
     * HENkaku to function. henkaku.skprx is hard-coded by taiHEN itself. */
    if (strcasecmp(name, "henkaku.suprx") == 0) return 1;
    if (strcasecmp(name, "henkaku.skprx") == 0) return 1;
    /* storagemgr.skprx / gamesd.skprx redirect ux0: to the SD card adapter.
     * Disabling either causes ux0: to go missing on next boot, which on
     * SD2Vita systems means the system can no longer find any apps at all. */
    if (strcasecmp(name, "storagemgr.skprx") == 0) return 1;
    if (strcasecmp(name, "gamesd.skprx")     == 0) return 1;
    return 0;
}

static char *read_file_alloc(const char *path, int *out_len) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) { g_last_err = fd; return NULL; }
    /* sceIoGetstatByFd is broken on some FW — use sceIoLseek instead */
    int size = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (size < 0 || size > MAX_CFG_SIZE) {
        sceIoClose(fd); g_last_err = size < 0 ? size : -1; return NULL;
    }
    char *buf = malloc(size + 1);
    if (!buf) { sceIoClose(fd); g_last_err = -2; return NULL; }
    int rd = 0;
    while (rd < size) {
        int r = sceIoRead(fd, buf + rd, size - rd);
        if (r <= 0) break;
        rd += r;
    }
    sceIoClose(fd);
    if (rd != size) { free(buf); g_last_err = -3; return NULL; }
    buf[size] = '\0';
    if (out_len) *out_len = size;
    return buf;
}

static int write_all(const char *path, const char *data, int len) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return fd;
    int done = 0;
    while (done < len) {
        int w = sceIoWrite(fd, data + done, len - done);
        if (w <= 0) { sceIoClose(fd); return w < 0 ? w : -1; }
        done += w;
    }
    sceIoClose(fd);
    return 0;
}

/* 1 if the file at path has exactly this content */
static int file_matches(const char *path, const char *data, int len) {
    int rl = 0;
    char *back = read_file_alloc(path, &rl);
    if (!back) return 0;
    int same = (rl == len) && memcmp(back, data, len) == 0;
    free(back);
    return same;
}

/* Safe config write:
 *   1. first write of this run: copy the untouched original to <path>.bak
 *   2. write <path>.tmp and read it back to verify
 *   3. replace the config with the .tmp (direct write as fallback)
 *   4. read the config back and compare
 * Returns 0 on success, negative on failure (detail in g_last_err). */
static int safe_write_config(const char *path,
                             const char *original, int orig_len,
                             const char *data, int len) {
    char aux[300];
    if (!g_backup_done && original) {
        snprintf(aux, sizeof(aux), "%s.bak", path);
        int r = write_all(aux, original, orig_len);
        if (r < 0 || !file_matches(aux, original, orig_len)) {
            g_last_err = r < 0 ? r : -10;
            return -1;          /* no backup = no change to the config */
        }
        g_backup_done = 1;
    }

    snprintf(aux, sizeof(aux), "%s.tmp", path);
    int r = write_all(aux, data, len);
    if (r < 0 || !file_matches(aux, data, len)) {
        sceIoRemove(aux);
        g_last_err = r < 0 ? r : -11;
        return -1;              /* config untouched */
    }

    sceIoRemove(path);
    if (sceIoRename(aux, path) < 0) {
        /* Rename refused: write the config directly (backup exists) */
        r = write_all(path, data, len);
        sceIoRemove(aux);
        if (r < 0) { g_last_err = r; return -1; }
    }

    if (!file_matches(path, data, len)) { g_last_err = -12; return -1; }
    return 0;
}

/* Returns the tai config path that taiHEN actually loaded on this hardware */
static const char *get_active_config(void) {
    if (g_compat.active_tai_config[0] == '\0' ||
        g_compat.active_tai_config[0] == '(')
        return NULL;
    return g_compat.active_tai_config;
}

/* Case-insensitive substring test (strcasestr is not in every newlib) */
static int strcasestr_simple(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, nl) == 0) return 1;
    return 0;
}

/* Returns true for lines that are plugin paths (.skprx or .suprx) */
static int is_plugin_line(const char *line) {
    if (!strstr(line, "0:")) return 0;
    if (!strcasestr_simple(line, ".skprx") && !strcasestr_simple(line, ".suprx")) return 0;
    return 1;
}

/* ── Line parsing (shared by load / save / dedup) ───────────────────────── */

typedef struct {
    char *start;        /* line text (not NUL-terminated in the buffer) */
    int   len;          /* length without the newline and without '\r' */
    int   has_cr;       /* line ended in \r\n */
    int   has_nl;       /* line ended in \n (last line may not) */
    int   is_section;   /* "*NAME" line */
    int   is_plugin;    /* plugin path line (commented or not) */
    int   commented;
    char  path[MAX_LINE];   /* plugin path, trimmed */
    char  section[64];      /* section this line belongs to */
} CfgLine;

/* Split buf into lines. Returns a malloc'd array (caller frees) */
static CfgLine *parse_lines(char *buf, int len, int *out_count) {
    int cap = 1;
    for (int i = 0; i < len; i++) if (buf[i] == '\n') cap++;
    CfgLine *L = calloc(cap, sizeof(CfgLine));
    if (!L) { g_last_err = -2; return NULL; }

    int n = 0;
    char section[64] = "";
    char *p = buf, *end = buf + len;
    while (p < end && n < cap) {
        char *nl = memchr(p, '\n', end - p);
        char *le = nl ? nl : end;
        CfgLine *c = &L[n++];
        c->start  = p;
        c->has_nl = (nl != NULL);
        c->len    = (int)(le - p);
        if (c->len > 0 && p[c->len - 1] == '\r') { c->has_cr = 1; c->len--; }

        /* Work on a trimmed copy */
        char tmp[MAX_LINE * 2];
        int tl = c->len < (int)sizeof(tmp) - 1 ? c->len : (int)sizeof(tmp) - 1;
        memcpy(tmp, p, tl); tmp[tl] = '\0';
        char *t = tmp;
        while (*t == ' ' || *t == '\t') t++;

        if (*t == '*') {
            c->is_section = 1;
            char *sname = t + 1;
            int sl = strlen(sname);
            while (sl > 0 && (sname[sl-1] == ' ' || sname[sl-1] == '\t')) sname[--sl] = '\0';
            snprintf(section, sizeof(section), "%s", sname);
        } else {
            if (*t == '#') { c->commented = 1; t++; while (*t == '#' || *t == ' ' || *t == '\t') t++; }
            int pl = strlen(t);
            while (pl > 0 && (t[pl-1] == ' ' || t[pl-1] == '\t')) t[--pl] = '\0';
            if (pl > 0 && pl < MAX_LINE && is_plugin_line(t)) {
                c->is_plugin = 1;
                memcpy(c->path, t, pl + 1);   /* pl < MAX_LINE checked above */
            }
        }
        snprintf(c->section, sizeof(c->section), "%s", section);
        p = nl ? nl + 1 : end;
    }
    *out_count = n;
    return L;
}

/* Append helper for building output */
static void out_put(char *out, int *pos, const char *s, int n) {
    memcpy(out + *pos, s, n); *pos += n;
}
static void out_eol(char *out, int *pos, const CfgLine *c) {
    if (c->has_cr) out[(*pos)++] = '\r';
    if (c->has_nl) out[(*pos)++] = '\n';
}

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Returns 1 if the file at this Vita path actually exists on disk */
static int file_exists_on_vita(const char *path) {
    SceIoStat st;
    return (sceIoGetstat(path, &st) >= 0) ? 1 : 0;
}

static int find_entry(const char *section, const char *path) {
    for (int i = 0; i < g_plugin_count; i++)
        if (strcmp(g_plugins[i].section, section) == 0 &&
            strcasecmp(g_plugins[i].path, path) == 0)
            return i;
    return -1;
}

int plugins_load(void) {
    g_plugin_count = 0;
    g_last_err = 0;
    const char *config_path = get_active_config();
    if (!config_path) { g_last_err = -20; return -1; }

    int len;
    char *buf = read_file_alloc(config_path, &len);
    if (!buf) return -1;

    int n = 0;
    CfgLine *L = parse_lines(buf, len, &n);
    if (!L) { free(buf); return -1; }

    for (int i = 0; i < n && g_plugin_count < MAX_PLUGINS; i++) {
        CfgLine *c = &L[i];
        if (!c->is_plugin) continue;
        int e = find_entry(c->section, c->path);
        if (e >= 0) {
            /* Same plugin listed twice in the same section: one entry */
            if (!c->commented) g_plugins[e].enabled = 1;
            continue;
        }
        PluginEntry *pe = &g_plugins[g_plugin_count++];
        snprintf(pe->path,    sizeof(pe->path),    "%s", c->path);
        snprintf(pe->section, sizeof(pe->section), "%s", c->section);
        pe->essential = is_essential_plugin(c->path);
        pe->enabled   = !c->commented;   /* real state, also for essentials */
    }
    free(L);
    free(buf);
    return g_plugin_count;
}

int         plugins_get_count(void)          { return g_plugin_count; }
int         plugins_is_enabled(int idx)      { return (idx >= 0 && idx < g_plugin_count) ? g_plugins[idx].enabled : 0; }
int         plugins_is_essential(int idx)    { return (idx >= 0 && idx < g_plugin_count) ? g_plugins[idx].essential : 0; }
void        plugins_toggle(int idx)          { if (idx >= 0 && idx < g_plugin_count && !g_plugins[idx].essential) g_plugins[idx].enabled ^= 1; }
const char *plugins_get_config_path(void)    { const char *p = get_active_config(); return p ? p : "(no config found)"; }
int         plugins_last_error(void)         { return g_last_err; }

const char *plugins_get_name(int idx) {
    if (idx < 0 || idx >= g_plugin_count) return "???";
    return basename_of(g_plugins[idx].path);
}

const char *plugins_get_path(int idx) {
    if (idx < 0 || idx >= g_plugin_count) return "";
    return g_plugins[idx].path;
}

const char *plugins_get_section(int idx) {
    if (idx < 0 || idx >= g_plugin_count) return "";
    return g_plugins[idx].section;
}

/*
 * plugins_save — Write plugin toggle state back to the ACTIVE config file.
 * On PCH-2000 with imc0: active, this correctly edits imc0:tai/config.txt.
 *
 * Every line is copied unchanged except plugin lines whose on/off state
 * differs from the menu: those get a '#' added or removed. No line is ever
 * dropped. Returns 0 on success (also when nothing changed), -1 on error.
 */
int plugins_save(void) {
    g_last_err = 0;
    const char *config_path = get_active_config();
    if (!config_path) { g_last_err = -20; return -1; }

    int len;
    char *original = read_file_alloc(config_path, &len);
    if (!original) return -1;

    char *work = malloc(len + 1);
    if (!work) { free(original); g_last_err = -2; return -1; }
    memcpy(work, original, len + 1);

    int n = 0;
    CfgLine *L = parse_lines(work, len, &n);
    if (!L) { free(work); free(original); return -1; }

    /* Worst case each line gains one '#' */
    char *out = malloc(len + n + 16);
    if (!out) { free(L); free(work); free(original); g_last_err = -2; return -1; }
    int pos = 0;

    for (int i = 0; i < n; i++) {
        CfgLine *c = &L[i];
        int e = c->is_plugin ? find_entry(c->section, c->path) : -1;
        if (e >= 0 && !g_plugins[e].essential &&
            g_plugins[e].enabled == c->commented) {
            if (g_plugins[e].enabled) {
                /* enable: write just the path (drops '#' and spacing) */
                out_put(out, &pos, c->path, strlen(c->path));
            } else {
                /* disable: '#' in front of the original text */
                out[pos++] = '#';
                out_put(out, &pos, c->start, c->len);
            }
        } else {
            out_put(out, &pos, c->start, c->len);
        }
        out_eol(out, &pos, c);
    }

    int r = 0;
    if (pos != len || memcmp(out, original, len) != 0)
        r = safe_write_config(config_path, original, len, out, pos);

    free(out); free(L); free(work); free(original);
    return r;
}

/*
 * plugins_remove_duplicates — Removes lines that list the same plugin file
 * twice inside the SAME section. Lines in different sections are never
 * duplicates (a plugin can legitimately load in *main and *NPXS10015).
 * Which line is kept:
 *   1. The path that actually exists on disk (file_exists_on_vita)
 *   2. If both/neither exist: the enabled (uncommented) one
 *   3. If both enabled: the ur0: path (AutoPlugin2 canonical)
 *   4. Otherwise the first one
 * Returns number of duplicates removed, or -1 on error.
 */
int plugins_remove_duplicates(void) {
    g_last_err = 0;
    const char *config_path = get_active_config();
    if (!config_path) { g_last_err = -20; return -1; }

    int len;
    char *original = read_file_alloc(config_path, &len);
    if (!original) return -1;
    char *work = malloc(len + 1);
    if (!work) { free(original); g_last_err = -2; return -1; }
    memcpy(work, original, len + 1);

    int n = 0;
    CfgLine *L = parse_lines(work, len, &n);
    if (!L) { free(work); free(original); return -1; }

    char *drop   = calloc(n ? n : 1, 1);
    int  *exists = malloc(sizeof(int) * (n ? n : 1));
    if (!drop || !exists) {
        free(drop); free(exists); free(L); free(work); free(original);
        g_last_err = -2; return -1;
    }
    for (int i = 0; i < n; i++)
        exists[i] = L[i].is_plugin ? file_exists_on_vita(L[i].path) : 0;

    int removed = 0;
    for (int i = 0; i < n; i++) {
        if (!L[i].is_plugin || drop[i]) continue;
        int best = i;
        for (int j = i + 1; j < n; j++) {
            if (!L[j].is_plugin || drop[j]) continue;
            if (strcmp(L[j].section, L[i].section) != 0) continue;
            if (strcasecmp(basename_of(L[j].path), basename_of(L[i].path)) != 0) continue;

            /* j duplicates best — decide which survives */
            int keep_j = 0;
            if (!exists[best] && exists[j]) keep_j = 1;
            else if (exists[best] == exists[j]) {
                if (L[best].commented && !L[j].commented) keep_j = 1;
                else if (L[best].commented == L[j].commented &&
                         strncasecmp(L[j].path, "ur0:", 4) == 0 &&
                         strncasecmp(L[best].path, "ur0:", 4) != 0) keep_j = 1;
            }
            if (keep_j) { drop[best] = 1; best = j; }
            else        { drop[j] = 1; }
            removed++;
        }
    }

    int r = 0;
    if (removed > 0) {
        char *out = malloc(len + 4);
        if (!out) r = -1, g_last_err = -2;
        else {
            int pos = 0;
            for (int i = 0; i < n; i++) {
                if (drop[i]) continue;
                out_put(out, &pos, L[i].start, L[i].len);
                out_eol(out, &pos, &L[i]);
            }
            r = safe_write_config(config_path, original, len, out, pos);
            free(out);
        }
    }

    free(drop); free(exists); free(L); free(work); free(original);
    return r < 0 ? -1 : removed;
}

/*
 * plugins_clean_config — Full config repair:
 *   1. Reads every plugin entry (including commented ones)
 *   2. Deduplicates by filename WITHIN each section (v1.1.4: never across
 *      sections) using file-existence rules
 *   3. Re-enables any plugin whose file actually exists on disk
 *   4. Writes a clean config.txt preserving section structure
 *
 * This fixes configs corrupted by AutoPlugin2's repair feature
 * which mass-comments all entries and introduces duplicates.
 *
 * Returns: number of changes made (deduped + re-enabled), or -1 on error.
 */
int plugins_clean_config(void) {
    g_last_err = 0;
    const char *config_path = get_active_config();
    if (!config_path) { g_last_err = -20; return -1; }

    int len;
    char *original = read_file_alloc(config_path, &len);
    if (!original) return -1;

    /* --- Pass 1: Collect all unique plugin entries --- */
    typedef struct {
        char path[MAX_LINE];
        char section[64];
        int  enabled;        /* original state in config */
        int  file_exists;    /* does the file actually exist on disk? */
    } CleanEntry;

    #define CLEAN_MAX_ENTRIES (MAX_PLUGINS * 2)
    #define CLEAN_MAX_SECTIONS 64
    static CleanEntry entries[CLEAN_MAX_ENTRIES];
    int entry_count = 0;
    static char section_order[CLEAN_MAX_SECTIONS][64];
    int  section_count = 0;
    int  overflow = 0;   /* too many entries/sections: refuse to rewrite */

    char *buf = malloc(len + 1);
    if (!buf) { free(original); return -1; }
    memcpy(buf, original, len + 1);

    char current_section[64] = "";
    char *line = strtok(buf, "\n");
    while (line) {
        int ll = strlen(line);
        if (ll > 0 && line[ll-1] == '\r') line[--ll] = '\0';

        char *stripped = line;
        while (*stripped == ' ' || *stripped == '\t') stripped++;

        if (stripped[0] == '*') {
            snprintf(current_section, sizeof(current_section), "%s", stripped + 1);
            /* Track section order */
            int found = 0;
            for (int i = 0; i < section_count; i++)
                if (strcmp(section_order[i], current_section) == 0) { found = 1; break; }
            if (!found) {
                if (section_count < CLEAN_MAX_SECTIONS)
                    snprintf(section_order[section_count++], 64, "%s", current_section);
                else
                    overflow = 1;
            }
        } else if (current_section[0]) {
            int commented = (stripped[0] == '#');
            char *raw = commented ? stripped + 1 : stripped;
            while (*raw == ' ' || *raw == '\t') raw++;

            int rl = strlen(raw);
            while (rl > 0 && (raw[rl-1] == ' ' || raw[rl-1] == '\t')) raw[--rl] = '\0';
            if (is_plugin_line(raw)) {
                const char *name = basename_of(raw);
                /* Duplicate = same filename in the SAME section only */
                int existing = -1;
                for (int i = 0; i < entry_count; i++)
                    if (strcmp(entries[i].section, current_section) == 0 &&
                        strcasecmp(basename_of(entries[i].path), name) == 0) { existing = i; break; }

                if (existing < 0 && entry_count >= CLEAN_MAX_ENTRIES) {
                    overflow = 1;
                } else if (existing < 0) {
                    /* New entry */
                    snprintf(entries[entry_count].path,    MAX_LINE, "%s", raw);
                    snprintf(entries[entry_count].section, 64,       "%s", current_section);
                    entries[entry_count].enabled     = !commented;
                    entries[entry_count].file_exists = file_exists_on_vita(raw);
                    entry_count++;
                } else if (existing >= 0) {
                    /* Duplicate — keep the better one using file-existence rules */
                    CleanEntry *e = &entries[existing];
                    int new_exists = file_exists_on_vita(raw);
                    int new_enabled = !commented;

                    if (!e->file_exists && new_exists) {
                        snprintf(e->path,    MAX_LINE, "%s", raw);
                        snprintf(e->section, 64,       "%s", current_section);
                        e->enabled = new_enabled; e->file_exists = 1;
                    } else if (e->file_exists && !new_exists) {
                        /* keep existing */
                    } else if (!e->enabled && new_enabled) {
                        snprintf(e->path,    MAX_LINE, "%s", raw);
                        snprintf(e->section, 64,       "%s", current_section);
                        e->enabled = 1; e->file_exists = new_exists;
                    } else if (strncmp(raw, "ur0:", 4) == 0 &&
                               strncmp(e->path, "ur0:", 4) != 0) {
                        snprintf(e->path,    MAX_LINE, "%s", raw);
                        snprintf(e->section, 64,       "%s", current_section);
                        e->enabled = new_enabled; e->file_exists = new_exists;
                    }
                }
            }
        }
        line = strtok(NULL, "\n");
    }
    free(buf);

    if (overflow) {
        /* Rewriting would lose plugin lines; leave the config alone */
        free(original);
        g_last_err = -30;
        return -1;
    }

    /* --- Pass 2: Re-enable plugins whose file exists on disk --- */
    int changes = 0;
    for (int i = 0; i < entry_count; i++) {
        if (entries[i].file_exists && !entries[i].enabled) {
            entries[i].enabled = 1;
            changes++;
        }
    }

    /* --- Pass 3: Write clean config --- */
    char *out = malloc(len * 2 + 4096);
    if (!out) { free(original); return -1; }
    int out_pos = 0;

    /* Standard header */
    const char *header =
        "# PSVita taiHEN config - cleaned by PS Vita Recovery Menu\n"
        "# henkaku.skprx is hard-coded and not listed here\n"
        "# Kernel plugins require reboot. User plugins need taiHEN refresh.\n\n";
    int hl = strlen(header);
    memcpy(out + out_pos, header, hl);
    out_pos += hl;

    for (int s = 0; s < section_count; s++) {
        /* Check if this section has any plugins */
        int has_plugins = 0;
        for (int i = 0; i < entry_count; i++)
            if (strcmp(entries[i].section, section_order[s]) == 0) { has_plugins = 1; break; }
        if (!has_plugins) continue;

        /* Write section header */
        out[out_pos++] = '*';
        int sl = strlen(section_order[s]);
        memcpy(out + out_pos, section_order[s], sl);
        out_pos += sl;
        out[out_pos++] = '\n';

        /* Write plugins for this section */
        for (int i = 0; i < entry_count; i++) {
            if (strcmp(entries[i].section, section_order[s]) != 0) continue;
            if (!entries[i].enabled) out[out_pos++] = '#';
            int pl = strlen(entries[i].path);
            memcpy(out + out_pos, entries[i].path, pl);
            out_pos += pl;
            out[out_pos++] = '\n';
            changes++; /* count all written lines as changes from original mess */
        }
        out[out_pos++] = '\n';
    }
    out[out_pos] = '\0';

    int wr = safe_write_config(config_path, original, len, out, out_pos);
    free(out);
    free(original);
    return wr < 0 ? -1 : changes;
}

/*
 * plugins_apply_safe_mode() — silently disable all non-essential plugins.
 *
 * Used by main.c when the L-trigger safe mode boot flag is detected.
 * No confirmation dialog — applies immediately and saves.
 * Returns number of plugins disabled, or negative on error.
 */
int plugins_apply_safe_mode(void) {
    int n = plugins_get_count();
    if (n <= 0) return -1;
    int changed = 0;
    for (int i = 0; i < n; i++) {
        if (!plugins_is_essential(i) && plugins_is_enabled(i)) {
            plugins_toggle(i);
            changed++;
        }
    }
    if (changed > 0)
        plugins_save();
    return changed;
}
