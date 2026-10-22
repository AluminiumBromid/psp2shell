/*
	PSP2SHELL
	Copyright (C) 2016, Cpasjuste

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Control protocol server, see CTL_PROTOCOL.md.

#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/net/net.h>
#include <psp2/appmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/promoterutil.h>
#include <taihen.h>

#include "libmodule.h"

#include "psp2shell.h"
#include "main.h"
#include "file.h"
#include "utility.h"
#include "md5.h"
#include "ctl.h"
#include "p2s_va.h"

#define CTL_VERSION 1
#define CTL_LINE_MAX 2048
#define CTL_ARGS_MAX 8
#define CTL_CLIENTS_MAX 4
#define CTL_IO_SIZE (32 * 1024)
#define CTL_ERR_PROTO 0x80000000
#define SCE_ERR_EEXIST 0x80010011

// -DCTL_TRACE: step-by-step trace into the kernel log (shows up on the log port)
#ifdef CTL_TRACE
int sceClibPrintf(const char *fmt, ...);
// the pause lets the log pump (5 ms) send each line before the next step runs
#define TRACE(fmt, ...) do { sceClibPrintf("[ctl] " fmt "\n", ##__VA_ARGS__); sceKernelDelayThread(100 * 1000); } while (0)
#else
#define TRACE(fmt, ...)
#endif

typedef struct {
    int sock;
    SceUID thid;
    char buf[CTL_LINE_MAX];
    int len;      // bytes in buf
    int line_end; // end of the last returned line (incl. '\n') in buf
} ctl_conn;

static ctl_conn conns[CTL_CLIENTS_MAX];
static int server_sock = -1;
static SceUID accept_thid = -1;
static int accept_started = 0;
static SceUID cmd_mutex = -1;
static int quit = 0;

// shared by all commands, only used while holding cmd_mutex
static char io_buf[CTL_IO_SIZE];

// legacy command output capture (see ctl_capture)
static SceUID capture_thid = -1;
static ctl_conn *capture_conn = NULL;
static int capture_error = 0;

void p2s_cmd_parse(s_client *client, P2S_CMD *cmd);

// ------------------------------------------------------------------------------------- output

static int send_all(int sock, const void *data, int len) {
    const char *p = data;
    while (len > 0) {
        int sent = sceNetSend(sock, p, (unsigned int) len, 0);
        if (sent <= 0) {
            return -1;
        }
        p += sent;
        len -= sent;
    }
    return 0;
}

static void ctl_printf(ctl_conn *c, const char *fmt, ...) {
    char line[CTL_LINE_MAX];
    P2S_VA_LIST args;
    P2S_VA_START(args, fmt);
    int len = vsnprintf(line, sizeof(line), fmt, P2S_VA_ARGS(args));
    P2S_VA_END(args);
    if (len >= (int) sizeof(line)) {
        len = sizeof(line) - 1;
        line[len - 1] = '\n';
    }
    if (len > 0) {
        send_all(c->sock, line, len);
    }
}

#define ctl_data(c, fmt, ...) ctl_printf(c, "* " fmt "\n", ##__VA_ARGS__)
#define ctl_ok(c) ctl_printf(c, "OK\n")
#define ctl_err(c, code, fmt, ...) ctl_printf(c, "ERR 0x%08X " fmt "\n", (unsigned int) (code), ##__VA_ARGS__)

static const char *u64_str(uint64_t v, char *buf) {
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char) ('0' + v % 10);
        v /= 10;
    } while (v > 0);
    for (int i = 0; i < n; i++) {
        buf[i] = tmp[n - 1 - i];
    }
    buf[n] = '\0';
    return buf;
}

static const char *time_str(const SceDateTime *t, char *buf) {
    snprintf(buf, 40, "%04d-%02d-%02dT%02d:%02d:%02d",
             t->year, t->month, t->day, t->hour, t->minute, t->second);
    return buf;
}

int ctl_capturing() {
    return capture_conn != NULL && sceKernelGetThreadId() == capture_thid;
}

int ctl_capture(int color, const char *text) {
    if (!ctl_capturing()) {
        return 0;
    }
    if (color == COL_RED) {
        capture_error = 1;
    }
    // one data line per non-empty line, whitespace trimmed
    const char *p = text;
    while (*p) {
        const char *end = strchr(p, '\n');
        int len = end ? (int) (end - p) : (int) strlen(p);
        const char *s = p;
        int n = len;
        while (n > 0 && (*s == ' ' || *s == '\t' || *s == '\r')) {
            s++;
            n--;
        }
        while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) {
            n--;
        }
        if (n > 0) {
            // copied by hand: libk's printf has no "%.*s"
            char line[CTL_LINE_MAX];
            int m = n < CTL_LINE_MAX - 3 ? n : CTL_LINE_MAX - 3;
            line[0] = '*';
            line[1] = ' ';
            memcpy(line + 2, s, (size_t) m);
            line[m + 2] = '\n';
            send_all(capture_conn->sock, line, m + 3);
        }
        p += len + (end ? 1 : 0);
    }
    return 1;
}

// -------------------------------------------------------------------------------------- input

// Returns the next line (without '\n') in c->buf, or NULL when the connection is gone.
// Bytes after the line stay buffered from c->buf + c->line_end.

static char *read_line(ctl_conn *c) {
    // drop the previously returned line
    if (c->line_end > 0) {
        memmove(c->buf, c->buf + c->line_end, (size_t) (c->len - c->line_end));
        c->len -= c->line_end;
        c->line_end = 0;
    }
    while (1) {
        char *nl = memchr(c->buf, '\n', (size_t) c->len);
        if (nl != NULL) {
            *nl = '\0';
            c->line_end = (int) (nl - c->buf) + 1;
            if (nl > c->buf && nl[-1] == '\r') {
                nl[-1] = '\0';
            }
            return c->buf;
        }
        if (c->len >= CTL_LINE_MAX - 1) {
            // overlong line: discard it
            c->len = 0;
        }
        int n = sceNetRecv(c->sock, c->buf + c->len, (unsigned int) (CTL_LINE_MAX - 1 - c->len), 0);
        if (n <= 0) {
            return NULL;
        }
        c->len += n;
    }
}

// read exactly len raw bytes, buffered bytes first
static int read_raw(ctl_conn *c, char *dst, int len) {
    int got = 0;
    int buffered = c->len - c->line_end;
    if (buffered > 0) {
        got = buffered < len ? buffered : len;
        memcpy(dst, c->buf + c->line_end, (size_t) got);
        c->line_end += got;
    }
    while (got < len) {
        int n = sceNetRecv(c->sock, dst + got, (unsigned int) (len - got), 0);
        if (n <= 0) {
            return -1;
        }
        got += n;
    }
    return got;
}

// Split line in place. Supports "double quoted" args with \" and \\ escapes.
static int parse_args(char *line, char **argv, int max) {
    int argc = 0;
    char *r = line, *w = line;
    while (*r) {
        while (*r == ' ' || *r == '\t') {
            r++;
        }
        if (!*r) {
            break;
        }
        if (argc == max) {
            return -1;
        }
        argv[argc++] = w;
        if (*r == '"') {
            r++;
            while (*r && *r != '"') {
                if (*r == '\\' && (r[1] == '"' || r[1] == '\\')) {
                    r++;
                }
                *w++ = *r++;
            }
            if (*r != '"') {
                return -1;
            }
            r++;
        } else {
            while (*r && *r != ' ' && *r != '\t') {
                *w++ = *r++;
            }
        }
        if (*r) {
            r++;
        }
        *w++ = '\0';
    }
    return argc;
}

// ----------------------------------------------------------------------------------- commands

static void cmd_ls(ctl_conn *c, const char *path) {
    TRACE("ls: dopen %s", path);
    SceUID dfd = sceIoDopen(path);
    TRACE("ls: dopen -> 0x%08X", dfd);
    if (dfd < 0) {
        ctl_err(c, dfd, "cannot open directory");
        return;
    }
    SceIoDirent d;
    char size[24], mtime[40];
    while (1) {
        TRACE("ls: memset %p size %u", &d, (unsigned int) sizeof(d));
        memset(&d, 0, sizeof(d));
        TRACE("ls: dread");
        int r = sceIoDread(dfd, &d);
        TRACE("ls: dread -> %d", r);
        if (r <= 0) {
            break;
        }
        int dir = SCE_S_ISDIR(d.d_stat.st_mode);
        TRACE("ls: mode 0x%X dir %d", d.d_stat.st_mode, dir);
        u64_str(dir ? 0 : (uint64_t) d.d_stat.st_size, size);
        TRACE("ls: size %s", size);
        time_str(&d.d_stat.st_mtime, mtime);
        TRACE("ls: mtime %s", mtime);
        ctl_data(c, "%s %s %s %s", dir ? "d" : "f", size, mtime, d.d_name);
        TRACE("ls: sent");
    }
    TRACE("ls: done");
    sceIoDclose(dfd);
    ctl_ok(c);
}

static void cmd_stat(ctl_conn *c, const char *path) {
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    int res = sceIoGetstat(path, &st);
    if (res < 0) {
        ctl_err(c, res, "cannot stat");
        return;
    }
    char size[24], mtime[40];
    int dir = SCE_S_ISDIR(st.st_mode);
    ctl_data(c, "%c %s %s", dir ? 'd' : 'f', u64_str(dir ? 0 : (uint64_t) st.st_size, size),
             time_str(&st.st_mtime, mtime));
    ctl_ok(c);
}

static void cmd_md5(ctl_conn *c, const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        ctl_err(c, fd, "cannot open file");
        return;
    }
    md5_ctx ctx;
    md5_init(&ctx);
    int n;
    while ((n = sceIoRead(fd, io_buf, CTL_IO_SIZE)) > 0) {
        md5_update(&ctx, (const uint8_t *) io_buf, (size_t) n);
    }
    sceIoClose(fd);
    if (n < 0) {
        ctl_err(c, n, "read failed");
        return;
    }
    uint8_t digest[16];
    char hex[33];
    md5_final(&ctx, digest);
    for (int i = 0; i < 16; i++) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    ctl_data(c, "%s", hex);
    ctl_ok(c);
}

static int mkdir_parents(const char *path) {
    char tmp[MAX_PATH_LENGTH];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    p2s_removeEndSlash(tmp);

    // skip the device part ("ux0:" or "ux0:/")
    char *p = strchr(tmp, ':');
    p = p ? p + 1 : tmp;
    while (*p == '/') {
        p++;
    }
    for (; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            int res = sceIoMkdir(tmp, 0777);
            *p = '/';
            if (res < 0 && res != (int) SCE_ERR_EEXIST) {
                return res;
            }
        }
    }
    int res = sceIoMkdir(tmp, 0777);
    if (res == (int) SCE_ERR_EEXIST && s_isDir(tmp)) {
        return 0;
    }
    return res;
}

static void cmd_mkdir(ctl_conn *c, int parents, const char *path) {
    int res = parents ? mkdir_parents(path) : sceIoMkdir(path, 0777);
    if (res < 0) {
        ctl_err(c, res, "mkdir failed");
    } else {
        ctl_ok(c);
    }
}

static void cmd_rm(ctl_conn *c, char *path) {
    if (s_isDir(path)) {
        ctl_err(c, CTL_ERR_PROTO, "is a directory, use rmdir");
        return;
    }
    int res = sceIoRemove(path);
    if (res < 0) {
        ctl_err(c, res, "remove failed");
    } else {
        ctl_ok(c);
    }
}

static void cmd_rmdir(ctl_conn *c, int recursive, char *path) {
    if (!s_isDir(path)) {
        ctl_err(c, CTL_ERR_PROTO, "not a directory");
        return;
    }
    int res = recursive ? s_removePath(path, NULL) : sceIoRmdir(path);
    if (recursive ? res != 1 : res < 0) {
        ctl_err(c, res, "rmdir failed");
    } else {
        ctl_ok(c);
    }
}

static void cmd_mv(ctl_conn *c, char *src, char *dst) {
    int res = sceIoRename(src, dst);
    if (res < 0 && s_movePath(src, dst, MOVE_INTEGRATE | MOVE_REPLACE, NULL) != 1) {
        ctl_err(c, res, "move failed");
        return;
    }
    ctl_ok(c);
}

static void cmd_get(ctl_conn *c, const char *path) {
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    int res = sceIoGetstat(path, &st);
    if (res < 0 || SCE_S_ISDIR(st.st_mode)) {
        ctl_err(c, res < 0 ? res : CTL_ERR_PROTO, "not a file");
        return;
    }
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        ctl_err(c, fd, "cannot open file");
        return;
    }
    char size[24];
    ctl_printf(c, "DATA %s\n", u64_str((uint64_t) st.st_size, size));
    uint64_t left = (uint64_t) st.st_size;
    while (left > 0) {
        int want = left < CTL_IO_SIZE ? (int) left : CTL_IO_SIZE;
        int n = sceIoRead(fd, io_buf, want);
        if (n <= 0) {
            // the size was announced: pad so the stream stays in sync, then report
            memset(io_buf, 0, CTL_IO_SIZE);
            while (left > 0) {
                int pad = left < CTL_IO_SIZE ? (int) left : CTL_IO_SIZE;
                send_all(c->sock, io_buf, pad);
                left -= pad;
            }
            sceIoClose(fd);
            ctl_err(c, n, "read failed");
            return;
        }
        if (send_all(c->sock, io_buf, n) != 0) {
            break;
        }
        left -= n;
    }
    sceIoClose(fd);
    ctl_ok(c);
}

static void cmd_put(ctl_conn *c, const char *path, const char *size_str) {
    char *end = NULL;
    unsigned long size = strtoul(size_str, &end, 10);
    if (end == size_str || *end != '\0') {
        ctl_err(c, CTL_ERR_PROTO, "bad size");
        return;
    }
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        ctl_err(c, fd, "cannot create file");
        return;
    }
    ctl_printf(c, "READY\n");

    int write_err = 0;
    unsigned long left = size;
    while (left > 0) {
        int want = left < CTL_IO_SIZE ? (int) left : CTL_IO_SIZE;
        if (read_raw(c, io_buf, want) != want) {
            sceIoClose(fd);
            return; // connection gone
        }
        if (!write_err) {
            int n = sceIoWrite(fd, io_buf, want);
            if (n != want) {
                write_err = n < 0 ? n : (int) CTL_ERR_PROTO;
            }
        }
        left -= want;
    }
    sceIoClose(fd);
    if (write_err) {
        ctl_err(c, write_err, "write failed");
    } else {
        ctl_ok(c);
    }
}

static void cmd_install(ctl_conn *c, const char *dir) {
    char path[MAX_PATH_LENGTH];
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", dir);
    if (!s_exist(path)) {
        ctl_err(c, CTL_ERR_PROTO, "no sce_sys/param.sfo");
        return;
    }
    snprintf(path, sizeof(path), "%s/sce_sys/package/head.bin", dir);
    if (!s_exist(path)) {
        ctl_err(c, CTL_ERR_PROTO, "no sce_sys/package/head.bin");
        return;
    }

    // ScePaf is already loaded in SceShell, only the promoter needs loading
    int loaded = sceSysmoduleIsLoadedInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL) != SCE_SYSMODULE_LOADED;
    int res = 0;
    if (loaded) {
        res = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
        if (res < 0) {
            ctl_err(c, res, "loading ScePromoterUtil failed");
            return;
        }
    }
    res = scePromoterUtilityInit();
    if (res >= 0) {
        int promote = scePromoterUtilityPromotePkgWithRif(dir, 1);
        scePromoterUtilityExit();
        res = promote;
    }
    if (loaded) {
        sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    }
    if (res < 0) {
        ctl_err(c, res, "promote failed");
    } else {
        ctl_ok(c);
    }
}

// sceAppMgrGetStatusByName is looked up at runtime instead of imported: a psp2shell build that
// imported it (and sceAppMgrDestroyAppByName) hung SceShell at boot on fw 3.65 (2026-10-22).
// With a runtime lookup the module's imports stay unchanged; if the lookup fails, appstate reports
// an error and nothing else is affected.
#define SCE_APPMGR_USER_LIBNID 0xA6605D6F
#define SCE_APPMGR_GET_STATUS_BY_NAME_NID 0x656E0ABD

typedef int (*GetStatusByNameFn)(const char *name, SceAppMgrAppState *state);
static GetStatusByNameFn get_status_by_name = NULL;
static const char *get_status_by_name_module = NULL;
static int get_status_by_name_res = 0;

static GetStatusByNameFn find_get_status_by_name() {
    static const char *modules[] = {"SceDriverUser", "SceAppMgrUser", "SceAppMgr"};
    if (get_status_by_name == NULL) {
        for (unsigned int i = 0; i < sizeof(modules) / sizeof(modules[0]); i++) {
            uintptr_t func = 0;
            get_status_by_name_res = taiGetModuleExportFunc(modules[i], SCE_APPMGR_USER_LIBNID,
                                                            SCE_APPMGR_GET_STATUS_BY_NAME_NID, &func);
            if (get_status_by_name_res >= 0 && func != 0) {
                get_status_by_name = (GetStatusByNameFn) func;
                get_status_by_name_module = modules[i];
                break;
            }
        }
    }
    return get_status_by_name;
}

static void cmd_appstate(ctl_conn *c, const char *titleid) {
    GetStatusByNameFn fn = find_get_status_by_name();
    if (fn == NULL) {
        ctl_err(c, get_status_by_name_res, "appstate not supported (sceAppMgrGetStatusByName not found)");
        return;
    }
    SceAppMgrAppState state;
    memset(&state, 0, sizeof(state));
    int res = fn(titleid, &state);
    // 0: the app is running; anything else (e.g. 0x80800002) means it isn't
    ctl_data(c, "%s %s", res == 0 ? "running" : "stopped", get_status_by_name_module);
    ctl_ok(c);
}

typedef struct {
    const char *name;
    int type;
    int nargs;
} legacy_cmd;

static const legacy_cmd legacy_cmds[] = {
        {"launch",    CMD_LAUNCH,           1},
        {"title",     CMD_TITLE,            0},
        {"reset",     CMD_RESET,            0},
        {"reboot",    CMD_REBOOT,           0},
        {"modlist",   CMD_MODLS,            0},
        {"modlistp",  CMD_MODLS_PID,        1},
        {"modinfo",   CMD_MODINFO,          1},
        {"modinfop",  CMD_MODINFO_PID,      2},
        {"modstart",  CMD_MODLOADSTART,     1},
        {"modstartp", CMD_MODLOADSTART_PID, 2},
        {"modstop",   CMD_MODSTOPUNLOAD,    1},
        {"modstopp",  CMD_MODSTOPUNLOAD_PID, 2},
        {"kmodstart", CMD_KMODLOADSTART,    1},
        {"kmodstop",  CMD_KMODSTOPUNLOAD,   1},
        {"moddump",   CMD_MODDUMP,          3},
        {"thlist",    CMD_THLS,             0},
        {"memr",      CMD_MEMR,             2},
        {"memw",      CMD_MEMW,             2},
};

static void cmd_legacy(ctl_conn *c, const legacy_cmd *lc, int argc, char **argv) {
    P2S_CMD cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = lc->type;
    for (int i = 0; i < lc->nargs; i++) {
        strncpy(cmd.args[i], argv[i + 1], P2S_SIZE_STRING - 1);
    }

    if (lc->type == CMD_REBOOT) {
        // no answer possible afterwards
        ctl_ok(c);
        p2s_cmd_parse(NULL, &cmd);
        return;
    }

    capture_error = 0;
    capture_conn = c;
    capture_thid = sceKernelGetThreadId();
    p2s_cmd_parse(NULL, &cmd);
    capture_conn = NULL;
    capture_thid = -1;

    if (capture_error) {
        ctl_err(c, CTL_ERR_PROTO, "%s failed", lc->name);
    } else {
        ctl_ok(c);
    }
}

#define NEED(n) if (argc != (n)) { ctl_err(c, CTL_ERR_PROTO, "usage: %s", usage); return; }

static void execute(ctl_conn *c, int argc, char **argv) {
    const char *name = argv[0];
    const char *usage = name;

    if (strcmp(name, "hello") == 0) {
        ctl_data(c, "psp2shell-ctl %d", CTL_VERSION);
        ctl_ok(c);
    } else if (strcmp(name, "ls") == 0) {
        usage = "ls <dir>";
        NEED(2);
        cmd_ls(c, argv[1]);
    } else if (strcmp(name, "stat") == 0) {
        usage = "stat <path>";
        NEED(2);
        cmd_stat(c, argv[1]);
    } else if (strcmp(name, "md5") == 0) {
        usage = "md5 <file>";
        NEED(2);
        cmd_md5(c, argv[1]);
    } else if (strcmp(name, "mkdir") == 0) {
        usage = "mkdir [-p] <dir>";
        int parents = argc == 3 && strcmp(argv[1], "-p") == 0;
        NEED(2 + parents);
        cmd_mkdir(c, parents, argv[argc - 1]);
    } else if (strcmp(name, "rm") == 0) {
        usage = "rm <file>";
        NEED(2);
        cmd_rm(c, argv[1]);
    } else if (strcmp(name, "rmdir") == 0) {
        usage = "rmdir [-r] <dir>";
        int recursive = argc == 3 && strcmp(argv[1], "-r") == 0;
        NEED(2 + recursive);
        cmd_rmdir(c, recursive, argv[argc - 1]);
    } else if (strcmp(name, "mv") == 0) {
        usage = "mv <src> <dst>";
        NEED(3);
        cmd_mv(c, argv[1], argv[2]);
    } else if (strcmp(name, "get") == 0) {
        usage = "get <file>";
        NEED(2);
        cmd_get(c, argv[1]);
    } else if (strcmp(name, "put") == 0) {
        usage = "put <file> <size>";
        NEED(3);
        cmd_put(c, argv[1], argv[2]);
    } else if (strcmp(name, "install") == 0) {
        usage = "install <dir>";
        NEED(2);
        cmd_install(c, argv[1]);
    } else if (strcmp(name, "appstate") == 0) {
        usage = "appstate <titleid>";
        NEED(2);
        cmd_appstate(c, argv[1]);
    } else if (strcmp(name, "destroy") == 0) {
        sceAppMgrDestroyOtherApp();
        ctl_ok(c);
    } else {
        for (unsigned int i = 0; i < sizeof(legacy_cmds) / sizeof(legacy_cmds[0]); i++) {
            if (strcmp(name, legacy_cmds[i].name) == 0) {
                if (argc != legacy_cmds[i].nargs + 1) {
                    ctl_err(c, CTL_ERR_PROTO, "%s takes %d argument(s)", name, legacy_cmds[i].nargs);
                } else {
                    cmd_legacy(c, &legacy_cmds[i], argc, argv);
                }
                return;
            }
        }
        ctl_err(c, CTL_ERR_PROTO, "unknown command: %s", name);
    }
}

// ------------------------------------------------------------------------------------ threads

static int client_thread(SceSize args, void *argp) {
    ctl_conn *c = *(ctl_conn **) argp;
    char *argv[CTL_ARGS_MAX];
    char *line;

    while (!quit && (line = read_line(c)) != NULL) {
        TRACE("line: %s", line);
        int argc = parse_args(line, argv, CTL_ARGS_MAX);
        TRACE("argc %d, locking", argc);
        sceKernelLockMutex(cmd_mutex, 1, NULL);
        TRACE("locked");
        if (argc < 0) {
            ctl_err(c, CTL_ERR_PROTO, "bad quoting or too many arguments");
        } else if (argc > 0) {
            execute(c, argc, argv);
        }
        sceKernelUnlockMutex(cmd_mutex, 1);
        TRACE("unlocked");
    }

    TRACE("client gone");
    sceNetSocketClose(c->sock);
    c->sock = -1;
    return sceKernelExitDeleteThread(0);
}

static int accept_thread(SceSize args, void *argp) {
    while (!quit) {
        if (server_sock < 0) {
            sceKernelDelayThread(1000 * 1000);
            continue;
        }
        int sock = p2s_get_sock(server_sock);
        if (sock < 0) {
            // server socket closed (network reset or exit), main reopens it
            sceKernelDelayThread(1000 * 1000);
            continue;
        }
        ctl_conn *c = NULL;
        for (int i = 0; i < CTL_CLIENTS_MAX; i++) {
            if (conns[i].sock < 0) {
                c = &conns[i];
                break;
            }
        }
        if (c == NULL || quit) {
            const char *busy = "ERR 0x80000000 too many clients\n";
            send_all(sock, busy, (int) strlen(busy));
            sceNetSocketClose(sock);
            continue;
        }
        TRACE("accept sock %d slot %d", sock, (int) (c - conns));
        c->sock = sock;
        c->len = 0;
        c->line_end = 0;
        c->thid = sceKernelCreateThread("psp2shell_ctl", client_thread, 64, 0x4000, 0, 0x10000, 0);
        TRACE("thread 0x%08X", c->thid);
        if (c->thid < 0 || sceKernelStartThread(c->thid, sizeof(c), &c) < 0) {
            sceNetSocketClose(sock);
            c->sock = -1;
        }
    }
    return sceKernelExitDeleteThread(0);
}

void ctl_init() {
    for (int i = 0; i < CTL_CLIENTS_MAX; i++) {
        conns[i].sock = -1;
    }
    cmd_mutex = sceKernelCreateMutex("psp2shell_ctl", 0, 0, NULL);
    accept_thid = sceKernelCreateThread("psp2shell_ctla", accept_thread, 64, 0x2000, 0, 0x10000, 0);
}

int ctl_open(int port) {
    ctl_close();
    server_sock = p2s_bind_port(server_sock, port);
    return server_sock;
}

void ctl_close() {
    if (server_sock >= 0) {
        sceNetSocketClose(server_sock);
        server_sock = -1;
    }
}

void ctl_start() {
    if (!accept_started && accept_thid >= 0 && sceKernelStartThread(accept_thid, 0, NULL) >= 0) {
        accept_started = 1;
    }
}

void ctl_exit() {
    quit = 1;
    ctl_close();
    for (int i = 0; i < CTL_CLIENTS_MAX; i++) {
        if (conns[i].sock >= 0) {
            sceNetSocketClose(conns[i].sock);
        }
    }
    if (accept_started) {
        sceKernelWaitThreadEnd(accept_thid, NULL, NULL);
    } else if (accept_thid >= 0) {
        sceKernelDeleteThread(accept_thid);
    }
}
