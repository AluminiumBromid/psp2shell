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

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/net/net.h>
#include <main.h>

#include "main.h"
#include "utility.h"
#include "psp2shell.h"
#include "libmodule.h"

static SceUID thid_wait = -1;
static SceUID thid_kbuf = -1;
static SceUID thid_log = -1;
static SceUID thid_client = -1;
#ifdef DEBUG
static int listen_port = 5555;
#else
static int listen_port = 3333;
#endif
static s_client *client;
static int server_sock_msg;
static int server_sock_cmd;
static int quit = 0;

#ifndef DEBUG
// Log stream: raw text on listen_port + 2, several readers at once, independent of the shell session.
#define LOG_CLIENTS_MAX 4
#define LOG_CHUNK 4096
static int server_sock_log = -1;
static int log_thread_started = 0;
static int log_socks[LOG_CLIENTS_MAX];
#endif

void p2s_cmd_parse(s_client *client, P2S_CMD *cmd);

static void close_server() {

    if (server_sock_msg >= 0) {
        sceNetSocketClose(server_sock_msg);
        server_sock_msg = -1;
    }
    if (server_sock_cmd >= 0) {
        sceNetSocketClose(server_sock_cmd);
        server_sock_cmd = -1;
    }
#ifndef DEBUG
    if (server_sock_log >= 0) {
        sceNetSocketClose(server_sock_log);
        server_sock_log = -1;
    }
#endif
}

static int open_server() {

    close_server();

    server_sock_msg = p2s_bind_port(server_sock_msg, listen_port);
    if (server_sock_msg <= 0) {
        return -1;
    }
    server_sock_cmd = p2s_bind_port(server_sock_cmd, listen_port + 1);
    if (server_sock_cmd <= 0) {
        return -1;
    }
#ifndef DEBUG
    // the shell still works without the log port
    server_sock_log = p2s_bind_port(server_sock_log, listen_port + 2);
#endif

    return 0;
}

static void p2s_msg_wait_reply() {

    if (client->msg_sock < 0) {
        return;
    }

    char buf[1];

    // wait for client to receive message
    long timeout = 1000000000;
    sceNetSetsockopt(client->msg_sock,
                     SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeout, 4);
    sceNetRecv(client->msg_sock, buf, 1, 0);
    timeout = 0;
    sceNetSetsockopt(client->msg_sock,
                     SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeout, 4);
}

void psp2shell_print_color(int color, const char *fmt, ...) {

    if (client->msg_sock < 0) {
        return;
    }

    client->msg.color = color;
    memset(client->msg.buffer, 0, P2S_KMSG_SIZE);
    va_list args;
    va_start(args, fmt);
    vsnprintf(client->msg.buffer, P2S_KMSG_SIZE, fmt, args);
    va_end(args);

    if (client->msg_sock >= 0) {

        if (p2s_msg_send_msg(client->msg_sock, &client->msg) == 0) {
            // wait for client to receive message
            p2s_msg_wait_reply();
        }
    }
}

static void welcome() {

    char msg[1024];
    memset(msg, 0, 1024);
    strcpy(msg, "\n");
    strcat(msg, "                     ________         .__           .__  .__   \n");
    strcat(msg, "______  ____________ \\_____  \\   _____|  |__   ____ |  | |  |  \n");
    strcat(msg, "\\____ \\/  ___/\\____ \\ /  ____/  /  ___/  |  \\_/ __ \\|  | |  |  \n");
    strcat(msg, "|  |_> >___ \\ |  |_> >       \\  \\___ \\|   Y  \\  ___/|  |_|  |__\n");
    strcat(msg, "|   __/____  >|   __/\\_______ \\/____  >___|  /\\___  >____/____/\n");
    sprintf(msg + strlen(msg), "|__|       \\/ |__|           \\/     \\/     \\/     \\/ %s\n\n", VERSION);
    PRINT_OK(msg);
}

int cmd_thread(SceSize args, void *argp) {

    printf("cmd_thread\n");

    int sock = *((int *) argp);

    memset(client, 0, sizeof(s_client));
    client->msg_sock = sock;

    // init client file listing memory/**/
    memset(&client->fileList, 0, sizeof(s_FileList));
    strcpy(client->fileList.path, HOME_PATH);
    s_fileListGetEntries(&client->fileList, HOME_PATH);

    // Welcome!
    printf("welcome client\n");
    welcome();

    // get data sock
    client->cmd_sock = p2s_get_sock(server_sock_cmd);
    printf("got data sock: %i\n", client->cmd_sock);

    while (!quit) {

        int res = p2s_cmd_receive(client->cmd_sock, &client->cmd);
        if (res != 0) {
            if (res == P2S_ERR_SOCKET) {
                PRINT_ERR("p2s_cmd_receive sock failed: 0x%08X\n", res);
                break;
            } else {
                PRINT_ERR("p2s_cmd_receive failed: 0x%08X\n", res);
            }
        } else {
            p2s_cmd_parse(client, &client->cmd);
        }
    }

    printf("closing connection\n");
    if (client->msg_sock >= 0) {
        sceNetSocketClose(client->msg_sock);
        client->msg_sock = -1;
    }
    if (client->cmd_sock >= 0) {
        sceNetSocketClose(client->cmd_sock);
        client->cmd_sock = -1;
    }
    s_fileListEmpty(&client->fileList);

    sceKernelExitDeleteThread(0);
    return 0;
}

static int thread_wait(SceSize args, void *argp) {

    // Enso fix ?!
    sceKernelDelayThread(1000 * 1000 * 5);

    // load/wait network modules
    while (p2s_netInit() != 0) {
        if (quit) {
            psp2shell_exit();
            sceKernelExitDeleteThread(0);
            return -1;
        }
        sceKernelDelayThread(1000 * 1000);
    }

    // setup sockets
    if (open_server() != 0) {
        printf("open_server failed\n");
        psp2shell_exit();
        sceKernelExitDeleteThread(0);
        return -1;
    }

#ifndef DEBUG
    if (thid_log >= 0 && sceKernelStartThread(thid_log, 0, NULL) >= 0) {
        log_thread_started = 1;
    }
#endif

    // accept incoming connections
    printf("Waiting for connections...\n");
    while (!quit) {

        int client_sock = p2s_get_sock(server_sock_msg);
        if (client_sock < 0) {
            if (quit) {
                break;
            }
            printf("network disconnected: reset (%i)\n", client_sock);
            sceKernelDelayThread(1000 * 1000);
            open_server();
            continue;
        } else if (client_sock == 0) {
            printf("client_sock == 0\n");
        }

        if (quit) {
            break;
        }

        printf("new connexion on port %i (sock=%i)\n", listen_port, client_sock);
        // max client/socket count reached
        if (client->cmd_sock > 0) {
            printf("Connection refused, max client reached (1)\n");
            sceNetSocketClose(client_sock);
            continue;
        }

        printf("Connection accepted\n");
        thid_client = sceKernelCreateThread("psp2shell_cmd", cmd_thread, 64, 0x5000, 0, 0x10000, 0);
        if (thid_client >= 0)
            sceKernelStartThread(thid_client, sizeof(int), (void *) &client_sock);
    }

    psp2shell_exit();

    sceKernelExitDeleteThread(0);
    return 0;
}

#ifndef DEBUG

static int log_client_count() {
    int count = 0;
    for (int i = 0; i < LOG_CLIENTS_MAX; i++) {
        if (log_socks[i] >= 0) {
            count++;
        }
    }
    return count;
}

static int send_all(int sock, const char *data, int len) {
    while (len > 0) {
        int sent = sceNetSend(sock, data, (unsigned int) len, 0);
        if (sent <= 0) {
            return -1;
        }
        data += sent;
        len -= sent;
    }
    return 0;
}

static void log_send(const char *data, int len) {
    int any = 0;
    for (int i = 0; i < LOG_CLIENTS_MAX; i++) {
        int sock = log_socks[i];
        if (sock < 0) {
            continue;
        }
        any = 1;
        if (send_all(sock, data, len) != 0) {
            // gone or stalled past SO_SNDTIMEO: drop the reader, never block the log
            log_socks[i] = -1;
            sceNetSocketClose(sock);
        }
    }
    // old clients without a log connection still get logs mixed into the shell
    if (!any && client != NULL && client->msg_sock >= 0) {
        sceNetSend(client->msg_sock, data, (unsigned int) len, 0);
    }
}

// Drains the kernel log ring. Only reads while somebody listens, so the ring keeps the
// newest output (including boot) for the first reader.
static int thread_kbuf(SceSize args, void *argp) {

    static char buf[LOG_CHUNK];

    while (!quit) {

        if (log_client_count() == 0 && (client == NULL || client->msg_sock < 0)) {
            sceKernelDelayThread(10 * 1000);
            continue;
        }

        SceUInt32 dropped = 0;
        int len = kpsp2shell_log_read(buf, LOG_CHUNK, &dropped);
        if (dropped > 0) {
            char note[64];
            int note_len = snprintf(note, sizeof(note), "\n[psp2shell: %u bytes of log dropped]\n",
                                    (unsigned int) dropped);
            log_send(note, note_len);
        }
        if (len > 0) {
            log_send(buf, len);
        } else {
            sceKernelDelayThread(5 * 1000);
        }
    }

    return sceKernelExitDeleteThread(0);
}

static int thread_log_accept(SceSize args, void *argp) {

    while (!quit) {

        if (server_sock_log < 0) {
            sceKernelDelayThread(1000 * 1000);
            continue;
        }

        int sock = p2s_get_sock(server_sock_log);
        if (sock < 0) {
            // server socket closed (network reset or exit), thread_wait reopens it
            sceKernelDelayThread(1000 * 1000);
            continue;
        }

        int slot = -1;
        for (int i = 0; i < LOG_CLIENTS_MAX; i++) {
            if (log_socks[i] < 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0 || quit) {
            sceNetSocketClose(sock);
            continue;
        }

        int timeout = 2 * 1000 * 1000;
        sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &timeout, sizeof(timeout));
        log_socks[slot] = sock;
    }

    return sceKernelExitDeleteThread(0);
}

#endif

void _start() __attribute__ ((weak, alias ("module_start")));

int module_start(SceSize argc, const void *args) {

    // setup clients data
    client = p2s_malloc(sizeof(s_client));
    if (client == NULL) { // crap
        printf("client alloc failed\n");
        psp2shell_exit();
        sceKernelExitDeleteThread(0);
        return -1;
    }
    memset(client, 0, sizeof(s_client));
    client->msg_sock = -1;
    client->cmd_sock = -1;

#ifndef DEBUG
    for (int i = 0; i < LOG_CLIENTS_MAX; i++) {
        log_socks[i] = -1;
    }
    thid_kbuf = sceKernelCreateThread("psp2shell_kbuf", thread_kbuf, 64, 0x2000, 0, 0x10000, 0);
    if (thid_kbuf >= 0) {
        sceKernelStartThread(thid_kbuf, 0, NULL);
    }
    // started by thread_wait once the network is up
    thid_log = sceKernelCreateThread("psp2shell_log", thread_log_accept, 64, 0x2000, 0, 0x10000, 0);
#endif
    thid_wait = sceKernelCreateThread("psp2shell_wait", thread_wait, 64, 0x2000, 0, 0x10000, 0);
    if (thid_wait >= 0) {
        sceKernelStartThread(thid_wait, 0, NULL);
    }

    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
    psp2shell_exit();
    return SCE_KERNEL_STOP_SUCCESS;
}

void module_exit(void) {

}

void psp2shell_exit() {

    if (quit) {
        return;
    }

    printf("psp2shell_exit\n");
    quit = 1;

    printf("close_client\n");
    if (client != NULL) {
        if (client->msg_sock >= 0) {
            sceNetSocketClose(client->msg_sock);
            client->msg_sock = -1;
        }
        if (client->cmd_sock >= 0) {
            sceNetSocketClose(client->cmd_sock);
            client->cmd_sock = -1;
        }
        if (thid_client >= 0) {
            sceKernelWaitThreadEnd(thid_client, NULL, NULL);
        }
        p2s_free(client);
        client = NULL;
    }

#ifndef DEBUG
    for (int i = 0; i < LOG_CLIENTS_MAX; i++) {
        if (log_socks[i] >= 0) {
            sceNetSocketClose(log_socks[i]);
            log_socks[i] = -1;
        }
    }
#endif

    printf("close_server\n");
    close_server();
    if (thid_wait >= 0) {
        sceKernelWaitThreadEnd(thid_wait, NULL, NULL);
    }

    if (thid_kbuf >= 0) {
        printf("sceKernelDeleteThread: thid_kbuf\n");
        sceKernelDeleteThread(thid_kbuf);
    }
#ifndef DEBUG
    if (log_thread_started) {
        sceKernelWaitThreadEnd(thid_log, NULL, NULL);
    } else if (thid_log >= 0) {
        sceKernelDeleteThread(thid_log);
    }
#endif
}
