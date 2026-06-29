#include "protocol.h"
#include "unity.h"
#include "unity_internals.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static proto_conn *server_conn;
static proto_conn *client_conn;

void tearDown() {
    close(client_conn->tcp_fd);
    close(server_conn->tcp_fd);
    proto_free(client_conn);
    proto_free(server_conn);
}

void setUp() {
    int sp[2];
    int rv = socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    if (rv == -1) {
        perror("socketpair");
        exit(EXIT_FAILURE);
    }
    int size = 50;
    if (setsockopt(sp[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == -1) {
        perror("setsockopt SO_SNDBUF");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(sp[0], SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) == -1) {
        perror("setsockopt SO_RCVBUF");
        exit(EXIT_FAILURE);
    }
    if (setsockopt(sp[1], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == -1) {
        perror("setsockopt SO_SNDBUF");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(sp[1], SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) == -1) {
        perror("setsockopt SO_RCVBUF");
        exit(EXIT_FAILURE);
    }
    server_conn = proto_new(sp[0]);
    client_conn = proto_new(sp[1]);
}

int clog_fd(int fd) {
    char buf[4096];
    int total = 0;
    for (;;) {
        int n = send(fd, buf, sizeof(buf), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n == -1) {
            if (EWOULDBLOCK == errno || errno == EAGAIN) {
                return total;
            } else {
                exit(EXIT_FAILURE);
            }
        }
        total += n;
    }
}
void unclog_fd(int fd, int ngarbage) {
    char buf[ngarbage];
    while (ngarbage > 0) {
        int n = read(fd, buf, ngarbage);
        if (n == -1) {
            exit(EXIT_FAILURE);
        }
        ngarbage -= n;
    }
}

void test_send_receive() {
    // send
    char str[] = "hello there!!!!!!!!";
    char str2[] = "hello again!!!!!!!!";
    char str3[] = "hello again agon!!!";
    int str_size = sizeof(str);
    int str2_size = sizeof(str2);
    int str3_size = sizeof(str3);
    proto_write(server_conn, str, str_size);
    proto_write(server_conn, str2, str2_size);
    proto_write(server_conn, str3, str3_size);

    // receive
    char buf[800];
    TEST_ASSERT_EQUAL_INT(0, proto_load(client_conn));

    int n = proto_read(client_conn, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING(buf, str);
    TEST_ASSERT_EQUAL_INT(str_size, n);
    TEST_ASSERT_EQUAL_INT(0, proto_load(client_conn));

    n = proto_read(client_conn, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING(buf, str2);
    TEST_ASSERT_EQUAL_INT(str2_size, n);
    TEST_ASSERT_EQUAL_INT(0, proto_load(client_conn));

    n = proto_read(client_conn, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING(buf, str3);
    TEST_ASSERT_EQUAL_INT(str3_size, n);
}

void test_something() {
    int ngarbage = clog_fd(server_conn->tcp_fd);
    printf("n garbage == %d\n", ngarbage);
    proto_write(server_conn, "hi", 3);
    proto_write(server_conn, "hello", 6);
    proto_write(server_conn, "hello there", 12);

    TEST_ASSERT_EQUAL_size_t(14 + 8 + 5, server_conn->pending_out);
    TEST_ASSERT_EQUAL_INT(0, memcmp(server_conn->outbuf, "\x00\x05hi\0\x00\x08hello\0\x00\x0ehello there", 14 + 8 + 5));

    char buf[1000];

    unclog_fd(client_conn->tcp_fd, ngarbage);
}
int main() {
    UNITY_BEGIN();

    RUN_TEST(test_send_receive);
    RUN_TEST(test_something);

    return UNITY_END();
}
