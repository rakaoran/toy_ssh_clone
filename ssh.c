#include "protocol.h"
#include <arpa/inet.h>
#include <asm-generic/errno-base.h>
#include <asm-generic/ioctls.h>
#include <asm-generic/socket.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define DEFAULT_PORT "10987"

const char COMMAND = 0;
const char WINSIZE = 1;

int send_win_size();
void sigwinch_handler(int _);
uint32_t pack(char *inbuf, uint16_t in_size, char type, char *outbuf, uint16_t out_size);
void enable_raw_mode();
void disable_raw_mode();
void get_win_size(uint16_t *col, uint16_t *row);

int server_fd;
int sigfd;
proto_conn *conn;

int main(int argc, char **argv) {

    char *port;
    char *remote_server;
    if (argc <= 1 || argc > 3) {
        fprintf(stderr, "Usage: ssh <remote_address> <port>\nPort is optional (defaults to 10987)\n");
        exit(EXIT_FAILURE);
    }
    remote_server = argv[1];
    if (argc == 2) {
        port = DEFAULT_PORT;
    } else {
        port = argv[2];
    }

    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_socktype = SOCK_STREAM;

    int rv = getaddrinfo(remote_server, port, &hints, &res);
    if (rv != 0) {
        fprintf(stderr, "%s", gai_strerror(rv));
        exit(EXIT_FAILURE);
    }

    for (p = res; p != NULL; p = p->ai_next) {
        server_fd = socket(p->ai_family, p->ai_socktype | SOCK_CLOEXEC, p->ai_protocol);
        if (server_fd == -1) {
            continue;
        }
        rv = connect(server_fd, p->ai_addr, p->ai_addrlen);
        if (rv == 0) {
            break;
        }
    }

    if (p == NULL) {
        fprintf(stderr, "Could not connect\n");
        exit(EXIT_FAILURE);
    }

    freeaddrinfo(res);

    enable_raw_mode();
    atexit(disable_raw_mode);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGWINCH);

    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
        perror("sigprocmask");
        exit(EXIT_FAILURE);
    }

    if ((sigfd = signalfd(-1, &mask, 0)) == -1) {
        perror("sigprocmask");
        exit(EXIT_FAILURE);
    }

    conn = proto_new(server_fd);

    send_win_size();
    int epfd = epoll_create1(EPOLL_CLOEXEC);

    struct epoll_event server_event = {0};
    server_event.events = EPOLLIN | EPOLLHUP | EPOLLRDHUP | EPOLLERR;
    server_event.data.fd = server_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &server_event)) {
        perror("epollctl add serverfd");
        exit(EXIT_FAILURE);
    }

    struct epoll_event stdin_event = {0};
    stdin_event.events = EPOLLIN | EPOLLHUP | EPOLLRDHUP | EPOLLERR;
    stdin_event.data.fd = STDIN_FILENO;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, STDIN_FILENO, &stdin_event) == -1) {
        perror("epollctl add stdin");
        exit(EXIT_FAILURE);
    }

    struct epoll_event signal_event = {0};
    signal_event.events = EPOLLIN;
    signal_event.data.fd = sigfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, sigfd, &signal_event) == -1) {
        perror("epollctl add sigfd");
        exit(EXIT_FAILURE);
    }

    struct epoll_event event;
    char packetbuf[MAX_PACKET_SIZE];
    char buf[MAX_PAYLOAD_SIZE];
    char c;
    for (;;) {
        rv = epoll_wait(epfd, &event, 1, -1);
        if (rv == -1) {
            perror("epoll_wait");
            break;
        }
        if (event.events & (EPOLLIN)) {
            if (event.data.fd == server_fd) {
                int n;
                while ((n = proto_read(conn, buf, sizeof(buf))) > 0) {
                    if (n < 0) {
                        break;
                    }
                    rv = write(STDOUT_FILENO, buf, n);
                    if (rv == -1) {
                        perror("write to stdout");
                        break;
                    }
                }
            } else if (event.data.fd == STDIN_FILENO) {
                int n = read(STDIN_FILENO, &c, 1);
                if (n == -1) {
                    perror("read from stdin");
                    break;
                }
                if (n == 0) {
                    break;
                }
                int packet_size = pack(&c, 1, COMMAND, packetbuf, sizeof(packetbuf));
                rv = proto_write(conn, packetbuf, packet_size);
                if (rv < 0) {
                    fprintf(stderr, "write to server failed\n");
                    exit(EXIT_FAILURE);
                }
            } else if (event.data.fd == sigfd) {
                struct signalfd_siginfo s;
                read(sigfd, &s, sizeof(s));
                send_win_size();
            }
        } else {
            break;
        }
    }
    close(server_fd);
    close(epfd);
}
struct termios old, new;
void enable_raw_mode() {
    tcgetattr(0, &old);
    new = old;

    cfmakeraw(&new);
    tcsetattr(STDIN_FILENO, 0, &new);
}

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, 0, &old);
}

int send_win_size() {
    struct winsize ws;

    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1) {
        perror("ioctl");
        exit(EXIT_FAILURE);
    }
    char packedbuf[100];
    uint16_t winsize[2];
    *winsize = htons(ws.ws_col);
    *(winsize + 1) = htons(ws.ws_row);
    uint32_t n = pack((char *)winsize, sizeof(winsize), WINSIZE, packedbuf, sizeof(packedbuf));
    int err = proto_write(conn, packedbuf, n);
    if (err < 0) {
        perror("send winsize");
        return err;
    }
    return 0;
}

uint32_t pack(char *inbuf, uint16_t in_size, char type, char *outbuf, uint16_t out_size) {
    uint32_t total_size = in_size + 1;
    if (out_size < total_size) {
        return -1;
    }
    *outbuf = type;
    memcpy(outbuf + 1, inbuf, in_size);
    return total_size;
}
