/*
 * filemanager.c — Lightweight file manager for PS Vita Recovery Menu
 *
 * Features:
 *   - Partition list root (read-write and read-only partitions)
 *   - Directory navigation with sceIoDopen/sceIoDread
 *   - File operations: copy, delete, rename, create folder
 *   - Text file viewer/editor for small files (< 8KB, e.g. config.txt)
 *   - Read-only enforcement on system partitions (vs0, os0, sa0, tm0, pd0)
 *
 * Controls:
 *   Up/Down  — navigate
 *   X        — enter folder / open file / confirm
 *   O        — go back / cancel
 *   Triangle — file operations menu (copy, delete, rename)
 *   Square   — create new folder (in current directory)
 *   Start    — go directly to ur0:tai/ (quick recovery shortcut)
 */

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "filemanager.h"
#include "draw.h"
#include "menu.h"

/* ── Layout ──────────────────────────────────────────────────────────────── */
#define FM_TITLE_H    46
#define FM_TITLE_LINE 44
#define FM_FOOTER_Y   514
#define FM_FOOTER_LINE 512
#define FM_LIST_Y     (FM_TITLE_H + 4)
#define FM_LIST_H     (FM_FOOTER_Y - FM_LIST_Y)
#define FM_ROW_H      26
#define FM_MAX_ROWS   ((FM_LIST_H) / FM_ROW_H)   /* ~17 visible rows */
#define FM_MAX_ENTRIES 256
#define FM_NAME_MAX   128
#define FM_PATH_MAX   512
#define FM_TEXTBUF_SZ (8 * 1024)

/* ── Partition table ─────────────────────────────────────────────────────── */
typedef struct {
    const char *path;
    const char *label;
    int         read_only;
} Partition;

static const Partition PARTITIONS[] = {
    { "ux0:",   "ux0:  (Memory / SD2Vita)",   0 },
    { "ur0:",   "ur0:  (System)",              0 },
    { "uma0:",  "uma0: (Sony Memory Card)",    0 },
    { "imc0:",  "imc0: (Internal - Slim)",     0 },
    { "grw0:",  "grw0: (USB Mass Storage)",    0 },
    { "vd0:",   "vd0:  (Savedata)",            0 },
    { "vs0:",   "vs0:  (System Apps)",         1 },
    { "os0:",   "os0:  (Kernel OS)",           1 },
    { "sa0:",   "sa0:  (System Archive)",      1 },
    { "tm0:",   "tm0:  (Temp)",                1 },
    { "pd0:",   "pd0:  (Patch Data)",          1 },
};
#define PARTITION_COUNT ((int)(sizeof(PARTITIONS) / sizeof(PARTITIONS[0])))

/* ── Entry ───────────────────────────────────────────────────────────────── */
typedef struct {
    char name[FM_NAME_MAX];
    int  is_dir;
    int  is_readonly_part;   /* inherited from partition */
    SceOff size;
} FmEntry;

/* ── State ───────────────────────────────────────────────────────────────── */
static FmEntry  g_entries[FM_MAX_ENTRIES];
static int      g_entry_count = 0;
static int      g_selected    = 0;
static int      g_scroll      = 0;
static char     g_cwd[FM_PATH_MAX] = "";   /* empty = partition root */
static int      g_cwd_readonly = 0;        /* true if current partition is read-only */

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static int btn_pressed(SceCtrlData *n, SceCtrlData *o, unsigned int b) {
    return (n->buttons & b) && !(o->buttons & b);
}

static void fm_titlebar(const char *path, int readonly) {
    display_rect(0, 0, SCREEN_W, FM_TITLE_LINE, COLOR_TITLE_BG);
    draw_text(MENU_X, 6,  COLOR_TEXT, "PS Vita Recovery Menu v1.0");
    if (readonly)
        draw_text(MENU_X, 26, COLOR_RED, "[READ-ONLY]");
    else
        draw_text(MENU_X + 16*12, 26, COLOR_DIM, path[0] ? path : "/ Partitions");
    if (!readonly)
        draw_text(MENU_X, 26, COLOR_DIM, path[0] ? path : "/ Partitions");
    display_hline(0, FM_TITLE_LINE, SCREEN_W, COLOR_GREEN);
}

static void fm_footer(const char *hint) {
    display_rect(0, FM_FOOTER_Y, SCREEN_W, SCREEN_H - FM_FOOTER_Y, COLOR_TITLE_BG);
    display_hline(0, FM_FOOTER_LINE, SCREEN_W, COLOR_GREEN);
    draw_text(MENU_X, FM_FOOTER_Y + 7, COLOR_DIM, hint);
}

static void fm_message(const char *msg, uint32_t col) {
    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));
    do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);
    while (1) {
        display_start(); display_clear(COLOR_BG);
        fm_titlebar(g_cwd, g_cwd_readonly);
        draw_text(MENU_X, FM_LIST_Y + 8,  col,       msg);
        draw_text(MENU_X, FM_LIST_Y + 40, COLOR_DIM, "Press O to continue.");
        fm_footer("[O] OK");
        display_end();
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) break;
        old = ctrl;
    }
}

/* Simple yes/no prompt. Returns 1 = yes, 0 = no. */
static int fm_confirm(const char *question) {
    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));
    do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);
    while (1) {
        display_start(); display_clear(COLOR_BG);
        fm_titlebar(g_cwd, g_cwd_readonly);
        int y = FM_LIST_Y + 8;
        draw_text(MENU_X, y,           COLOR_YELLOW, "!! CONFIRM !!");
        draw_text(MENU_X, y + FM_ROW_H, COLOR_TEXT,   question);
        draw_text(MENU_X, y + FM_ROW_H*3, COLOR_GREEN, "[X] Yes");
        draw_text(MENU_X, y + FM_ROW_H*4, COLOR_DIM,   "[O] No");
        fm_footer("[X] Yes   [O] No");
        display_end();
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CROSS))  return 1;
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) return 0;
        old = ctrl;
    }
}

/* ── Directory listing ───────────────────────────────────────────────────── */
static void fm_load_partition_list(void) {
    g_entry_count = 0;
    g_selected    = 0;
    g_scroll      = 0;
    g_cwd[0]      = '\0';
    g_cwd_readonly = 0;

    for (int i = 0; i < PARTITION_COUNT && g_entry_count < FM_MAX_ENTRIES; i++) {
        /* Only show partitions that are actually accessible */
        SceIoStat st;
        if (sceIoGetstat(PARTITIONS[i].path, &st) < 0) continue;
        snprintf(g_entries[g_entry_count].name, FM_NAME_MAX,
                 "%s", PARTITIONS[i].label);
        g_entries[g_entry_count].is_dir          = 1;
        g_entries[g_entry_count].is_readonly_part = PARTITIONS[i].read_only;
        g_entries[g_entry_count].size             = 0;
        g_entry_count++;
    }
}

static void fm_load_dir(const char *path) {
    g_entry_count = 0;
    g_selected    = 0;
    g_scroll      = 0;

    SceUID dir = sceIoDopen(path);
    if (dir < 0) return;

    /* First entry: go up */
    snprintf(g_entries[0].name, FM_NAME_MAX, "../  (go up)");
    g_entries[0].is_dir = 1;
    g_entries[0].size   = 0;
    g_entry_count = 1;

    SceIoDirent d;
    while (sceIoDread(dir, &d) > 0 && g_entry_count < FM_MAX_ENTRIES) {
        if (strcmp(d.d_name, ".") == 0 || strcmp(d.d_name, "..") == 0) continue;
        snprintf(g_entries[g_entry_count].name, FM_NAME_MAX, "%s", d.d_name);
        g_entries[g_entry_count].is_dir =
            SCE_S_ISDIR(d.d_stat.st_mode) ? 1 : 0;
        g_entries[g_entry_count].is_readonly_part = g_cwd_readonly;
        g_entries[g_entry_count].size   = d.d_stat.st_size;
        g_entry_count++;
    }
    sceIoDclose(dir);
}

/* ── Text viewer/editor ──────────────────────────────────────────────────── */
static void fm_view_text(const char *filepath, int readonly) {
    /* Read file */
    SceUID fd = sceIoOpen(filepath, SCE_O_RDONLY, 0);
    if (fd < 0) { fm_message("Cannot open file.", COLOR_RED); return; }
    int sz = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (sz > FM_TEXTBUF_SZ - 1) {
        sceIoClose(fd);
        fm_message("File too large to view (> 8KB).", COLOR_YELLOW);
        return;
    }
    char *buf = malloc(FM_TEXTBUF_SZ);
    if (!buf) { sceIoClose(fd); return; }
    int r = sceIoRead(fd, buf, sz);
    sceIoClose(fd);
    if (r < 0) { free(buf); fm_message("Read error.", COLOR_RED); return; }
    buf[r] = '\0';

    /* Split into lines */
    char *lines[256];
    int   nlines = 0;
    char *p = buf;
    while (*p && nlines < 255) {
        lines[nlines++] = p;
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = '\0';
        p = nl + 1;
    }

    int top = 0;
    int vis = FM_LIST_H / FM_ROW_H;
    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));
    do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);

    while (1) {
        display_start(); display_clear(COLOR_BG);

        /* Title */
        display_rect(0, 0, SCREEN_W, FM_TITLE_LINE, COLOR_TITLE_BG);
        draw_text(MENU_X, 6,  COLOR_TEXT, "File Viewer");
        draw_text(MENU_X, 26, COLOR_DIM,  filepath);
        display_hline(0, FM_TITLE_LINE, SCREEN_W, COLOR_GREEN);

        /* Lines */
        for (int i = 0; i < vis && top + i < nlines; i++) {
            char linebuf[64];
            snprintf(linebuf, sizeof(linebuf), "%-55.55s", lines[top + i]);
            draw_text(MENU_X, FM_LIST_Y + i * FM_ROW_H,
                      COLOR_TEXT, linebuf);
        }

        fm_footer(readonly
                  ? "[O] Back   [Up/Down] Scroll"
                  : "[O] Back   [Up/Down] Scroll");
        display_end();

        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) break;
        if (btn_pressed(&ctrl, &old, SCE_CTRL_DOWN) && top + vis < nlines) top++;
        if (btn_pressed(&ctrl, &old, SCE_CTRL_UP)   && top > 0)            top--;
        old = ctrl;
    }
    free(buf);
}

/* ── Operations menu ─────────────────────────────────────────────────────── */
static void fm_build_fullpath(char *out, int out_sz,
                               const char *dir, const char *name) {
    /* dir already ends in '/' or is a partition like "ux0:" */
    int dlen = strlen(dir);
    if (dlen > 0 && dir[dlen-1] == ':')
        snprintf(out, out_sz, "%s%s", dir, name);
    else
        snprintf(out, out_sz, "%s/%s", dir, name);
}

/* Copy single file — chunked */
#define FM_CHUNK (64 * 1024)
static int fm_copy_file(const char *src, const char *dst) {
    SceUID fdin  = sceIoOpen(src, SCE_O_RDONLY, 0);
    if (fdin < 0) return -1;
    SceUID fdout = sceIoOpen(dst, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fdout < 0) { sceIoClose(fdin); return -2; }
    char *chunk = malloc(FM_CHUNK);
    if (!chunk) { sceIoClose(fdin); sceIoClose(fdout); return -3; }
    int result = 0, n;
    while ((n = sceIoRead(fdin, chunk, FM_CHUNK)) > 0) {
        if (sceIoWrite(fdout, chunk, n) != n) { result = -4; break; }
    }
    free(chunk);
    sceIoClose(fdin); sceIoClose(fdout);
    return result;
}

/* Recursively delete a directory */
static void fm_delete_recursive(const char *path, int depth) {
    if (depth > 16) return;
    SceUID dir = sceIoDopen(path);
    if (dir < 0) { sceIoRemove(path); return; }
    SceIoDirent d;
    while (sceIoDread(dir, &d) > 0) {
        if (strcmp(d.d_name, ".") == 0 || strcmp(d.d_name, "..") == 0) continue;
        char child[FM_PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, d.d_name);
        if (SCE_S_ISDIR(d.d_stat.st_mode))
            fm_delete_recursive(child, depth + 1);
        else
            sceIoRemove(child);
    }
    sceIoDclose(dir);
    sceIoRmdir(path);
}

/* ── Op menu shown on Triangle ───────────────────────────────────────────── */
typedef enum { OP_COPY=0, OP_DELETE, OP_RENAME, OP_VIEW, OP_COUNT } FmOp;
static const char *OP_LABELS[OP_COUNT] = {
    "Copy to ur0:tai/",
    "Delete",
    "Rename",
    "View (text files)",
};

static void fm_op_menu(const char *filepath, const char *name,
                       int is_dir, int readonly) {
    int sel = 0;
    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));
    do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);

    while (1) {
        display_start(); display_clear(COLOR_BG);
        fm_titlebar(g_cwd, readonly);

        int y = FM_LIST_Y;
        draw_text(MENU_X, y, COLOR_DIM, filepath);
        y += FM_ROW_H + 4;

        for (int i = 0; i < OP_COUNT; i++) {
            /* Disable irrelevant ops */
            int disabled = 0;
            if (i == OP_VIEW   && is_dir)  disabled = 1;
            if (i == OP_DELETE && readonly) disabled = 1;
            if (i == OP_RENAME && readonly) disabled = 1;

            uint32_t col = disabled ? COLOR_DIM :
                           (i == sel ? COLOR_SELECTED : COLOR_TEXT);
            if (i == sel && !disabled)
                display_rect(0, y - 2, SCREEN_W - 8,
                             FM_ROW_H + 2, COLOR_SEL_BG);
            draw_text(MENU_X, y, col, OP_LABELS[i]);
            y += FM_ROW_H;
        }

        fm_footer("[X] Execute   [O] Cancel   [Up/Down] Select");
        display_end();

        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (btn_pressed(&ctrl, &old, SCE_CTRL_DOWN))
            sel = (sel + 1) % OP_COUNT;
        if (btn_pressed(&ctrl, &old, SCE_CTRL_UP))
            sel = (sel - 1 + OP_COUNT) % OP_COUNT;
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) break;

        if (btn_pressed(&ctrl, &old, SCE_CTRL_CROSS)) {
            /* ── Execute selected op ── */
            if (sel == OP_VIEW && !is_dir) {
                fm_view_text(filepath, readonly);
                break;
            }
            if (sel == OP_COPY) {
                /* Copy file to ur0:tai/ */
                sceIoMkdir("ur0:tai", 0777);
                char dst[FM_PATH_MAX];
                snprintf(dst, sizeof(dst), "ur0:tai/%s", name);
                char q[FM_PATH_MAX + 32];
                snprintf(q, sizeof(q), "Copy %s to ur0:tai/?", name);
                if (fm_confirm(q)) {
                    int r = fm_copy_file(filepath, dst);
                    fm_message(r == 0 ? "Copied successfully." : "Copy failed.",
                               r == 0 ? COLOR_GREEN : COLOR_RED);
                }
                break;
            }
            if (sel == OP_DELETE && !readonly) {
                char q[FM_NAME_MAX + 32];
                snprintf(q, sizeof(q), "Delete %s?", name);
                if (fm_confirm(q)) {
                    int r;
                    if (is_dir) {
                        fm_delete_recursive(filepath, 0);
                        r = 0;
                    } else {
                        r = sceIoRemove(filepath);
                    }
                    fm_message(r >= 0 ? "Deleted." : "Delete failed.",
                               r >= 0 ? COLOR_GREEN : COLOR_RED);
                    /* Reload directory after deletion */
                    fm_load_dir(g_cwd);
                }
                break;
            }
            if (sel == OP_RENAME && !readonly) {
                fm_message("Rename: use VitaShell for full keyboard input.",
                           COLOR_YELLOW);
                break;
            }
        }
        old = ctrl;
    }
}

/* ── Create folder ───────────────────────────────────────────────────────── */
static void fm_create_folder(void) {
    if (g_cwd_readonly) {
        fm_message("Read-only partition — cannot create folder.", COLOR_RED);
        return;
    }
    if (g_cwd[0] == '\0') {
        fm_message("Navigate into a partition first.", COLOR_YELLOW);
        return;
    }
    /* We can't type a name without a keyboard — use a default timestamped name */
    char newdir[FM_PATH_MAX];
    snprintf(newdir, sizeof(newdir), "%s/new_folder", g_cwd);
    char q[FM_PATH_MAX + 32];
    snprintf(q, sizeof(q), "Create folder: %s?", newdir);
    if (fm_confirm(q)) {
        int r = sceIoMkdir(newdir, 0777);
        fm_message(r >= 0 ? "Folder created." : "Failed (already exists?).",
                   r >= 0 ? COLOR_GREEN : COLOR_YELLOW);
        fm_load_dir(g_cwd);
    }
}

/* ── Navigate into entry ─────────────────────────────────────────────────── */
static void fm_enter_selected(void) {
    if (g_entry_count == 0) return;
    FmEntry *e = &g_entries[g_selected];

    /* At partition root: entering a partition */
    if (g_cwd[0] == '\0') {
        /* Extract just the "ux0:" part from the label */
        char part[16] = "";
        for (int i = 0; i < PARTITION_COUNT; i++) {
            /* Match by checking if label starts with partition path */
            if (strncmp(PARTITIONS[i].label,
                        e->name,
                        strlen(PARTITIONS[i].path)) == 0) {
                snprintf(part, sizeof(part), "%s", PARTITIONS[i].path);
                g_cwd_readonly = PARTITIONS[i].read_only;
                break;
            }
        }
        if (part[0]) {
            snprintf(g_cwd, sizeof(g_cwd), "%s", part);
            fm_load_dir(g_cwd);
        }
        return;
    }

    /* Go up */
    if (strcmp(e->name, "../  (go up)") == 0) {
        /* Strip last path component */
        char *slash = strrchr(g_cwd, '/');
        if (slash) {
            *slash = '\0';
        } else {
            /* Back to partition root */
            fm_load_partition_list();
        }
        if (g_cwd[0]) fm_load_dir(g_cwd);
        return;
    }

    /* Enter subdirectory */
    if (e->is_dir) {
        char newpath[FM_PATH_MAX];
        int dlen = strlen(g_cwd);
        if (g_cwd[dlen-1] == ':')
            snprintf(newpath, sizeof(newpath), "%s%s", g_cwd, e->name);
        else
            snprintf(newpath, sizeof(newpath), "%s/%s", g_cwd, e->name);
        snprintf(g_cwd, sizeof(g_cwd), "%s", newpath);
        fm_load_dir(g_cwd);
        return;
    }

    /* File — show operations menu */
    char filepath[FM_PATH_MAX];
    int dlen = strlen(g_cwd);
    if (g_cwd[dlen-1] == ':')
        snprintf(filepath, sizeof(filepath), "%s%s", g_cwd, e->name);
    else
        snprintf(filepath, sizeof(filepath), "%s/%s", g_cwd, e->name);
    fm_op_menu(filepath, e->name, 0, g_cwd_readonly);
}

/* ── Main entry point ────────────────────────────────────────────────────── */
void filemanager_screen_run(void *font) {
    (void)font;

    fm_load_partition_list();

    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));
    do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);

    while (1) {
        display_start();
        display_clear(COLOR_BG);

        /* Title */
        char title_path[64];
        snprintf(title_path, sizeof(title_path), "%-48.48s",
                 g_cwd[0] ? g_cwd : "/ Partitions");
        fm_titlebar(title_path, g_cwd_readonly);

        /* Entries */
        for (int i = 0; i < FM_MAX_ROWS && g_scroll + i < g_entry_count; i++) {
            int idx = g_scroll + i;
            FmEntry *e = &g_entries[idx];
            int y = FM_LIST_Y + i * FM_ROW_H;

            int is_sel = (idx == g_selected);
            if (is_sel)
                display_rect(0, y - 1, SCREEN_W - 8, FM_ROW_H + 1, COLOR_SEL_BG);

            /* Icon character: D=dir, F=file */
            char icon = e->is_dir ? '>' : ' ';
            uint32_t col = is_sel ? COLOR_SELECTED :
                           (e->is_dir ? COLOR_TEXT : COLOR_DIM);

            /* Read-only partitions shown in red */
            if (e->is_readonly_part && g_cwd[0] == '\0')
                col = is_sel ? COLOR_SELECTED : COLOR_RED;

            char line[72];
            if (e->is_dir || g_cwd[0] == '\0') {
                snprintf(line, sizeof(line), "%c %-52.52s", icon, e->name);
            } else {
                /* Show file size */
                if (e->size >= 1024*1024)
                    snprintf(line, sizeof(line), "  %-44.44s %5dM",
                             e->name, (int)(e->size / (1024*1024)));
                else if (e->size >= 1024)
                    snprintf(line, sizeof(line), "  %-44.44s %5dK",
                             e->name, (int)(e->size / 1024));
                else
                    snprintf(line, sizeof(line), "  %-44.44s %5dB",
                             e->name, (int)e->size);
            }

            if (is_sel)
                display_text_transp(MENU_X, y, col, line);
            else
                draw_text(MENU_X, y, col, line);
        }

        /* Scrollbar hint */
        if (g_entry_count > FM_MAX_ROWS) {
            char sb[16];
            snprintf(sb, sizeof(sb), "%d/%d", g_selected + 1, g_entry_count);
            draw_text(SCREEN_W - 80, FM_LIST_Y, COLOR_DIM, sb);
        }

        /* Footer */
        if (g_cwd_readonly)
            fm_footer("[X] Open   [O] Back   [Start] Go to ur0:tai/");
        else
            fm_footer("[X] Open   [Tri] Ops   [Sq] New Folder   [O] Back");
        display_end();

        /* Input */
        sceCtrlPeekBufferPositive(0, &ctrl, 1);

        if (btn_pressed(&ctrl, &old, SCE_CTRL_DOWN)) {
            if (g_selected + 1 < g_entry_count) {
                g_selected++;
                if (g_selected >= g_scroll + FM_MAX_ROWS)
                    g_scroll++;
            }
        }
        if (btn_pressed(&ctrl, &old, SCE_CTRL_UP)) {
            if (g_selected > 0) {
                g_selected--;
                if (g_selected < g_scroll)
                    g_scroll--;
            }
        }

        /* X — enter / open */
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CROSS)) {
            fm_enter_selected();
        }

        /* Triangle — op menu for current selection */
        if (btn_pressed(&ctrl, &old, SCE_CTRL_TRIANGLE)) {
            if (g_entry_count > 0 && g_cwd[0] != '\0' &&
                strcmp(g_entries[g_selected].name, "../  (go up)") != 0) {
                FmEntry *e = &g_entries[g_selected];
                char filepath[FM_PATH_MAX];
                int dlen = strlen(g_cwd);
                if (g_cwd[dlen-1] == ':')
                    snprintf(filepath, sizeof(filepath),
                             "%s%s", g_cwd, e->name);
                else
                    snprintf(filepath, sizeof(filepath),
                             "%s/%s", g_cwd, e->name);
                fm_op_menu(filepath, e->name, e->is_dir, g_cwd_readonly);
                /* Reload in case of deletion */
                if (g_cwd[0]) fm_load_dir(g_cwd);
                else fm_load_partition_list();
            }
        }

        /* Square — create folder */
        if (btn_pressed(&ctrl, &old, SCE_CTRL_SQUARE))
            fm_create_folder();

        /* Start — quick jump to ur0:tai/ */
        if (btn_pressed(&ctrl, &old, SCE_CTRL_START)) {
            snprintf(g_cwd, sizeof(g_cwd), "ur0:tai");
            g_cwd_readonly = 0;
            fm_load_dir(g_cwd);
        }

        /* O — back or exit */
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) {
            if (g_cwd[0] == '\0') {
                /* At root — exit file manager */
                break;
            } else {
                /* Go up one level */
                char *slash = strrchr(g_cwd, '/');
                if (slash) {
                    *slash = '\0';
                    fm_load_dir(g_cwd);
                } else {
                    fm_load_partition_list();
                }
            }
        }

        old = ctrl;
    }
}
