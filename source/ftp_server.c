/*
 * ftp_server.c — Embedded FTP server for PS Vita Recovery Menu
 *
 * Implements a minimal RFC 959 FTP server.
 * Serves all accessible Vita mount points as top-level directories.
 * Supports: LIST, RETR, STOR, DELE, MKD, RMD, RNFR/RNTO, TYPE, PASV,
 *           PORT, PWD, CWD, CDUP, SYST, FEAT, NOOP, QUIT, SIZE, MDTM.
 *
 * Tested with: FileZilla, WinSCP, Windows Explorer FTP, macOS Finder.
 * Port: 1337 (control), passive data ports: 1338–1340.
 *
 * Architecture:
 *   - One listener thread accepts control connections.
 *   - Each client connection gets its own session thread.
 *   - Data transfers use PASV mode (active PORT also supported).
 *   - Max simultaneous clients: 3.
 */

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/ctrl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Net init memory pool */
static char g_net_memory[1 * 1024 * 1024];   /* 1 MB */
#include "ftp_server.h"
#include "fs_policy.h"
#include "display.h"
#include "draw.h"
#include "menu.h"

/* ── Config ──────────────────────────────────────────────────────────────── */
#define FTP_PORT        1337
#define FTP_PASV_BASE   1338
#define FTP_MAX_CLIENTS 3
#define FTP_BUF_SIZE    (64 * 1024)
#define FTP_CMD_SIZE    512
#define FTP_PATH_SIZE   256

/* ── Mount points exposed as virtual root directories ─────────────────────── */
/* v1.1.9: system drives listed too, but READ-ONLY (fs_policy.c) */
static const char *g_mounts[] = {
    "ux0:", "ur0:", "imc0:", "uma0:", "xmc0:", "grw0:", "host0:", "psp0:",
    "os0:", "vs0:", "sa0:", "pd0:", "tm0:", "ud0:", "vd0:"
};
#define MOUNT_COUNT  ((int)(sizeof(g_mounts) / sizeof(g_mounts[0])))

/* ── Server state ─────────────────────────────────────────────────────────── */
static volatile int  g_running     = 0;
static volatile int  g_clients     = 0;   /* active FTP client sessions */
static SceUID        g_listen_thid = -1;
static int           g_listen_sock = -1;
static char          g_server_ip[32];

/* ── Per-client session ───────────────────────────────────────────────────── */
typedef struct {
    int  ctrl_sock;
    int  data_sock;       /* passive listener */
    int  data_conn;       /* active data connection */
    char cwd[FTP_PATH_SIZE];
    char rename_from[FTP_PATH_SIZE];
    int  binary_mode;
    int  pasv_port;
    /* active mode (PORT) */
    int  use_port;
    char port_ip[32];
    int  port_port;
} FtpSession;

/* ── Networking helpers ───────────────────────────────────────────────────── */
static int net_send_str(int sock, const char *s) {
    return sceNetSend(sock, s, strlen(s), 0);
}

static int net_send_resp(int sock, int code, const char *msg) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%d %s\r\n", code, msg);
    return net_send_str(sock, buf);
}

/* Read one line (CR LF or LF) from socket into buf. Returns length or <=0. */
static int net_read_line(int sock, char *buf, int max) {
    int total = 0;
    while (total < max - 1) {
        char c;
        int r = sceNetRecv(sock, &c, 1, 0);
        if (r <= 0) return r;
        if (c == '\r') continue;
        if (c == '\n') break;
        buf[total++] = c;
    }
    buf[total] = '\0';
    return total;
}

/* ── Path helpers ─────────────────────────────────────────────────────────── */

/*
 * Virtual root "/" maps to the list of mount points.
 * "/ux0:" maps to "ux0:" on the Vita filesystem.
 * "/ux0:/path" maps to "ux0:/path".
 */
static void vpath_to_real(const char *vpath, char *real, int real_sz) {
    if (strcmp(vpath, "/") == 0 || vpath[0] == '\0') {
        /* Virtual root — no single real path */
        snprintf(real, real_sz, "/");
        return;
    }
    /* Strip leading slash */
    const char *p = (vpath[0] == '/') ? vpath + 1 : vpath;
    /* p should now start with "ux0:" or similar */
    snprintf(real, real_sz, "%s", p);
}

static void real_to_vpath(const char *real, char *vpath, int vpath_sz) {
    if (strcmp(real, "/") == 0) {
        snprintf(vpath, vpath_sz, "/");
        return;
    }
    snprintf(vpath, vpath_sz, "/%s", real);
}

/* Resolve a client path argument relative to cwd */
static void resolve_path(FtpSession *s, const char *arg, char *out, int out_sz) {
    if (!arg || arg[0] == '\0') {
        snprintf(out, out_sz, "%s", s->cwd);
        return;
    }
    if (arg[0] == '/') {
        /* Absolute virtual path */
        snprintf(out, out_sz, "%s", arg);
    } else {
        /* Relative to cwd */
        if (strcmp(s->cwd, "/") == 0)
            snprintf(out, out_sz, "/%s", arg);
        else
            snprintf(out, out_sz, "%s/%s", s->cwd, arg);
    }
}

/* ── Data connection ──────────────────────────────────────────────────────── */
static int open_data_conn(FtpSession *s) {
    if (s->use_port) {
        /* Active mode — connect to client */
        int sock = sceNetSocket("ftp_data_active",
                                SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
        if (sock < 0) return -1;
        SceNetSockaddrIn addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = SCE_NET_AF_INET;
        addr.sin_port   = sceNetHtons((unsigned short)s->port_port);
        sceNetInetPton(SCE_NET_AF_INET, s->port_ip, &addr.sin_addr);
        if (sceNetConnect(sock, (SceNetSockaddr *)&addr, sizeof(addr)) < 0) {
            sceNetSocketClose(sock);
            return -1;
        }
        return sock;
    } else {
        /* Passive mode — accept waiting connection */
        SceNetSockaddrIn addr;
        unsigned int len = sizeof(addr);
        int conn = sceNetAccept(s->data_sock,
                                (SceNetSockaddr *)&addr, &len);
        return conn;
    }
}

static void close_data_conn(FtpSession *s, int data_conn) {
    if (data_conn >= 0) sceNetSocketClose(data_conn);
    if (!s->use_port && s->data_sock >= 0) {
        sceNetSocketClose(s->data_sock);
        s->data_sock = -1;
    }
}

/* ── LIST helper ──────────────────────────────────────────────────────────── */
static void send_list(FtpSession *s, int data_conn, const char *vpath) {
    char real[FTP_PATH_SIZE];
    vpath_to_real(vpath, real, sizeof(real));

    char line[512];

    if (strcmp(real, "/") == 0) {
        /* Virtual root — list mount points */
        for (int i = 0; i < MOUNT_COUNT; i++) {
            /* Check if mount point is accessible */
            SceIoStat st;
            int ok = (sceIoGetstat(g_mounts[i], &st) >= 0);
            if (!ok) {
                SceUID dfd = sceIoDopen(g_mounts[i]);   /* v1.1.9: was never closed */
                if (dfd >= 0) { ok = 1; sceIoDclose(dfd); }
            }
            if (ok) {
                snprintf(line, sizeof(line),
                         "drwxr-xr-x 1 vita vita 0 Jan 01 00:00 %s\r\n",
                         g_mounts[i]);
                sceNetSend(data_conn, line, strlen(line), 0);
            }
        }
        return;
    }

    SceUID dd = sceIoDopen(real);
    if (dd < 0) return;

    SceIoDirent entry;
    while (sceIoDread(dd, &entry) > 0) {
        if (entry.d_name[0] == '\0') continue;

        int is_dir = SCE_S_ISDIR(entry.d_stat.st_mode);
        unsigned int size = (unsigned int)entry.d_stat.st_size;

        /* Date fields from SceDateTime */
        SceDateTime *t = &entry.d_stat.st_mtime;
        static const char *months[] = {
            "Jan","Feb","Mar","Apr","May","Jun",
            "Jul","Aug","Sep","Oct","Nov","Dec"
        };
        int mon = (t->month >= 1 && t->month <= 12) ? t->month - 1 : 0;

        snprintf(line, sizeof(line),
                 "%s 1 vita vita %10u %s %02d %02d:%02d %s\r\n",
                 is_dir ? "drwxr-xr-x" : "-rw-r--r--",
                 size,
                 months[mon], t->day,
                 t->hour, t->minute,
                 entry.d_name);
        sceNetSend(data_conn, line, strlen(line), 0);
    }
    sceIoDclose(dd);
}

/* ── RETR — send file to client ───────────────────────────────────────────── */
static void send_file(int data_conn, const char *real_path) {
    SceUID fd = sceIoOpen(real_path, SCE_O_RDONLY, 0);
    if (fd < 0) return;

    static char buf[FTP_BUF_SIZE];
    int n;
    while ((n = sceIoRead(fd, buf, sizeof(buf))) > 0)
        sceNetSend(data_conn, buf, n, 0);
    sceIoClose(fd);
}

/* ── STOR — receive file from client ─────────────────────────────────────── */
static void recv_file(int data_conn, const char *real_path) {
    SceUID fd = sceIoOpen(real_path,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return;

    static char buf[FTP_BUF_SIZE];
    int n;
    while ((n = sceNetRecv(data_conn, buf, sizeof(buf), 0)) > 0)
        sceIoWrite(fd, buf, n);
    sceIoClose(fd);
}

/* ── Session thread ───────────────────────────────────────────────────────── */
static int ftp_session_thread(SceSize args, void *argp) {
    FtpSession *s = *(FtpSession **)argp;

    __sync_fetch_and_add(&g_clients, 1);

    net_send_resp(s->ctrl_sock, 220,
                  "VitaRecovery FTP Server ready.");

    char cmd_buf[FTP_CMD_SIZE];

    while (1) {
        int r = net_read_line(s->ctrl_sock, cmd_buf, sizeof(cmd_buf));
        if (r <= 0) break;

        /* Split command and argument */
        char cmd[16] = {0};
        char arg[FTP_PATH_SIZE] = {0};
        char *sp = strchr(cmd_buf, ' ');
        if (sp) {
            int clen = (int)(sp - cmd_buf);
            if (clen > 15) clen = 15;
            strncpy(cmd, cmd_buf, clen);
            strncpy(arg, sp + 1, FTP_PATH_SIZE - 1);
        } else {
            strncpy(cmd, cmd_buf, 15);
        }

        /* Uppercase cmd */
        for (int i = 0; cmd[i]; i++)
            if (cmd[i] >= 'a' && cmd[i] <= 'z') cmd[i] -= 32;

        /* ── Command dispatch ── */
        if (strcmp(cmd, "USER") == 0 || strcmp(cmd, "PASS") == 0) {
            net_send_resp(s->ctrl_sock, 230, "Logged in.");

        } else if (strcmp(cmd, "SYST") == 0) {
            net_send_resp(s->ctrl_sock, 215, "UNIX Type: L8");

        } else if (strcmp(cmd, "FEAT") == 0) {
            net_send_str(s->ctrl_sock, "211-Features:\r\n SIZE\r\n MDTM\r\n211 End\r\n");

        } else if (strcmp(cmd, "NOOP") == 0) {
            net_send_resp(s->ctrl_sock, 200, "OK");

        } else if (strcmp(cmd, "TYPE") == 0) {
            s->binary_mode = (arg[0] == 'I');
            net_send_resp(s->ctrl_sock, 200, "Type set.");

        } else if (strcmp(cmd, "PWD") == 0 || strcmp(cmd, "XPWD") == 0) {
            char msg[FTP_PATH_SIZE + 8];
            snprintf(msg, sizeof(msg), "\"%s\"", s->cwd);
            net_send_resp(s->ctrl_sock, 257, msg);

        } else if (strcmp(cmd, "CWD") == 0 || strcmp(cmd, "XCWD") == 0) {
            char newpath[FTP_PATH_SIZE];
            resolve_path(s, arg, newpath, sizeof(newpath));
            char real[FTP_PATH_SIZE];
            vpath_to_real(newpath, real, sizeof(real));

            int ok = 0;
            if (strcmp(real, "/") == 0) {
                ok = 1;
            } else {
                SceUID dd = sceIoDopen(real);
                if (dd >= 0) { sceIoDclose(dd); ok = 1; }
            }
            if (ok) {
                snprintf(s->cwd, FTP_PATH_SIZE, "%s", newpath);
                net_send_resp(s->ctrl_sock, 250, "Directory changed.");
            } else {
                net_send_resp(s->ctrl_sock, 550, "No such directory.");
            }

        } else if (strcmp(cmd, "CDUP") == 0 || strcmp(cmd, "XCUP") == 0) {
            /* Walk back one directory level */
            char *last = strrchr(s->cwd, '/');
            if (last && last != s->cwd) {
                *last = '\0';
            } else {
                s->cwd[0] = '/'; s->cwd[1] = '\0';
            }
            net_send_resp(s->ctrl_sock, 200, "OK");

        } else if (strcmp(cmd, "PASV") == 0) {
            /* Open passive listener on FTP_PASV_BASE */
            if (s->data_sock >= 0) {
                sceNetSocketClose(s->data_sock);
                s->data_sock = -1;
            }
            s->data_sock = sceNetSocket("ftp_pasv",
                                        SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
            SceNetSockaddrIn addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = SCE_NET_AF_INET;
            addr.sin_port   = sceNetHtons((unsigned short)s->pasv_port);
            sceNetInetPton(SCE_NET_AF_INET, g_server_ip, &addr.sin_addr);
            sceNetBind(s->data_sock, (SceNetSockaddr *)&addr, sizeof(addr));
            sceNetListen(s->data_sock, 1);
            s->use_port = 0;

            /* Reply: 227 (h1,h2,h3,h4,p1,p2) */
            unsigned char *ip = (unsigned char *)&addr.sin_addr;
            /* Re-read bound IP */
            SceNetSockaddrIn bound;
            unsigned int blen = sizeof(bound);
            sceNetGetsockname(s->data_sock,
                              (SceNetSockaddr *)&bound, &blen);
            unsigned char *bip = (unsigned char *)&bound.sin_addr;
            int p1 = s->pasv_port >> 8;
            int p2 = s->pasv_port & 0xFF;
            char msg[64];
            snprintf(msg, sizeof(msg),
                     "Entering Passive Mode (%d,%d,%d,%d,%d,%d)",
                     bip[0], bip[1], bip[2], bip[3], p1, p2);
            net_send_resp(s->ctrl_sock, 227, msg);

        } else if (strcmp(cmd, "PORT") == 0) {
            /* PORT h1,h2,h3,h4,p1,p2 */
            int h[4], p[2];
            if (sscanf(arg, "%d,%d,%d,%d,%d,%d",
                       &h[0],&h[1],&h[2],&h[3],&p[0],&p[1]) == 6) {
                snprintf(s->port_ip, sizeof(s->port_ip),
                         "%d.%d.%d.%d", h[0],h[1],h[2],h[3]);
                s->port_port = p[0] * 256 + p[1];
                s->use_port  = 1;
                net_send_resp(s->ctrl_sock, 200, "PORT command OK.");
            } else {
                net_send_resp(s->ctrl_sock, 500, "Bad PORT.");
            }

        } else if (strcmp(cmd, "LIST") == 0 || strcmp(cmd, "NLST") == 0) {
            net_send_resp(s->ctrl_sock, 150, "Opening data connection.");
            int dc = open_data_conn(s);
            if (dc < 0) {
                net_send_resp(s->ctrl_sock, 425, "Cannot open data connection.");
            } else {
                char listpath[FTP_PATH_SIZE];
                if (arg[0])
                    resolve_path(s, arg, listpath, sizeof(listpath));
                else
                    snprintf(listpath, sizeof(listpath), "%s", s->cwd);
                send_list(s, dc, listpath);
                close_data_conn(s, dc);
                net_send_resp(s->ctrl_sock, 226, "Transfer complete.");
            }

        } else if (strcmp(cmd, "RETR") == 0) {
            char vpath[FTP_PATH_SIZE];
            resolve_path(s, arg, vpath, sizeof(vpath));
            char real[FTP_PATH_SIZE];
            vpath_to_real(vpath, real, sizeof(real));

            SceIoStat st;
            if (sceIoGetstat(real, &st) < 0) {
                net_send_resp(s->ctrl_sock, 550, "File not found.");
            } else {
                char msg[64];
                snprintf(msg, sizeof(msg),
                         "Opening BINARY data connection (%u bytes).",
                         (unsigned int)st.st_size);
                net_send_resp(s->ctrl_sock, 150, msg);
                int dc = open_data_conn(s);
                if (dc < 0) {
                    net_send_resp(s->ctrl_sock, 425,
                                  "Cannot open data connection.");
                } else {
                    send_file(dc, real);
                    close_data_conn(s, dc);
                    net_send_resp(s->ctrl_sock, 226, "Transfer complete.");
                }
            }

        } else if (strcmp(cmd, "STOR") == 0) {
            char vpath[FTP_PATH_SIZE];
            resolve_path(s, arg, vpath, sizeof(vpath));
            char real[FTP_PATH_SIZE];
            vpath_to_real(vpath, real, sizeof(real));
            if (!fs_path_writable(real)) {
                net_send_resp(s->ctrl_sock, 550, "Read-only drive.");
                close_data_conn(s, -1);   /* drop the prepared PASV socket */
                continue;
            }
            net_send_resp(s->ctrl_sock, 150, "Opening data connection.");
            int dc = open_data_conn(s);
            if (dc < 0) {
                net_send_resp(s->ctrl_sock, 425, "Cannot open data connection.");
            } else {
                recv_file(dc, real);
                close_data_conn(s, dc);
                net_send_resp(s->ctrl_sock, 226, "Transfer complete.");
            }

        } else if (strcmp(cmd, "DELE") == 0) {
            char vpath[FTP_PATH_SIZE];
            resolve_path(s, arg, vpath, sizeof(vpath));
            char real[FTP_PATH_SIZE];
            vpath_to_real(vpath, real, sizeof(real));
            if (!fs_path_writable(real))
                net_send_resp(s->ctrl_sock, 550, "Read-only drive.");
            else if (sceIoRemove(real) >= 0)
                net_send_resp(s->ctrl_sock, 250, "File deleted.");
            else
                net_send_resp(s->ctrl_sock, 550, "Delete failed.");

        } else if (strcmp(cmd, "MKD") == 0 || strcmp(cmd, "XMKD") == 0) {
            char vpath[FTP_PATH_SIZE];
            resolve_path(s, arg, vpath, sizeof(vpath));
            char real[FTP_PATH_SIZE];
            vpath_to_real(vpath, real, sizeof(real));
            if (!fs_path_writable(real)) {
                net_send_resp(s->ctrl_sock, 550, "Read-only drive.");
            } else if (sceIoMkdir(real, 0777) >= 0) {
                char msg[FTP_PATH_SIZE + 4];
                snprintf(msg, sizeof(msg), "\"%s\" created.", vpath);
                net_send_resp(s->ctrl_sock, 257, msg);
            } else {
                net_send_resp(s->ctrl_sock, 550, "MKD failed.");
            }

        } else if (strcmp(cmd, "RMD") == 0 || strcmp(cmd, "XRMD") == 0) {
            char vpath[FTP_PATH_SIZE];
            resolve_path(s, arg, vpath, sizeof(vpath));
            char real[FTP_PATH_SIZE];
            vpath_to_real(vpath, real, sizeof(real));
            if (!fs_path_writable(real))
                net_send_resp(s->ctrl_sock, 550, "Read-only drive.");
            else if (sceIoRmdir(real) >= 0)
                net_send_resp(s->ctrl_sock, 250, "Directory removed.");
            else
                net_send_resp(s->ctrl_sock, 550, "RMD failed.");

        } else if (strcmp(cmd, "RNFR") == 0) {
            char vpath[FTP_PATH_SIZE];
            resolve_path(s, arg, vpath, sizeof(vpath));
            vpath_to_real(vpath, s->rename_from, sizeof(s->rename_from));
            net_send_resp(s->ctrl_sock, 350, "Ready for RNTO.");

        } else if (strcmp(cmd, "RNTO") == 0) {
            char vpath[FTP_PATH_SIZE];
            resolve_path(s, arg, vpath, sizeof(vpath));
            char real_to[FTP_PATH_SIZE];
            vpath_to_real(vpath, real_to, sizeof(real_to));
            if (!fs_path_writable(s->rename_from) || !fs_path_writable(real_to))
                net_send_resp(s->ctrl_sock, 550, "Read-only drive.");
            else if (sceIoRename(s->rename_from, real_to) >= 0)
                net_send_resp(s->ctrl_sock, 250, "Rename OK.");
            else
                net_send_resp(s->ctrl_sock, 550, "Rename failed.");

        } else if (strcmp(cmd, "SIZE") == 0) {
            char vpath[FTP_PATH_SIZE];
            resolve_path(s, arg, vpath, sizeof(vpath));
            char real[FTP_PATH_SIZE];
            vpath_to_real(vpath, real, sizeof(real));
            SceIoStat st;
            if (sceIoGetstat(real, &st) >= 0) {
                char msg[32];
                snprintf(msg, sizeof(msg), "%u", (unsigned int)st.st_size);
                net_send_resp(s->ctrl_sock, 213, msg);
            } else {
                net_send_resp(s->ctrl_sock, 550, "File not found.");
            }

        } else if (strcmp(cmd, "MDTM") == 0) {
            /* Return a fixed timestamp — full mtime parsing not critical */
            net_send_resp(s->ctrl_sock, 213, "20000101000000");

        } else if (strcmp(cmd, "QUIT") == 0 || strcmp(cmd, "BYE") == 0) {
            net_send_resp(s->ctrl_sock, 221, "Bye.");
            break;

        } else {
            net_send_resp(s->ctrl_sock, 502, "Command not implemented.");
        }
    }

    /* Cleanup */
    if (s->data_sock >= 0) sceNetSocketClose(s->data_sock);
    sceNetSocketClose(s->ctrl_sock);
    free(s);
    if (__sync_sub_and_fetch(&g_clients, 1) < 0) g_clients = 0;
    return sceKernelExitDeleteThread(0);
}

/* ── Listener thread ──────────────────────────────────────────────────────── */
static int ftp_listener_thread(SceSize args, void *argp) {
    g_listen_sock = sceNetSocket("ftp_listen",
                                 SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (g_listen_sock < 0) return sceKernelExitDeleteThread(-1);

    int yes = 1;
    sceNetSetsockopt(g_listen_sock, SCE_NET_SOL_SOCKET,
                     SCE_NET_SO_REUSEADDR, &yes, sizeof(yes));

    SceNetSockaddrIn addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port   = sceNetHtons(FTP_PORT);
    sceNetBind(g_listen_sock, (SceNetSockaddr *)&addr, sizeof(addr));
    sceNetListen(g_listen_sock, FTP_MAX_CLIENTS);

    /* Set a 100ms accept timeout so the loop can check g_running */
    int timeout_ms = 100;
    sceNetSetsockopt(g_listen_sock, SCE_NET_SOL_SOCKET,
                     SCE_NET_SO_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));

    int client_idx = 0;

    while (g_running) {
        SceNetSockaddrIn client_addr;
        unsigned int clen = sizeof(client_addr);
        int csock = sceNetAccept(g_listen_sock,
                                 (SceNetSockaddr *)&client_addr, &clen);
        if (csock < 0) {
            /* Timeout or transient error — loop and check g_running */
            continue;
        }

        FtpSession *sess = malloc(sizeof(FtpSession));
        if (!sess) { sceNetSocketClose(csock); continue; }
        memset(sess, 0, sizeof(FtpSession));
        sess->ctrl_sock  = csock;
        sess->data_sock  = -1;
        sess->data_conn  = -1;
        sess->cwd[0]     = '/';
        sess->cwd[1]     = '\0';
        sess->binary_mode= 1;
        sess->use_port   = 0;
        /* Assign a unique passive port per client */
        sess->pasv_port  = FTP_PASV_BASE + (client_idx % FTP_MAX_CLIENTS);
        client_idx++;

        char tname[32];
        snprintf(tname, sizeof(tname), "ftp_sess_%d", client_idx);
        SceUID thid = sceKernelCreateThread(tname,
                          ftp_session_thread, 0x40, 0x10000, 0, 0, NULL);
        if (thid >= 0)
            sceKernelStartThread(thid, sizeof(FtpSession *), &sess);
        else
            free(sess);
    }

    sceNetSocketClose(g_listen_sock);
    g_listen_sock = -1;
    return sceKernelExitDeleteThread(0);
}

/* ── Start / Stop ─────────────────────────────────────────────────────────── */
static int g_net_inited = 0;

static int ftp_net_init(void) {
    if (g_net_inited) return 0;
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    SceNetInitParam param;
    memset(&param, 0, sizeof(param));
    param.memory      = g_net_memory;
    param.size        = sizeof(g_net_memory);
    param.flags       = 0;
    int r = sceNetInit(&param);
    if (r < 0 && r != 0x80400002) return r;  /* 0x80400002 = already inited */
    r = sceNetCtlInit();
    if (r < 0 && r != 0x80400002) return r;
    g_net_inited = 1;
    return 0;
}

int net_ensure_init(void) { return ftp_net_init(); }

static int ftp_start(void) {
    /* Init network stack if needed */
    if (ftp_net_init() < 0) return -1;

    /* Get WiFi IP */
    SceNetCtlInfo info;
    memset(&info, 0, sizeof(info));
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0)
        return -1;
    snprintf(g_server_ip, sizeof(g_server_ip), "%s", info.ip_address);

    g_clients = 0;
    g_running = 1;
    g_listen_thid = sceKernelCreateThread("ftp_listener",
                        ftp_listener_thread, 0x40, 0x8000, 0, 0, NULL);
    if (g_listen_thid < 0) { g_running = 0; return -1; }
    sceKernelStartThread(g_listen_thid, 0, NULL);
    return 0;
}

static void ftp_stop(void) {
    g_running = 0;
    if (g_listen_sock >= 0) {
        sceNetSocketClose(g_listen_sock);
        g_listen_sock = -1;
    }
    if (g_listen_thid >= 0) {
        sceKernelWaitThreadEnd(g_listen_thid, NULL, NULL);
        g_listen_thid = -1;
    }
}

/* ── Screen ───────────────────────────────────────────────────────────────── */
void ftp_screen_run(void) {
    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));
    do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);

    /* Init network stack, then check WiFi */
    ftp_net_init();
    SceNetCtlInfo info;
    int wifi_ok = (sceNetCtlInetGetInfo(
                       SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0);

    if (!wifi_ok) {
        /* No WiFi — show error and return */
        while (1) {
            display_start();
            display_clear(COLOR_BG);
            display_rect(0, 0, SCREEN_W, TITLE_LINE, COLOR_TITLE_BG);
            display_app_title(6);
            display_text(MENU_X, 26, COLOR_YELLOW, "FTP Server");
            display_hline(0, TITLE_LINE, SCREEN_W, COLOR_GREEN);
            display_text(MENU_X, TITLE_H + 30, COLOR_RED,
                         "WiFi is not connected.");
            display_text(MENU_X, TITLE_H + 56, COLOR_DIM,
                         "Connect to a WiFi network first,");
            display_text(MENU_X, TITLE_H + 80, COLOR_DIM,
                         "then return to this screen.");
            display_rect(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, COLOR_TITLE_BG);
            display_hline(0, FOOTER_Y, SCREEN_W, COLOR_GREEN);
            display_text(MENU_X, FOOTER_Y + 7, COLOR_DIM, "[O] Back");
            display_end();
            sceCtrlPeekBufferPositive(0, &ctrl, 1);
            if ((ctrl.buttons & SCE_CTRL_CIRCLE) &&
                !(old.buttons & SCE_CTRL_CIRCLE)) break;
            old = ctrl;
        }
        return;
    }

    /* Start server */
    if (ftp_start() < 0) {
        while (1) {
            display_start();
            display_clear(COLOR_BG);
            display_rect(0, 0, SCREEN_W, TITLE_LINE, COLOR_TITLE_BG);
            display_app_title(6);
            display_text(MENU_X, 26, COLOR_YELLOW, "FTP Server");
            display_hline(0, TITLE_LINE, SCREEN_W, COLOR_GREEN);
            display_text(MENU_X, TITLE_H + 30, COLOR_RED,
                         "Failed to start FTP server.");
            display_rect(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, COLOR_TITLE_BG);
            display_hline(0, FOOTER_Y, SCREEN_W, COLOR_GREEN);
            display_text(MENU_X, FOOTER_Y + 7, COLOR_DIM, "[O] Back");
            display_end();
            sceCtrlPeekBufferPositive(0, &ctrl, 1);
            if ((ctrl.buttons & SCE_CTRL_CIRCLE) &&
                !(old.buttons & SCE_CTRL_CIRCLE)) break;
            old = ctrl;
        }
        return;
    }

    /* Running screen */
    char url[64];
    snprintf(url, sizeof(url), "ftp://%s:%d", g_server_ip, FTP_PORT);

    while (1) {
        display_start();
        display_clear(COLOR_BG);

        /* Title bar */
        display_rect(0, 0, SCREEN_W, TITLE_LINE, COLOR_TITLE_BG);
        display_app_title(6);
        display_text(MENU_X, 26, COLOR_GREEN,  "FTP Server  [ RUNNING ]");
        display_hline(0, TITLE_LINE, SCREEN_W, COLOR_GREEN);

        int y = TITLE_H + 24;

        /* Big URL box */
        int bx = MENU_X - 8;
        int bw = SCREEN_W - bx * 2;
        display_rect(bx, y, bw, LINE_H * 2 + 12, RGBA8(0, 30, 0, 255));
        display_rect(bx, y, bw, 2, COLOR_GREEN);
        display_text(MENU_X, y + 6,          COLOR_DIM,   "Connect to:");
        /* Red = no client connected, green = at least one client connected */
        display_text(MENU_X, y + 6 + LINE_H,
                     g_clients > 0 ? COLOR_GREEN : COLOR_RED, url);
        y += LINE_H * 2 + 20;

        display_hline(0, y, SCREEN_W, RGBA8(40, 40, 40, 255));
        y += 10;

        /* Accessible partitions */
        display_text(MENU_X, y, COLOR_DIM, "Accessible partitions:");
        y += LINE_H + 4;

        for (int i = 0; i < MOUNT_COUNT; i++) {
            SceIoStat st;
            int ok = (sceIoGetstat(g_mounts[i], &st) >= 0);
            char row[48];
            snprintf(row, sizeof(row), "  %s", g_mounts[i]);
            display_text(MENU_X, y, ok ? COLOR_GREEN : COLOR_DIM, row);
            display_text(MENU_X + 160, y,
                         ok ? COLOR_GREEN : COLOR_DIM,
                         ok ? "accessible" : "not mounted");
            y += LINE_H;
        }

        y += 8;
        display_hline(0, y, SCREEN_W, RGBA8(40, 40, 40, 255));
        y += 10;

        /* Usage hint */
        display_text(MENU_X, y,          COLOR_DIM,
                     "Open FileZilla, WinSCP, or Windows Explorer");
        display_text(MENU_X, y + LINE_H, COLOR_DIM,
                     "and connect using the address above.");
        display_text(MENU_X, y + LINE_H * 2, COLOR_DIM,
                     "No username or password required.");

        /* Footer */
        display_rect(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, COLOR_TITLE_BG);
        display_hline(0, FOOTER_Y, SCREEN_W, COLOR_GREEN);
        display_text(MENU_X, FOOTER_Y + 7, COLOR_DIM,
                     "[O] Stop server and go back");
        display_end();

        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if ((ctrl.buttons & SCE_CTRL_CIRCLE) &&
            !(old.buttons & SCE_CTRL_CIRCLE)) break;
        old = ctrl;
    }

    ftp_stop();
}
