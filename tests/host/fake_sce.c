// Runs psp2shell_m's control server (source/ctl.c) on the PC: Vita file I/O maps to a local
// directory, sockets to BSD sockets, threads to pthreads. See run.sh.
//
// Note: this uses glibc, so libk quirks (va_start, missing "%.*s") are NOT covered here.

#define _GNU_SOURCE
#include "fake/fake_sce.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <time.h>
#include <unistd.h>
#include "ctl.h"
#undef st_mtime
#undef st_atime
#undef st_ctime

static const char *root;

static int sce_err(void) {
    switch (errno) {
        case ENOENT: return 0x80010002;
        case EEXIST: return 0x80010011;
        case ENOTEMPTY: return 0x8001005A;
        case ENOTDIR: return 0x80010014;
        case EISDIR: return 0x80010015;
        default: return 0x80010000 | errno;
    }
}

// "ux0:/a" or "ux0:a" -> <root>/ux0/a
static void host_path(const char *p, char *out) {
    const char *c = strchr(p, ':');
    snprintf(out, 1024, "%s/%.*s/%s", root, (int) (c - p), p, c[1] == '/' ? c + 2 : c + 1);
}

SceUID sceIoOpen(const char *p, int flags, int mode) {
    char h[1024];
    host_path(p, h);
    int f = (flags & SCE_O_WRONLY) ? O_WRONLY : O_RDONLY;
    if (flags & SCE_O_CREAT) f |= O_CREAT;
    if (flags & SCE_O_TRUNC) f |= O_TRUNC;
    int fd = open(h, f, 0644);
    return fd < 0 ? sce_err() : fd;
}
int sceIoRead(SceUID fd, void *b, SceSize n) { int r = read(fd, b, n); return r < 0 ? sce_err() : r; }
int sceIoWrite(SceUID fd, const void *b, SceSize n) { int r = write(fd, b, n); return r < 0 ? sce_err() : r; }
int sceIoClose(SceUID fd) { return close(fd); }

static void fill_stat(struct stat *s, SceIoStat *st) {
    memset(st, 0, sizeof *st);
    st->st_mode = S_ISDIR(s->st_mode) ? SCE_S_IFDIR : 0;
    st->st_size = s->st_size;
    struct tm tm;
    gmtime_r(&s->st_mtim.tv_sec, &tm);
    st->st_mtime = (SceDateTime) {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, 0};
}

typedef struct { DIR *d; char path[1024]; } fdir;
static fdir dirs[64];
SceUID sceIoDopen(const char *p) {
    char h[1024];
    host_path(p, h);
    DIR *d = opendir(h);
    if (!d) return sce_err();
    for (int i = 0; i < 64; i++) {
        if (!dirs[i].d) { dirs[i].d = d; strcpy(dirs[i].path, h); return 1000 + i; }
    }
    closedir(d);
    return -1;
}
int sceIoDread(SceUID fd, SceIoDirent *e) {
    fdir *f = &dirs[fd - 1000];
    struct dirent *de;
    while ((de = readdir(f->d)) && (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))) {}
    if (!de) return 0;
    char h[1300];
    snprintf(h, sizeof h, "%s/%s", f->path, de->d_name);
    struct stat s;
    stat(h, &s);
    fill_stat(&s, &e->d_stat);
    strncpy(e->d_name, de->d_name, 255);
    return 1;
}
int sceIoDclose(SceUID fd) { closedir(dirs[fd - 1000].d); dirs[fd - 1000].d = NULL; return 0; }
int sceIoGetstat(const char *p, SceIoStat *st) {
    char h[1024];
    host_path(p, h);
    struct stat s;
    if (stat(h, &s) < 0) return sce_err();
    fill_stat(&s, st);
    return 0;
}
int sceIoMkdir(const char *p, int m) { char h[1024]; host_path(p, h); return mkdir(h, 0755) < 0 ? sce_err() : 0; }
int sceIoRemove(const char *p) { char h[1024]; host_path(p, h); return unlink(h) < 0 ? sce_err() : 0; }
int sceIoRmdir(const char *p) { char h[1024]; host_path(p, h); return rmdir(h) < 0 ? sce_err() : 0; }
int sceIoRename(const char *a, const char *b) {
    char x[1024], y[1024];
    host_path(a, x);
    host_path(b, y);
    return rename(x, y) < 0 ? sce_err() : 0;
}

int s_isDir(char *p) { SceIoStat st; return sceIoGetstat(p, &st) == 0 && SCE_S_ISDIR(st.st_mode); }
int s_exist(char *p) { SceIoStat st; return sceIoGetstat(p, &st) == 0; }
int s_removePath(char *p, void *x) {
    char h[1024], cmd[1100];
    host_path(p, h);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", h);
    return system(cmd) == 0 ? 1 : -1;
}
int s_movePath(char *a, char *b, int f, void *x) { return sceIoRename(a, b) == 0 ? 1 : -1; }
int p2s_removeEndSlash(char *p) { size_t n = strlen(p); if (n && p[n - 1] == '/') p[n - 1] = 0; return 0; }

int sceNetSend(int s, const void *b, unsigned int n, int f) { return send(s, b, n, MSG_NOSIGNAL); }
int sceNetRecv(int s, void *b, unsigned int n, int f) { return recv(s, b, n, 0); }
int sceNetSocketClose(int s) { shutdown(s, SHUT_RDWR); return close(s); }
int p2s_bind_port(int s, int port) {
    s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(s, (void *) &a, sizeof a) < 0 || listen(s, 8) < 0) return -1;
    return s;
}
int p2s_get_sock(int s) { return accept(s, NULL, NULL); }

typedef struct { SceKernelThreadEntry e; char arg[64]; SceSize len; pthread_t t; } fthread;
static fthread threads[4096];
static int nthreads;
SceUID sceKernelCreateThread(const char *n, SceKernelThreadEntry e, int p, int s, int a, int c, void *o) {
    threads[nthreads].e = e;
    return nthreads++;
}
static void *trampoline(void *x) { fthread *t = x; t->e(t->len, t->arg); return NULL; }
int sceKernelStartThread(SceUID id, SceSize len, void *argp) {
    fthread *t = &threads[id];
    if (len) memcpy(t->arg, argp, len);
    t->len = len;
    return pthread_create(&t->t, NULL, trampoline, t);
}
int sceKernelExitDeleteThread(int s) { pthread_exit(NULL); }
int sceKernelDeleteThread(SceUID t) { return 0; }
int sceKernelWaitThreadEnd(SceUID t, int *s, void *to) { return 0; }
int sceKernelDelayThread(unsigned int us) { return usleep(us); }
SceUID sceKernelGetThreadId(void) { return (SceUID) (uintptr_t) pthread_self(); }

static pthread_mutex_t mutexes[8];
static int nmutexes;
SceUID sceKernelCreateMutex(const char *n, SceUInt a, int c, void *o) { pthread_mutex_init(&mutexes[nmutexes], NULL); return nmutexes++; }
int sceKernelLockMutex(SceUID m, int c, void *t) { return pthread_mutex_lock(&mutexes[m]); }
int sceKernelUnlockMutex(SceUID m, int c) { return pthread_mutex_unlock(&mutexes[m]); }

// App manager: "HANG00001" blocks for 20 s, PSPEMUCFW can be started/stopped.
static volatile int app_running = 1;
int sceAppMgrDestroyOtherApp(void) { app_running = 0; return 0; }
int sceAppMgrDestroyAppByName(const char *n) {
    if (!strcmp(n, "HANG00001")) { sleep(20); return 0; }
    app_running = 0;
    return 0;
}
int sceAppMgrLaunchAppByUri(int f, const char *uri) {
    if (!strstr(uri, "titleid=PSPEMUCFW")) return 0x80802003;
    app_running = 1;
    return 0;
}
int sceAppMgrGetStatusByName(const char *n, SceAppMgrAppState *st) { return app_running ? 0 : 0x80800002; }

// taiHEN NID lookup: SceAppMgrUser's sceAppMgrGetStatusByName lives in "SceDriverUser" here;
// P2S_FAKE_NO_APPSTATE=1 simulates a firmware where it can't be found.
int taiGetModuleExportFunc(const char *modname, uint32_t libnid, uint32_t funcnid, uintptr_t *func) {
    if (getenv("P2S_FAKE_NO_APPSTATE") == NULL && !strcmp(modname, "SceDriverUser") &&
        libnid == 0xA6605D6F && funcnid == 0x656E0ABD) {
        *func = (uintptr_t) sceAppMgrGetStatusByName;
        return 0;
    }
    return 0x90010002; // TAI_ERROR_NOT_FOUND-style
}

int sceSysmoduleIsLoadedInternal(int id) { return 0x805A1001; }
int sceSysmoduleLoadModuleInternal(int id) { return 0; }
int sceSysmoduleUnloadModuleInternal(int id) { return 0; }
int scePromoterUtilityInit(void) { return 0; }
int scePromoterUtilityExit(void) { return 0; }
// fake install: move the package to ux0:app/<TITLE_ID found in its param.sfo>
int scePromoterUtilityPromotePkgWithRif(const char *p, int sync) {
    char path[600], h[1024], tid[16] = "UNKNOWN00";
    snprintf(path, sizeof path, "%s/sce_sys/param.sfo", p);
    host_path(path, h);
    FILE *f = fopen(h, "rb");
    if (!f) return 0x80870001;
    unsigned char sfo[8192];
    size_t n = fread(sfo, 1, sizeof sfo, f);
    fclose(f);
    // TITLE_ID is the only 9-char upper-case/digit string value in our test SFOs
    for (size_t i = 0; i + 10 <= n; i++) {
        int ok = sfo[i + 9] == 0 && (i == 0 || sfo[i - 1] == 0);
        for (int j = 0; ok && j < 9; j++) ok = (sfo[i + j] >= 'A' && sfo[i + j] <= 'Z') || (sfo[i + j] >= '0' && sfo[i + j] <= '9');
        if (ok && strncmp((char *) sfo + i, "TITLE_ID", 8)) { memcpy(tid, sfo + i, 9); tid[9] = 0; break; }
    }
    char dst[64];
    snprintf(dst, sizeof dst, "ux0:app/%s", tid);
    sceIoMkdir("ux0:app", 0777);
    s_removePath(dst, NULL);
    return sceIoRename(p, dst);
}

// legacy command stand-ins: title prints text, memr prints an error
void psp2shell_print_color(int color, const char *fmt, ...) {
    char text[4096];
    va_list a;
    va_start(a, fmt);
    vsnprintf(text, sizeof text, fmt, a);
    va_end(a);
    if (ctl_capturing()) ctl_capture(color, text);
}
void p2s_cmd_parse(s_client *c, P2S_CMD *cmd) {
    if (cmd->type == CMD_TITLE) psp2shell_print_color(13, "\n\n\tname: SceShell\n\tpid: 0x000118C7\n\n");
    else if (cmd->type == CMD_LAUNCH) sceAppMgrLaunchAppByUri(0xFFFFF, "psgm:play?titleid=PSPEMUCFW"); // legacy launch
    else if (cmd->type == CMD_MEMR) psp2shell_print_color(COL_RED, "bad address %s\n", cmd->args[0]);
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <root dir> <port>\n", argv[0]); return 1; }
    root = argv[1];
    signal(SIGPIPE, SIG_IGN);
    ctl_init();
    if (ctl_open(atoi(argv[2])) < 0) { perror("bind"); return 1; }
    ctl_start();
    for (;;) pause();
}
