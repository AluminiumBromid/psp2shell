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

#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/cpu/spinlock.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/io/fcntl.h>
#include <taihen.h>

#include "psp2shell_k.h"

#define CHUNK_SIZE 2048
static uint8_t chunk[CHUNK_SIZE];

#define MAX_HOOKS 32
static SceUID g_hooks[MAX_HOOKS];
static tai_hook_ref_t ref_hooks[MAX_HOOKS];
static int __stdout_fd = 1073807367;

int (* _ksceKernelGetModuleList)(SceUID pid, int flags1, int flags2, SceUID *modids, size_t *num);
int (* _ksceKernelGetModuleInfo)(SceUID pid, SceUID modid, SceKernelModuleInfo *info);

// Log ring buffer. head/tail are free-running byte counters; the ring always keeps the newest
// output, so logs from boot are still there when the first client connects.
static char log_ring[P2S_LOG_RING_SIZE];
static SceUInt32 log_head;
static SceUInt32 log_tail;
static SceUInt32 log_dropped;
static int log_line_start = 1;
static SceKernelSpinlock log_lock;
static char log_read_buf[CHUNK_SIZE];

static void log_putc(char c) {
    if (log_head - log_tail == P2S_LOG_RING_SIZE) {
        log_tail++;
        log_dropped++;
    }
    log_ring[log_head & (P2S_LOG_RING_SIZE - 1)] = c;
    log_head++;
}

static void log_write(const char *data, SceSize len) {
    // "[seconds.micros pid] " in front of every line
    char prefix[32];
    SceUInt64 now = (SceUInt64) ksceKernelGetSystemTimeWide();
    int prefix_len = snprintf(prefix, sizeof(prefix), "[%5u.%06u %08X] ",
                              (unsigned int) (now / 1000000), (unsigned int) (now % 1000000),
                              (unsigned int) ksceKernelGetProcessId());

    SceKernelIntrStatus intr = ksceKernelSpinlockLowLockCpuSuspendIntr(&log_lock);
    for (SceSize i = 0; i < len; i++) {
        if (log_line_start) {
            for (int j = 0; j < prefix_len; j++) {
                log_putc(prefix[j]);
            }
        }
        log_putc(data[i]);
        log_line_start = data[i] == '\n';
    }
    ksceKernelSpinlockLowUnlockCpuResumeIntr(&log_lock, intr);
}

static int ksceDebugPrintf_hook(const char *fmt, ...) {
    char temp_buf[P2S_MSG_LEN];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(temp_buf, P2S_MSG_LEN, fmt, args);
    va_end(args);

    if (len > 0) {
        log_write(temp_buf, len < P2S_MSG_LEN ? (SceSize) len : P2S_MSG_LEN - 1);
    }

    // the va_list can't be forwarded to a variadic function, pass the formatted text instead
    return TAI_CONTINUE(int, ref_hooks[2], "%s", temp_buf);
}

static int ksceDebugPrintf2_hook(int num0, int num1, const char *fmt, ...) {
    char temp_buf[P2S_MSG_LEN];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(temp_buf, P2S_MSG_LEN, fmt, args);
    va_end(args);

    if (len > 0) {
        log_write(temp_buf, len < P2S_MSG_LEN ? (SceSize) len : P2S_MSG_LEN - 1);
    }

    return TAI_CONTINUE(int, ref_hooks[3], num0, num1, "%s", temp_buf);
}

static int sceIoWrite_hook(SceUID fd, const void *data, SceSize size) {
    if (ref_hooks[0] <= 0) {
        return 0;
    }

    if (fd == __stdout_fd) {
        char temp_buf[P2S_MSG_LEN];
        for (SceSize off = 0; off < size; off += P2S_MSG_LEN) {
            SceSize len = size - off < P2S_MSG_LEN ? size - off : P2S_MSG_LEN;
            if (ksceKernelMemcpyUserToKernel(temp_buf, (const char *) data + off, len) < 0) {
                break;
            }
            log_write(temp_buf, len);
        }
    }

    return TAI_CONTINUE(int, ref_hooks[0], fd, data, size);
}

static int sceKernelGetStdout_hook() {
    if (ref_hooks[1] <= 0) {
        return 0;
    }

    int fd = TAI_CONTINUE(int, ref_hooks[1]);
    __stdout_fd = fd;

    return fd;
}

int kpsp2shell_log_read(char *buf, SceSize size, SceUInt32 *dropped) {
    int copied = 0;
    SceUInt32 lost = 0;
    uint32_t state;
    ENTER_SYSCALL(state);

    while ((SceSize) copied < size) {
        SceSize want = size - copied < CHUNK_SIZE ? size - copied : CHUNK_SIZE;

        // copy out under the lock, touch user memory only after releasing it
        SceKernelIntrStatus intr = ksceKernelSpinlockLowLockCpuSuspendIntr(&log_lock);
        SceSize avail = log_head - log_tail;
        SceSize n = avail < want ? avail : want;
        for (SceSize i = 0; i < n; i++) {
            log_read_buf[i] = log_ring[(log_tail + i) & (P2S_LOG_RING_SIZE - 1)];
        }
        log_tail += n;
        lost += log_dropped;
        log_dropped = 0;
        ksceKernelSpinlockLowUnlockCpuResumeIntr(&log_lock, intr);

        if (n == 0) {
            break;
        }
        if (ksceKernelMemcpyKernelToUser(buf + copied, log_read_buf, n) < 0) {
            copied = -1;
            break;
        }
        copied += n;
    }

    if (dropped != NULL) {
        ksceKernelMemcpyKernelToUser(dropped, &lost, sizeof(lost));
    }

    EXIT_SYSCALL(state);
    return copied;
}

int kpsp2shell_dump(SceUID pid, const char *filename, void *addr, unsigned int size) {
    SceUID fd;
    fd = ksceIoOpen(filename, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 6);
    if (fd <= 0) {
        return fd;
    }

    size_t i = 0;
    size_t bufsize;

    while (i < size) {
        if (size - i > CHUNK_SIZE) {
            bufsize = CHUNK_SIZE;
        } else {
            bufsize = size - i;
        }
        ksceKernelMemcpyUserToKernelForPid(pid, chunk, addr + i, bufsize);
        i += bufsize;

        ksceIoWrite(fd, chunk, bufsize);
    }

    ksceIoClose(fd);

    return 0;
}

int kpsp2shell_dump_module(SceUID pid, SceUID uid, const char *dst) {
    int ret;
    SceKernelModuleInfo kinfo;
    memset(&kinfo, 0, sizeof(SceKernelModuleInfo));
    kinfo.size = sizeof(SceKernelModuleInfo);

    uint32_t state;
    ENTER_SYSCALL(state);

    SceUID kid = ksceKernelKernelUidForUserUid(pid, uid);
    if (kid < 0) {
        kid = uid;
    }

    ret = _ksceKernelGetModuleInfo(pid, kid, &kinfo);

    if (ret >= 0) {

        char path[128];

        for (int i = 0; i < 4; i++) {

            SceKernelSegmentInfo *seginfo = &kinfo.segments[i];

            if (seginfo->memsz <= 0 || seginfo->vaddr == NULL || seginfo->size != sizeof(*seginfo)) {
                continue;
            }

            memset(path, 0, 128);
            ksceKernelStrncpyUserToKernel(path, dst, 128);
            snprintf(path + strlen(path), 128, "/%s_0x%08X_seg%d.bin",
                     kinfo.module_name, (uintptr_t) seginfo->vaddr, i);

            ksceDebugPrintf_hook("dumping: %s\n", path);

            ret = kpsp2shell_dump(pid, path, seginfo->vaddr, seginfo->memsz);
        }
    }

    EXIT_SYSCALL(state);

    return ret;
}

int kpsp2shell_get_module_info(SceUID pid, SceUID uid, SceKernelModuleInfo *info) {
    int ret;
    SceKernelModuleInfo kinfo;
    memset(&kinfo, 0, sizeof(SceKernelModuleInfo));
    kinfo.size = sizeof(SceKernelModuleInfo);

    uint32_t state;
    ENTER_SYSCALL(state);

    SceUID kid = ksceKernelKernelUidForUserUid(pid, uid);
    if (kid < 0) {
        kid = uid;
    }

    ret = _ksceKernelGetModuleInfo(pid, kid, &kinfo);
    if (ret >= 0) {
        ksceKernelMemcpyKernelToUser(info, &kinfo, sizeof(SceKernelModuleInfo));
    }

    EXIT_SYSCALL(state);

    return ret;
}

int kpsp2shell_get_module_list(SceUID pid, int flags1, int flags2, SceUID *modids, size_t *num) {
    size_t count = 256;
    SceUID kmodids[256];

    uint32_t state;
    ENTER_SYSCALL(state);

    memset(kmodids, 0, sizeof(SceUID) * 256);
    int res = _ksceKernelGetModuleList(pid, flags1, flags2, kmodids, &count);
    if (res >= 0) {
        ksceKernelMemcpyKernelToUser(modids, &kmodids, sizeof(SceUID) * 256);
        ksceKernelMemcpyKernelToUser(num, &count, sizeof(size_t));
    } else {
        ksceKernelMemcpyKernelToUser(num, &count, sizeof(size_t));
    }

    EXIT_SYSCALL(state);

    return res;
}

void set_hooks() {
    uint32_t state;
    ENTER_SYSCALL(state);

    g_hooks[0] = taiHookFunctionExportForKernel(
            KERNEL_PID,
            &ref_hooks[0],
            "SceIofilemgr",
            0xF2FF276E, // SceIofilemgr
            0x34EFD876, // sceIoWrite
            sceIoWrite_hook);
    //LOG("hook: sceIoWrite: 0x%08X\n", g_hooks[0]);

    g_hooks[1] = taiHookFunctionExportForKernel(
            KERNEL_PID,
            &ref_hooks[1],
            "SceProcessmgr",
            0x2DD91812, // SceProcessmgr
            0xE5AA625C, // sceKernelGetStdout
            sceKernelGetStdout_hook);
    //LOG("hook: sceKernelGetStdout: 0x%08X\n", g_hooks[1]);

    g_hooks[2] = taiHookFunctionExportForKernel(
            KERNEL_PID,
            &ref_hooks[2],
            "SceSysmem",
            0x88758561, // SceDebugForDriver
            0x391B74B7, // ksceDebugPrintf
            ksceDebugPrintf_hook);
    //LOG("hook: _printf: 0x%08X\n", g_hooks[2]);

    g_hooks[3] = taiHookFunctionExportForKernel(
            KERNEL_PID,
            &ref_hooks[3],
            "SceSysmem",
            0x88758561, // SceDebugForDriver
            0x02B04343, // ksceDebugPrintf2
            ksceDebugPrintf2_hook);
    //LOG("hook: _printf2: 0x%08X\n", g_hooks[3]);

    EXIT_SYSCALL(state);
}

void delete_hooks() {
    for (int i = 0; i < MAX_HOOKS; i++) {
        if (g_hooks[i] >= 0)
            taiHookReleaseForKernel(g_hooks[i], ref_hooks[i]);
    }
}

int set_modulemgr_funcs() {
     // 3.60
    int res = module_get_export_func(KERNEL_PID, "SceKernelModulemgr", TAI_ANY_LIBRARY,
                                     0x97CF7B4E, (uintptr_t *)&_ksceKernelGetModuleList);
    
    // 3.63 - 3.74
    if (res < 0)
        res = module_get_export_func(KERNEL_PID, "SceKernelModulemgr", TAI_ANY_LIBRARY,
                                     0xB72C75A4, (uintptr_t *)&_ksceKernelGetModuleList);

    if (res < 0)
        return res;

    // 3.60
    res = module_get_export_func(KERNEL_PID, "SceKernelModulemgr", TAI_ANY_LIBRARY,
                                 0xD269F915, (uintptr_t *)&_ksceKernelGetModuleInfo);

    // 3.63 - 3.74
    if (res < 0)
        res = module_get_export_func(KERNEL_PID, "SceKernelModulemgr", TAI_ANY_LIBRARY,
                                     0xDAA90093, (uintptr_t *)&_ksceKernelGetModuleInfo);

    return res;
}

void _start() __attribute__ ((weak, alias ("module_start")));

int module_start(SceSize argc, const void *args) {
    if (set_modulemgr_funcs() < 0)
        return SCE_KERNEL_START_FAILED; 
    
    set_hooks();
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
    delete_hooks();
    return SCE_KERNEL_STOP_SUCCESS;
}
