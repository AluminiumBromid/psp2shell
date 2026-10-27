#ifndef _PSP2SHELL_K_H_
#define _PSP2SHELL_K_H_

#define P2S_KMSG_SIZE    0x1000

#ifdef __PSP2__

#include <stdbool.h>
#ifdef __VITA_KERNEL__
#include <psp2kern/kernel/modulemgr.h>
extern int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid, uint32_t funcnid, uintptr_t *func);
#else
#include <psp2/kernel/modulemgr.h>
#endif

// longest single printf captured by the kernel hooks (kept small, they run on arbitrary kernel stacks)
#define P2S_MSG_LEN 256
// kernel log ring buffer, must be a power of two
#define P2S_LOG_RING_SIZE (64 * 1024)

// Copy up to size bytes of buffered log output to buf (user memory) and consume them.
// If dropped is not NULL, it receives the number of bytes lost to ring overflow since the last read.
// Single reader only. Returns the number of bytes copied or a negative error.
int kpsp2shell_log_read(char *buf, SceSize size, SceUInt32 *dropped);

int kpsp2shell_get_module_info(SceUID pid, SceUID uid, SceKernelModuleInfo *info);

int kpsp2shell_get_module_list(SceUID pid, int flags1, int flags2, SceUID *modids, size_t *num);

int kpsp2shell_dump_module(SceUID pid, SceUID uid, const char *dst);

#endif // __PSP2__
#endif //_PSP2SHELL_K_H_