#include "protocol.h"
#include "unity.h"
#include <arpa/inet.h>
#include <asm-generic/errno-base.h>
#include <asm-generic/errno.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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
    server_conn = proto_new(sp[0]);
    client_conn = proto_new(sp[1]);
}

void clog_fd(int fd) {
    char buf[4096];
    for (;;) {
        int n = send(fd, buf, sizeof(buf), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n == -1) {
            if (EWOULDBLOCK == errno || errno == EAGAIN) {
                return;
            } else {
                exit(EXIT_FAILURE);
            }
        }
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
    clog_fd(server_conn->tcp_fd);
    proto_write(server_conn, "hi", 3);
    proto_write(server_conn, "hello", 6);
    proto_write(server_conn, "hello there", 12);

    TEST_ASSERT_EQUAL_size_t(14 + 8 + 5, server_conn->pending_out);

    TEST_ASSERT_EQUAL_INT(0, memcmp(server_conn->outbuf, "\x00\x05hi\0\x00\x08hello\0\x00\x0ehello there", 14 + 8 + 5));
}
int main() {
    RUN_TEST(test_send_receive);
    RUN_TEST(test_something);
}
