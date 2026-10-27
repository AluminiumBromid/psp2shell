// Host stand-ins for the Vita APIs ctl.c uses (POSIX underneath). See run.sh.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
typedef int SceUID; typedef unsigned int SceSize; typedef unsigned int SceUInt; typedef int64_t SceOff;
typedef struct { unsigned short year, month, day, hour, minute, second; unsigned int microsecond; } SceDateTime;
typedef struct { int st_mode; unsigned int st_attr; SceOff st_size; SceDateTime st_ctime, st_atime, st_mtime; } SceIoStat;
typedef struct { SceIoStat d_stat; char d_name[256]; void *d_private; int dummy; } SceIoDirent;
typedef struct { int dummy[32]; } SceAppMgrAppState;
#define SCE_S_IFDIR 0x1000
#define SCE_S_ISDIR(m) (((m) & SCE_S_IFDIR) != 0)
#define SCE_O_RDONLY 1
#define SCE_O_WRONLY 2
#define SCE_O_CREAT 0x200
#define SCE_O_TRUNC 0x400
#define SCE_SYSMODULE_LOADED 0
#define SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL 0x80000024
#define MAX_PATH_LENGTH 256
#define MOVE_INTEGRATE 1
#define MOVE_REPLACE 2
#define COL_RED 11
#define CMD_LAUNCH 19
#define CMD_TITLE 25
#define CMD_RESET 46
#define CMD_REBOOT 47
#define CMD_MODLS 30
#define CMD_MODLS_PID 31
#define CMD_MODINFO 32
#define CMD_MODINFO_PID 33
#define CMD_MODLOADSTART 34
#define CMD_MODLOADSTART_PID 35
#define CMD_MODSTOPUNLOAD 36
#define CMD_MODSTOPUNLOAD_PID 37
#define CMD_KMODLOADSTART 38
#define CMD_KMODSTOPUNLOAD 39
#define CMD_MODDUMP 40
#define CMD_THLS 41
#define CMD_MEMR 44
#define CMD_MEMW 45
#define P2S_SIZE_STRING 512
typedef struct { int type; char args[3][P2S_SIZE_STRING]; } P2S_CMD;
typedef struct s_client s_client;
SceUID sceIoOpen(const char *p, int flags, int mode); int sceIoRead(SceUID fd, void *b, SceSize n);
int sceIoWrite(SceUID fd, const void *b, SceSize n); int sceIoClose(SceUID fd);
SceUID sceIoDopen(const char *p); int sceIoDread(SceUID fd, SceIoDirent *d); int sceIoDclose(SceUID fd);
int sceIoGetstat(const char *p, SceIoStat *st); int sceIoMkdir(const char *p, int mode);
int sceIoRemove(const char *p); int sceIoRmdir(const char *p); int sceIoRename(const char *a, const char *b);
int sceNetSend(int s, const void *b, unsigned int n, int f); int sceNetRecv(int s, void *b, unsigned int n, int f);
int sceNetSocketClose(int s);
typedef int (*SceKernelThreadEntry)(SceSize, void *);
SceUID sceKernelCreateThread(const char *n, SceKernelThreadEntry e, int p, int s, int a, int c, void *o);
int sceKernelStartThread(SceUID t, SceSize len, void *argp); int sceKernelExitDeleteThread(int s);
int sceKernelDeleteThread(SceUID t); int sceKernelWaitThreadEnd(SceUID t, int *s, void *to);
int sceKernelDelayThread(unsigned int us); SceUID sceKernelGetThreadId(void);
SceUID sceKernelCreateMutex(const char *n, SceUInt a, int c, void *o);
int sceKernelLockMutex(SceUID m, int c, void *t); int sceKernelUnlockMutex(SceUID m, int c);
int sceAppMgrDestroyOtherApp(void); int sceAppMgrDestroyAppByName(const char *n);
int sceAppMgrLaunchAppByUri(int f, const char *uri);
int sceSysmoduleIsLoadedInternal(int id); int sceSysmoduleLoadModuleInternal(int id); int sceSysmoduleUnloadModuleInternal(int id);
int scePromoterUtilityInit(void); int scePromoterUtilityExit(void); int scePromoterUtilityPromotePkgWithRif(const char *p, int sync);
int s_isDir(char *p); int s_exist(char *p); int s_removePath(char *p, void *x); int s_movePath(char *a, char *b, int f, void *x);
int taiGetModuleExportFunc(const char *modname, uint32_t libnid, uint32_t funcnid, uintptr_t *func);
int p2s_removeEndSlash(char *p); int p2s_get_sock(int s); int p2s_bind_port(int s, int port);
