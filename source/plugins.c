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
#include "plugins.h"
#include "compat.h"

#define MAX_PLUGINS  64
#define MAX_LINE     256

typedef struct {
    char path[MAX_LINE];
    int  enabled;
    int  essential;   /* 1 = protected, cannot be toggled */
    char section[64];
} PluginEntry;

static PluginEntry g_plugins[MAX_PLUGINS];
static int         g_plugin_count = 0;

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
    if (strcmp(name, "henkaku.suprx") == 0) return 1;
    if (strcmp(name, "henkaku.skprx") == 0) return 1;
    /* storagemgr.skprx / gamesd.skprx redirect ux0: to the SD card adapter.
     * Disabling either causes ux0: to go missing on next boot, which on
     * SD2Vita systems means the system can no longer find any apps at all. */
    if (strcmp(name, "storagemgr.skprx") == 0) return 1;
    if (strcmp(name, "gamesd.skprx")     == 0) return 1;
    return 0;
}

static char *read_file_alloc(const char *path, int *out_len) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return NULL;
    /* sceIoGetstatByFd is broken on some FW — use sceIoLseek instead */
    int size = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (size <= 0 || size > 512 * 1024) { sceIoClose(fd); return NULL; }
    char *buf = malloc(size + 1);
    if (!buf) { sceIoClose(fd); return NULL; }
    sceIoRead(fd, buf, size);
    sceIoClose(fd);
    buf[size] = '\0';
    if (out_len) *out_len = size;
    return buf;
}

static int write_file(const char *path, const char *data, int len) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return -1;
    sceIoWrite(fd, data, len);
    sceIoClose(fd);
    return 0;
}

/* Returns the tai config path that taiHEN actually loaded on this hardware */
static const char *get_active_config(void) {
    if (g_compat.active_tai_config[0] == '\0' ||
        g_compat.active_tai_config[0] == '(')
        return NULL;
    return g_compat.active_tai_config;
}


/* Returns true for lines that are plugin paths (.skprx or .suprx) */
static int is_plugin_line(const char *line) {
    if (!strstr(line, "0:")) return 0;
    if (!strstr(line, ".skprx") && !strstr(line, ".suprx")) return 0;
    return 1;
}

/* ── Public API ──────────────────────────────────────────────────────────── */


/* Returns 1 if the file at this Vita path actually exists on disk */
static int file_exists_on_vita(const char *path) {
    SceIoStat st;
    return (sceIoGetstat(path, &st) >= 0) ? 1 : 0;
}

/* Find index of existing entry with same filename, -1 if none */
static int find_same_filename(const char *path) {
    const char *name = basename_of(path);
    for (int i = 0; i < g_plugin_count; i++) {
        if (strcmp(basename_of(g_plugins[i].path), name) == 0)
            return i;
    }
    return -1;
}

int plugins_load(void) {
    g_plugin_count = 0;
    const char *config_path = get_active_config();
    if (!config_path) return -1;

    int len;
    char *buf = read_file_alloc(config_path, &len);
    if (!buf) return -1;

    char current_section[64] = "";
    char *line = strtok(buf, "\n");
    while (line && g_plugin_count < MAX_PLUGINS) {
        int ll = strlen(line);
        if (ll > 0 && line[ll-1] == '\r') line[--ll] = '\0';
        if (ll == 0) { line = strtok(NULL, "\n"); continue; }

        if (line[0] == '*') {
            snprintf(current_section, sizeof(current_section), "%s", line + 1);
        } else {
            int commented  = (line[0] == '#');
            char *raw_line = commented ? line + 1 : line;
            while (*raw_line == ' ' || *raw_line == '\t') raw_line++;
            if (is_plugin_line(raw_line)) {
                int existing = find_same_filename(raw_line);
                if (existing < 0) {
                    /* No duplicate — just add it */
                    PluginEntry *e = &g_plugins[g_plugin_count++];
                    snprintf(e->path,    sizeof(e->path),    "%s", raw_line);
                    snprintf(e->section, sizeof(e->section), "%s", current_section);
                    e->essential = is_essential_plugin(raw_line);
                    /* Essential plugins are always ON regardless of config state */
                    e->enabled = e->essential ? 1 : !commented;
                } else {
                    /* Duplicate filename found — decide which entry to keep:
                     * Rule 1: Keep whichever path actually exists on disk.
                     * Rule 2: If both exist, keep the enabled (uncommented) one.
                     * Rule 3: If both enabled, keep ur0: path (AutoPlugin2 canonical).
                     * We update the existing entry in-place if the new one is better. */
                    PluginEntry *e = &g_plugins[existing];
                    int old_exists = file_exists_on_vita(e->path);
                    int new_exists = file_exists_on_vita(raw_line);

                    if (!old_exists && new_exists) {
                        /* New path actually has the file — replace */
                        snprintf(e->path,    sizeof(e->path),    "%s", raw_line);
                        snprintf(e->section, sizeof(e->section), "%s", current_section);
                        e->enabled = !commented;
                    } else if (old_exists && !new_exists) {
                        /* Old path has the file — keep old, discard new (do nothing) */
                    } else if (!commented && !e->enabled) {
                        /* Both exist (or neither): new is enabled, old is disabled
                         * — the enabled one is the real one */
                        snprintf(e->path,    sizeof(e->path),    "%s", raw_line);
                        snprintf(e->section, sizeof(e->section), "%s", current_section);
                        e->enabled = 1;
                    } else if (strncmp(raw_line, "ur0:", 4) == 0 &&
                               strncmp(e->path,  "ur0:", 4) != 0) {
                        /* Both exist and same enabled state:
                         * prefer ur0: (AutoPlugin2 canonical location) */
                        snprintf(e->path,    sizeof(e->path),    "%s", raw_line);
                        snprintf(e->section, sizeof(e->section), "%s", current_section);
                        e->enabled = !commented;
                    }
                    /* Otherwise keep existing entry as-is */
                }
            }
        }
        line = strtok(NULL, "\n");
    }
    free(buf);
    return g_plugin_count;
}

int         plugins_get_count(void)          { return g_plugin_count; }
int         plugins_is_enabled(int idx)      { return (idx >= 0 && idx < g_plugin_count) ? g_plugins[idx].enabled : 0; }
int         plugins_is_essential(int idx)    { return (idx >= 0 && idx < g_plugin_count) ? g_plugins[idx].essential : 0; }
void        plugins_toggle(int idx)          { if (idx >= 0 && idx < g_plugin_count && !g_plugins[idx].essential) g_plugins[idx].enabled ^= 1; }
const char *plugins_get_config_path(void)    { const char *p = get_active_config(); return p ? p : "(no config found)"; }

const char *plugins_get_name(int idx) {
    if (idx < 0 || idx >= g_plugin_count) return "???";
    const char *p = strrchr(g_plugins[idx].path, '/');
    return p ? p + 1 : g_plugins[idx].path;
}

const char *plugins_get_section(int idx) {
    if (idx < 0 || idx >= g_plugin_count) return "";
    return g_plugins[idx].section;
}

/*
 * plugins_save — Write plugin toggle state back to the ACTIVE config file.
 * On PCH-2000 with imc0: active, this correctly edits imc0:tai/config.txt.
 */
int plugins_save(void) {
    const char *config_path = get_active_config();
    if (!config_path) return -1;

    int len;
    char *original = read_file_alloc(config_path, &len);
    if (!original) return -1;

    char *out = malloc(len + MAX_PLUGINS * 2 + 64);
    if (!out) { free(original); return -1; }
    int out_pos = 0;

    /* Track which g_plugins entries have been written to avoid duplicates */
    int written[MAX_PLUGINS];
    for (int i = 0; i < MAX_PLUGINS; i++) written[i] = 0;

    char *line = strtok(original, "\n");
    while (line) {
        int ll = strlen(line);
        if (ll > 0 && line[ll-1] == '\r') line[--ll] = '\0';

        int commented  = (line[0] == '#');
        char *raw_line = commented ? line + 1 : line;
        while (*raw_line == ' ' || *raw_line == '\t') raw_line++;

        int handled = 0;
        if (is_plugin_line(raw_line)) {
            /* Match by basename — same logic as plugins_load() deduplication.
             * If two lines share a basename, only the FIRST match in g_plugins
             * will have the toggled state; subsequent duplicates are dropped
             * (written as blank — i.e. skip them entirely to clean up). */
            const char *raw_name = basename_of(raw_line);
            int matched_idx = -1;
            for (int i = 0; i < g_plugin_count; i++) {
                if (strcmp(basename_of(g_plugins[i].path), raw_name) == 0) {
                    matched_idx = i;
                    break;
                }
            }
            if (matched_idx >= 0 && !written[matched_idx]) {
                PluginEntry *e = &g_plugins[matched_idx];
                /* Write the canonical path we loaded (may differ from raw_line
                 * if dedup chose a different path, e.g. ur0: over ux0:) */
                if (!e->enabled && !e->essential) out[out_pos++] = '#';
                int pl = strlen(e->path);
                memcpy(out + out_pos, e->path, pl);
                out_pos += pl;
                out[out_pos++] = '\n';
                written[matched_idx] = 1;
                handled = 1;
            } else if (matched_idx >= 0 && written[matched_idx]) {
                /* Duplicate line — already written this plugin, drop it */
                handled = 1;
            }
        }
        if (!handled) {
            memcpy(out + out_pos, line, ll);
            out_pos += ll;
            out[out_pos++] = '\n';
        }
        line = strtok(NULL, "\n");
    }
    out[out_pos] = '\0';
    int r = write_file(config_path, out, out_pos);
    free(out);
    free(original);
    return r;
}

/*
 * plugins_remove_duplicates — Rewrites config.txt keeping only the REAL
 * occurrence of each plugin, determined by:
 *   1. The path that actually exists on disk (file_exists_on_vita)
 *   2. If both/neither exist: the enabled (uncommented) one
 *   3. If both enabled: the ur0: path (AutoPlugin2 canonical)
 * Returns number of duplicates removed, or -1 on error.
 */
int plugins_remove_duplicates(void) {
    const char *config_path = get_active_config();
    if (!config_path) return -1;

    int len;
    char *original = read_file_alloc(config_path, &len);
    if (!original) return -1;

    /* --- Pass 1: For each filename, decide which path is the real one --- */
    typedef struct { char keep[MAX_LINE]; char drop[MAX_LINE]; } DupPair;
    DupPair pairs[MAX_PLUGINS];
    int pair_count = 0;

    /* Re-parse to collect all entries grouped by filename */
    typedef struct { char path[MAX_LINE]; int enabled; } RawEntry;
    RawEntry raw[MAX_PLUGINS * 2];
    int raw_count = 0;
    char *buf2 = malloc(len + 1);
    if (!buf2) { free(original); return -1; }
    memcpy(buf2, original, len + 1);

    char *line = strtok(buf2, "\n");
    while (line && raw_count < MAX_PLUGINS * 2) {
        int ll = strlen(line);
        if (ll > 0 && line[ll-1] == '\r') line[--ll] = '\0';
        int commented = (ll > 0 && line[0] == '#');
        char *rl = commented ? line + 1 : line;
        while (*rl == ' ' || *rl == '\t') rl++;
        if (is_plugin_line(rl)) {
            snprintf(raw[raw_count].path, MAX_LINE, "%s", rl);
            raw[raw_count].enabled = !commented;
            raw_count++;
        }
        line = strtok(NULL, "\n");
    }
    free(buf2);

    /* Find pairs with same filename */
    char checked[MAX_PLUGINS][64];
    int checked_count = 0;
    for (int i = 0; i < raw_count; i++) {
        const char *name = basename_of(raw[i].path);
        /* Already processed this filename? */
        int already = 0;
        for (int c = 0; c < checked_count; c++)
            if (strcmp(checked[c], name) == 0) { already = 1; break; }
        if (already) continue;
        snprintf(checked[checked_count++], 64, "%s", name);

        /* Find all entries with this filename */
        int matches[8], mc = 0;
        for (int j = i; j < raw_count && mc < 8; j++)
            if (strcmp(basename_of(raw[j].path), name) == 0)
                matches[mc++] = j;
        if (mc < 2) continue; /* no duplicate */

        /* Pick the best one using the rules */
        int best = matches[0];
        for (int m = 1; m < mc; m++) {
            int bi = best, ci = matches[m];
            int b_exists = file_exists_on_vita(raw[bi].path);
            int c_exists = file_exists_on_vita(raw[ci].path);
            if (!b_exists && c_exists) { best = ci; continue; }
            if (b_exists && !c_exists) continue;
            /* Both or neither exist — use enabled state */
            if (!raw[bi].enabled && raw[ci].enabled) { best = ci; continue; }
            if (raw[bi].enabled && !raw[ci].enabled) continue;
            /* Both same state — prefer ur0: */
            if (strncmp(raw[ci].path, "ur0:", 4) == 0 &&
                strncmp(raw[bi].path, "ur0:", 4) != 0) { best = ci; }
        }
        /* All non-best matches go into drop list */
        for (int m = 0; m < mc && pair_count < MAX_PLUGINS; m++) {
            if (matches[m] != best) {
                snprintf(pairs[pair_count].keep, MAX_LINE, "%s", raw[best].path);
                snprintf(pairs[pair_count].drop, MAX_LINE, "%s", raw[matches[m]].path);
                pair_count++;
            }
        }
    }

    if (pair_count == 0) { free(original); return 0; }

    /* --- Pass 2: Rewrite config dropping the identified duplicates --- */
    char *out = malloc(len + 4);
    if (!out) { free(original); return -1; }
    int out_pos = 0;
    int removed = 0;

    /* Need a fresh copy since strtok destroyed original */
    char *orig2 = malloc(len + 1);
    if (!orig2) { free(out); free(original); return -1; }
    memcpy(orig2, original, len + 1);

    line = strtok(orig2, "\n");
    while (line) {
        int ll = strlen(line);
        if (ll > 0 && line[ll-1] == '\r') line[--ll] = '\0';
        int commented = (ll > 0 && line[0] == '#');
        char *rl = commented ? line + 1 : line;
        while (*rl == ' ' || *rl == '\t') rl++;

        int skip = 0;
        if (is_plugin_line(rl)) {
            for (int p = 0; p < pair_count; p++) {
                if (strcmp(rl, pairs[p].drop) == 0) {
                    skip = 1;
                    removed++;
                    break;
                }
            }
        }
        if (!skip) {
            memcpy(out + out_pos, line, ll);
            out_pos += ll;
            out[out_pos++] = '\n';
        }
        line = strtok(NULL, "\n");
    }
    out[out_pos] = '\0';
    free(orig2);
    free(original);

    if (removed > 0)
        write_file(config_path, out, out_pos);
    free(out);
    return removed;
}

/*
 * plugins_clean_config — Full config repair:
 *   1. Reads every plugin entry (including commented ones)
 *   2. Deduplicates by filename using file-existence rules
 *   3. Re-enables any plugin whose file actually exists on disk
 *   4. Writes a clean config.txt preserving section structure
 *
 * This fixes configs corrupted by AutoPlugin2's repair feature
 * which mass-comments all entries and introduces duplicates.
 *
 * Returns: number of changes made (deduped + re-enabled), or -1 on error.
 */
int plugins_clean_config(void) {
    const char *config_path = get_active_config();
    if (!config_path) return -1;

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

    CleanEntry entries[MAX_PLUGINS * 2];
    int entry_count = 0;
    char section_order[16][64];
    int  section_count = 0;

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
            if (!found && section_count < 16)
                snprintf(section_order[section_count++], 64, "%s", current_section);
        } else if (current_section[0]) {
            int commented = (stripped[0] == '#');
            char *raw = commented ? stripped + 1 : stripped;
            while (*raw == ' ' || *raw == '\t') raw++;

            if (is_plugin_line(raw)) {
                const char *name = basename_of(raw);
                /* Check for existing entry with same filename */
                int existing = -1;
                for (int i = 0; i < entry_count; i++)
                    if (strcmp(basename_of(entries[i].path), name) == 0) { existing = i; break; }

                if (existing < 0 && entry_count < MAX_PLUGINS * 2) {
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

    write_file(config_path, out, out_pos);
    free(out);
    free(original);
    return changes;
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
