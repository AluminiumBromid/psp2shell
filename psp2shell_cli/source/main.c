#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <termios.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>

#include "p2s_cmd.h"
#include "p2s_msg.h"
#include "cmd.h"
#include "utility.h"
#include "main.h"
#include "errno.h"

int msg_sock = -1;
int cmd_sock = -1;
int log_sock = -1;
int done = 0;
char **_argv;

void *msg_thread(void *unused);

void *log_thread(void *unused);

// readline
char history_path[512];

void process_line(char *line);

tcflag_t old_lflag;
cc_t old_vtime;
struct termios term;
int readline_callback = 0;
// readline

void setup_terminal() {
    if (tcgetattr(STDIN_FILENO, &term) < 0) {
        perror("tcgetattr");
        exit(1);
    }
    old_lflag = term.c_lflag;
    old_vtime = term.c_cc[VTIME];
    term.c_lflag &= ~ICANON;
    term.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &term) < 0) {
        perror("tcsetattr");
        exit(1);
    }
    rl_callback_handler_install("psp2shell> ", process_line);
    readline_callback = 1;
}

void close_terminal() {
    term.c_lflag = old_lflag;
    term.c_cc[VTIME] = old_vtime;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &term) < 0) {
        perror("tcsetattr");
        exit(1);
    }
    readline_callback = 0;
    rl_callback_handler_remove();
    fflush(stdin);
}

void reset_terminal(void) {
    rl_replace_line("", 0);
    rl_refresh_line(0, 0);
}

void sig_handler(int sig) {
    if (sig == SIGINT) {
        reset_terminal();
    }
}

void close_socks() {
    close(msg_sock);
    close(cmd_sock);
    if (log_sock >= 0) {
        close(log_sock);
    }
    msg_sock = -1;
    cmd_sock = -1;
    log_sock = -1;
}

// single connection attempt, -1 if nobody listens
int connect_once(char *ip, int port) {

    struct sockaddr_in addr;
    bzero((char *) &addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip);
    addr.sin_port = htons((uint16_t) port);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    if (connect(sock, (struct sockaddr *) &addr, sizeof(struct sockaddr)) < 0) {
        close(sock);
        return -1;
    }

    return sock;
}

int get_sock(int sock, char *ip, int port, bool verbose) {

    struct sockaddr_in addr;
    bzero((char *) &addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip);
    addr.sin_port = htons((uint16_t) port);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("%s\n", strerror(errno));
        return -1;
    }

    if (verbose) {
        printf("connecting to %s:%d ... ", ip, port);
        fflush(stdout);
    }

    while (connect(sock, (struct sockaddr *) &addr, sizeof(struct sockaddr)) < 0) {
        if (errno != EINPROGRESS && errno != EALREADY) {
            if (verbose) {
                printf("%s\n", strerror(errno));
            }
        } else {
            if (verbose) {
                printf("\n");
            }
        }
        if (verbose) {
            printf("connecting to %s:%d ... ", ip, port);
            fflush(stdout);
        }
        sleep(2);
    }

    return sock;
}

int connect_psp2(char *address, int port) {

    if ((msg_sock = get_sock(msg_sock, address, port, true)) < 0) {
        printf("get_sock failed (port=%i)\n", port);
        return msg_sock;
    }

    pthread_t resp_thread;
    if (pthread_create(&resp_thread, NULL, msg_thread, NULL) < 0) {
        printf("could not create thread\n");
        exit(EXIT_FAILURE);
    }

    if ((cmd_sock = get_sock(cmd_sock, address, port + 1, false)) < 0) {
        printf("get_sock failed (port=%i)\n", port + 1);
        close(msg_sock);
        return cmd_sock;
    }

    // Device logs come on their own port (port + 2). Older modules don't have it and mix
    // logs into msg_sock instead. PSP2SHELL_NO_LOG=1 leaves the log port to another reader.
    if (getenv("PSP2SHELL_NO_LOG") == NULL) {
        log_sock = connect_once(address, port + 2);
        pthread_t log_tid;
        if (log_sock >= 0 && pthread_create(&log_tid, NULL, log_thread, NULL) != 0) {
            close(log_sock);
            log_sock = -1;
        }
    }

    setup_terminal();
    // catch CTRL+C
    signal(SIGINT, sig_handler);

    return 0;
}

void print_hex(char *line) {

    size_t num_tokens;
    char **tokens = strsplit(line, " ", &num_tokens);

    if (num_tokens == 5) {

        unsigned char chars[16];
        memset(chars, 0, sizeof(unsigned char) * 16);

        for (int i = 0; i < 4; i++) {

            unsigned int hex = (unsigned int) strtoul(tokens[i + 1], NULL, 16);

            int index = i + (i * 3);
            chars[index] = (unsigned char) ((hex >> 24) & 0xFF);
            if (!isprint(chars[index])) {
                chars[index] = '.';
            }
            chars[index + 1] = (unsigned char) ((hex >> 16) & 0xFF);
            if (!isprint(chars[index + 1])) {
                chars[index + 1] = '.';
            }
            chars[index + 2] = (unsigned char) ((hex >> 8) & 0xFF);
            if (!isprint(chars[index + 2])) {
                chars[index + 2] = '.';
            }
            chars[index + 3] = (unsigned char) hex;
            if (!isprint(chars[index + 3])) {
                chars[index + 3] = '.';
            }
        }

        line[strlen(line) - 1] = '\0';

        printf(GRN "%s | %c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c\n" RES,
               line,
               chars[0], chars[1], chars[2], chars[3], chars[4],
               chars[5], chars[6], chars[7], chars[8], chars[9],
               chars[10], chars[11], chars[12], chars[13], chars[14], chars[15]);
        rl_refresh_line(0, 0);
    }
}

void *msg_thread(void *unused) {

    P2S_MSG msg;

    // receive message from psp2shell
    while (true) {

        int res = p2s_msg_receive(msg_sock, &msg);
        if (res != 0) {
            if (res == P2S_ERR_SOCKET) {
                printf("p2s_msg_receive sock failed: 0x%08X\n", res);
                break;
            } else {
                // print raw data (sceClibPrintf, ksceDebugPrintf...)
                msg.color = COL_NONE;
            }
        }

        switch (msg.color) {
            case COL_RED:
                printf(RED "%s" RES, msg.buffer);
                send(msg_sock, "\n", 1, 0);
                break;
            case COL_YELLOW:
                printf(YEL "%s" RES, msg.buffer);
                send(msg_sock, "\n", 1, 0);
                break;
            case COL_GREEN:
                printf(GRN "%s" RES, msg.buffer);
                send(msg_sock, "\n", 1, 0);
                break;
            case COL_HEX:
                print_hex(msg.buffer);
                send(msg_sock, "\n", 1, 0);
                break;
            default:
                printf("%s", msg.buffer);
                break;
        }

        fflush(stdout);
        ssize_t len = strlen(msg.buffer);
        if (msg.buffer[len - 1] == '\n') { // allow printing to the shell without new line
            rl_refresh_line(0, 0);
        }
    }

    printf("disconnected\n");
    signal(SIGINT, SIG_DFL);
    close_terminal();
    close(msg_sock);
    close(cmd_sock);
    msg_sock = -1;
    cmd_sock = -1;

    // restart
    execvp(_argv[0], _argv);
}

void *log_thread(void *unused) {

    char buffer[4096];

    while (true) {
        ssize_t len = recv(log_sock, buffer, sizeof(buffer), 0);
        if (len <= 0) {
            break;
        }
        fwrite(buffer, 1, (size_t) len, stdout);
        fflush(stdout);
        if (buffer[len - 1] == '\n' && readline_callback) {
            rl_refresh_line(0, 0);
        }
    }

    // msg_thread notices the disconnect and restarts the client
    return NULL;
}

void process_line(char *line) {

    if (line == NULL) {
        close_terminal();
        exit(0);
    }

    if (strlen(line) > 0) {

        // handle history
        add_history(line);
        append_history(1, history_path);

        // parse cmd
        size_t num_tokens;
        char **tokens = strsplit(line, " ", &num_tokens);
        if (num_tokens > 0) {
            COMMAND *c = cmd_find(tokens[0]);
            if (c == NULL) {
                printf("Command not found. Use ? for help.\n");
            } else {
                printf("\n");
                c->func((int) num_tokens, tokens);
                printf("\n");
            }
        }

        if (tokens != NULL) {
            for (int i = 0; i < num_tokens; i++) {
                if (tokens[i] != NULL)
                    free(tokens[i]);
            }
            free(tokens);
        }
    }

    free(line);
}

int process_args(int argc, char **argv) {

    char *line = (char *) malloc(1024);

    connect_psp2(argv[1], strtol(argv[2], NULL, 10));

    memset(line, 0, 1024);
    for (int i = 3; i < argc; i++) {
        sprintf(line + strlen(line), "%s ", argv[i]);
    }
    process_line(line);
    close_terminal();

    return 0;
}

int main(int argc, char **argv) {

    fd_set fds;

    if (argc < 3) {
        fprintf(stderr, "Usage %s <address> <port_num> (<cmd>)\n", argv[0]);
        return EXIT_FAILURE;
    } else if (argc > 3) {
        process_args(argc, argv);
        exit(0);
    }

    _argv = argv;

    // load history from file
    memset(history_path, 0, 512);
    snprintf(history_path, 512, "%s/.psp2shell_history", getenv("HOME"));
    if (read_history(history_path) != 0) { // append_history needs the file created
        write_history(history_path);
    }

    while (!done) {

        if (msg_sock < 0) {
            if (connect_psp2(argv[1], strtol(argv[2], NULL, 10)) < 0) {
                break;
            }
        } else {

            int error = 0;
            socklen_t len = sizeof(error);

            int ret = getsockopt(msg_sock, SOL_SOCKET, SO_ERROR, &error, &len);
            if (ret != 0 || error != 0) {
                printf("socket is dead...\n");
                close_socks();
                close_terminal();
            }

            ret = getsockopt(cmd_sock, SOL_SOCKET, SO_ERROR, &error, &len);
            if (ret != 0 || error != 0) {
                printf("socket is dead...\n");
                close_socks();
                close_terminal();
            }

            FD_ZERO(&fds);
            FD_SET(fileno(stdin), &fds);
            if (select(fileno(stdin) + 1, &fds, NULL, NULL, NULL) < 0) {
                //continue (CTRL+C)
            } else if (FD_ISSET(fileno(stdin), &fds) && readline_callback) {
                rl_callback_read_char();
            }
        }
    }

    close_terminal();
    return -1;
}
