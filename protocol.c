#include "protocol.h"
#include <arpa/inet.h>
#include <asm-generic/errno-base.h>
#include <asm-generic/errno.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

proto_conn *proto_new(int tcp_fd) {
    proto_conn *p = malloc(sizeof(proto_conn));
    p->tcp_fd = tcp_fd;
    p->inbuf = malloc(INBUF_SIZE);
    p->outbuf = malloc(OUTBUF_SIZE);
    p->pending_out = 0;
    p->pending_in = 0;
    p->inlen = INBUF_SIZE;
    p->outlen = OUTBUF_SIZE;
    p->out_ptr = 0;
    p->in_ptr = 0;

    return p;
}
void proto_free(proto_conn *conn) {
    free(conn->inbuf);
    free(conn->outbuf);
    free(conn);
}

static int queue_bytes(proto_conn *conn, char *buf, size_t len) {
    if (len == 0) {
        return 0;
    }
    if (conn->outlen - conn->pending_out < len) {
        return -1;
    }
    int head = (conn->out_ptr + conn->pending_out) % conn->outlen;

    if (head + len - 1 >= conn->outlen) {
        int part1len = conn->outlen - head;
        int part2len = len - part1len;
        memcpy(conn->outbuf + head, buf, part1len);
        memcpy(conn->outbuf, buf + part1len, part2len);
    } else {
        memcpy(conn->outbuf + head, buf, len);
    }
    conn->pending_out += len;
    return 0;
}

int proto_flush(proto_conn *conn) {
    while (conn->pending_out != 0) {
        if (conn->out_ptr + conn->pending_out - 1 >= conn->outlen) {
            int n = send(conn->tcp_fd, conn->outbuf + conn->out_ptr, conn->outlen - conn->out_ptr,
                         MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return conn->pending_out;
                }
                return -1;
            }
            conn->pending_out -= n;
            conn->out_ptr = (conn->out_ptr + n) % conn->outlen;
        } else {
            int n = send(conn->tcp_fd, conn->outbuf + conn->out_ptr, conn->pending_out, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return conn->pending_out;
                }
                return -1;
            }
            conn->pending_out -= n;
            conn->out_ptr = (conn->out_ptr + n) % conn->outlen;
        }
    }
    return 0;
}

static int proto_load(proto_conn *conn) {
    size_t available_space = conn->inlen - conn->pending_in;
    if (available_space == 0) {
        return 0;
    }

    size_t head = (conn->in_ptr + conn->pending_in) % conn->inlen;
    if (conn->in_ptr > head) {
        ssize_t n = recv(conn->tcp_fd, conn->inbuf + head, available_space, MSG_DONTWAIT);
        if (n <= 0) {
            if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return 0;
            return -1;
        }
        conn->pending_in += n;
        return 0;

    } else {
        ssize_t n = recv(conn->tcp_fd, conn->inbuf + head, conn->inlen - head, MSG_DONTWAIT);
        if (n <= 0) {
            if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return 0;
            return -1;
        }

        conn->pending_in += n;
        available_space -= n;
        if (available_space == 0) {
            return 0;
        }
        n = recv(conn->tcp_fd, conn->inbuf, available_space, MSG_DONTWAIT);
        if (n <= 0) {
            if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return 0;
            return -1;
        }
        conn->pending_in += n;
        return 0;
    }
}
/*
PACKET:
[2 size] [n body]
*/
int proto_read(proto_conn *conn, char *buf, size_t len) {

    if (proto_load(conn) < 0) {
        return -1;
    }

    if (conn->pending_in <= 2) {
        return 0;
    }
    uint16_t packlen;
    char *p = (char *)&packlen;
    memcpy(p, conn->inbuf + conn->in_ptr, 1);
    int next = (conn->in_ptr + 1) % conn->inlen;
    memcpy(p + 1, conn->inbuf + next, 1);
    packlen = ntohs(packlen);
    if (packlen > MAX_PACKET_SIZE) {
        return -1;
    }
    if (packlen <= 2) {
        return -1;
    }
    if (packlen > conn->pending_in) {
        return 0;
    }
    uint16_t payloadlen = packlen - 2;
    if (len < payloadlen) {
        return -1;
    }
    conn->in_ptr = (conn->in_ptr + 2) % conn->inlen;

    if (conn->in_ptr + payloadlen - 1 >= conn->inlen) {
        int part1len = conn->inlen - conn->in_ptr;
        int part2len = payloadlen - part1len;
        memcpy(buf, conn->inbuf + conn->in_ptr, part1len);
        memcpy(buf + part1len, conn->inbuf, part2len);
        conn->in_ptr = part2len;
    } else {
        memcpy(buf, conn->inbuf + conn->in_ptr, payloadlen);
        conn->in_ptr = (conn->in_ptr + payloadlen) % conn->inlen;
    }
    conn->pending_in -= packlen;
    return payloadlen;
}

int proto_write(proto_conn *conn, char *buf, size_t len) {

    if (len == 0) {
        return -1;
    }
    int rv = proto_flush(conn);
    if (rv == -1) {
        return -1;
    }

    if (len > MAX_PAYLOAD_SIZE) {
        return -1;
    }
    uint16_t packlen = len + 2;

    char packet[len + 2];
    uint16_t headerlen = htons(len + 2);
    memcpy(packet, &headerlen, 2);
    memcpy(packet + 2, buf, len);

    if (rv > 0) {
        return queue_bytes(conn, packet, packlen);
    }
    // if rv == 0:
    int total = 0;
    while (total != packlen) {
        int sentnow = send(conn->tcp_fd, packet + total, packlen - total, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sentnow == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return -1;
        }
        total += sentnow;
    }

    return queue_bytes(conn, packet + total, packlen - total);
}
