#define _GNU_SOURCE
#include "protocol.h"
#include "stdio.h"
#include <arpa/inet.h>
#include <asm-generic/ioctls.h>
#include <asm-generic/socket.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_PORT "10987"
#define MAX_CONNECTIONS 10

const char COMMAND = 0;
const char WINSIZE = 1;

enum fd_t { client_fdt, master_fdt, server_fdt, signal_fdt };

typedef struct _connection {
    enum fd_t fd_type;
    int master_fd;
    int server_fd;
    int signal_fd;
    proto_conn *proto_conn;
    struct _connection *other;
} connection;

void recvfrom_client(connection *conn);
void recvfrom_pty(connection *conn);
void free_child_proc();
void reg_conn(connection *conn);
void unreg_conn(connection *conn);
void handle_new_conn(int cfd);

int epfd;
int conn_count = 0;
int server_fd;
char buf[MAX_PAYLOAD_SIZE];

int main(int argc, char **argv) {
    char *port;
    if (argc == 1) {
        port = DEFAULT_PORT;
    } else if (argc == 2) {
        port = argv[1];
    } else {
        fprintf(stderr, "Usage: sshd <port>\nPort is optional (defaults to 10987)\n");
        exit(EXIT_FAILURE);
    }

    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET6;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int rv = getaddrinfo(NULL, port, &hints, &res);
    if (rv != 0) {
        fprintf(stderr, "getaddrinfo: %s", gai_strerror(rv));
        exit(EXIT_FAILURE);
    }

    for (p = res; p != NULL; p = p->ai_next) {
        server_fd = socket(p->ai_family, p->ai_socktype | SOCK_CLOEXEC, p->ai_protocol);
        if (server_fd == -1) {
            continue;
        }
        int error = bind(server_fd, p->ai_addr, p->ai_addrlen);
        if (error == 0) {
            break;
        }
    }

    if (p == NULL) {
        fprintf(stderr, "Could not bind\n");
        exit(EXIT_FAILURE);
    }

    freeaddrinfo(res);

    int yes = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    connection *server_conn = malloc(sizeof(connection));
    server_conn->server_fd = server_fd;
    server_conn->fd_type = server_fdt;
    int error = listen(server_fd, 10);
    if (error == -1) {
        perror("bind");
        exit(1);
    }
    struct epoll_event event;
    epfd = epoll_create1(EPOLL_CLOEXEC);

    reg_conn(server_conn);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
        perror("sigprocmask");
        exit(EXIT_FAILURE);
    }
    int sigfd = signalfd(-1, &mask, 0);
    connection *signal_conn = malloc(sizeof(connection));
    signal_conn->fd_type = signal_fdt;
    signal_conn->signal_fd = sigfd;

    reg_conn(signal_conn);
    for (;;) {
        if (epoll_wait(epfd, &event, 1, -1) == -1) {
            perror("epoll_wait");
            break;
        }
        uint16_t e = event.events;
        connection *conn = event.data.ptr;
        if (e & EPOLLIN) {
            if (conn->fd_type == server_fdt) {
                int cfd = accept4(server_fd, NULL, NULL, SOCK_CLOEXEC);
                handle_new_conn(cfd);
            } else if (conn->fd_type == client_fdt) {
                recvfrom_client(conn);
            } else if (conn->fd_type == master_fdt) {
                recvfrom_pty(conn);
            } else if (conn->fd_type == signal_fdt) {
                struct signalfd_siginfo si;
                read(sigfd, &si, sizeof(si));
                free_child_proc();
            } else {
                fprintf(stderr, "Unexpected fd type: %d\n", conn->fd_type);
                exit(EXIT_FAILURE);
            }
        } else {
            unreg_conn(conn);
        }
    }
}
void free_child_proc() {
    if (waitpid(-1, NULL, WNOHANG) > 0)
        printf("Child process freed\n");
}

void reg_conn(connection *conn) {
    struct epoll_event event = {.events = EPOLLIN | EPOLLRDHUP, .data.ptr = conn};
    switch (conn->fd_type) {
    case client_fdt:
        epoll_ctl(epfd, EPOLL_CTL_ADD, conn->proto_conn->tcp_fd, &event);
        printf("client fd %d registered\n", conn->proto_conn->tcp_fd);
        conn_count++;
        break;

    case master_fdt:
        epoll_ctl(epfd, EPOLL_CTL_ADD, conn->master_fd, &event);
        break;

    case server_fdt:
        epoll_ctl(epfd, EPOLL_CTL_ADD, conn->server_fd, &event);
        break;
    case signal_fdt:
        epoll_ctl(epfd, EPOLL_CTL_ADD, conn->signal_fd, &event);
        break;
    }
}

void unreg_conn(connection *conn) {
    connection *other_conn = conn->other;
    switch (conn->fd_type) {
    case client_fdt:
        epoll_ctl(epfd, EPOLL_CTL_DEL, conn->proto_conn->tcp_fd, NULL);
        epoll_ctl(epfd, EPOLL_CTL_DEL, other_conn->master_fd, NULL);
        close(conn->proto_conn->tcp_fd);
        printf("fd %d closed\n", conn->proto_conn->tcp_fd);
        close(other_conn->master_fd);
        proto_free(conn->proto_conn);
        break;
    case master_fdt:
        epoll_ctl(epfd, EPOLL_CTL_DEL, other_conn->proto_conn->tcp_fd, NULL);
        epoll_ctl(epfd, EPOLL_CTL_DEL, conn->master_fd, NULL);
        close(conn->master_fd);
        close(other_conn->proto_conn->tcp_fd);
        printf("fd %d closed\n", other_conn->proto_conn->tcp_fd);
        break;
    case server_fdt:
        break;
    case signal_fdt:
        break;
    }
    free(conn);
    free(other_conn);
    conn_count--;
}
void handle_new_conn(int cfd) {
    if (cfd == -1) {
        perror("accept");
        return;
    }
    if (conn_count >= MAX_CONNECTIONS) {
        char *msg = "Max connections reached, try again later\n";
        send(cfd, msg, strlen(msg), MSG_NOSIGNAL | MSG_DONTWAIT);
        close(cfd);
        return;
    }
    int opt = 1;
    if (setsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) == -1) {
        perror("setsockopt KEEPALIVE failed");
        return;
    }
    int masterfd;
    int pid = forkpty(&masterfd, NULL, NULL, NULL);
    if (pid == 0) {
        execlp("bash", "bash", NULL);
        perror("execlp(bash)");
    } else if (pid > 0) {
        connection *master_conn = malloc(sizeof(connection));
        connection *client_conn = malloc(sizeof(connection));
        client_conn->other = master_conn;
        master_conn->other = client_conn;
        client_conn->fd_type = client_fdt;
        client_conn->proto_conn = proto_new(cfd);
        master_conn->master_fd = masterfd;
        master_conn->fd_type = master_fdt;
        reg_conn(client_conn);
        reg_conn(master_conn);
    } else {
        char *msg = "Unexpected problem occured, please report\n";
        send(cfd, msg, strlen(msg), MSG_NOSIGNAL | MSG_DONTWAIT);
        close(cfd);
    }
}

void recvfrom_client(connection *conn) {
    int n;
    while ((n = proto_read(conn->proto_conn, buf, sizeof(buf))) > 0) {
        if (*buf == COMMAND) {
            int written = write(conn->other->master_fd, buf + 1, n - 1);
            if (written <= 0) {
                unreg_conn(conn);
                return;
            }
        } else if (*buf == WINSIZE) {
            uint16_t col;
            uint16_t row;
            uint16_t *p = (uint16_t *)(buf + sizeof(uint32_t) + 1);
            col = *(uint16_t *)(p);
            row = *(uint16_t *)(p + 1);
            struct winsize ws;
            ws.ws_col = ntohs(col);
            ws.ws_row = ntohs(row);
            if (ioctl(conn->other->master_fd, TIOCSWINSZ, &ws) == -1) {
                n = -1;
                perror("ioctl");
                return;
            }
        } else {
            fprintf(stderr, "Unexpected packet thing, ignoring!\n");
        }
    }
    if (n == -1) {
        unreg_conn(conn);
    }
}

void recvfrom_pty(connection *conn) {
    int n = read(conn->master_fd, buf, sizeof(buf));
    if (n <= 0) {
        unreg_conn(conn);
        return;
    }
    int rv = proto_write(conn->other->proto_conn, buf, n);
    if (rv < 0) {
        unreg_conn(conn);
    }
}
