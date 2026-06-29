#include <stdint.h>
#include <sys/types.h>

#ifndef MAX_PACKET_SIZE
#define MAX_PACKET_SIZE (16 * 1024)
#endif

#ifndef MAX_PAYLOAD_SIZE
#define MAX_PAYLOAD_SIZE ((16 * 1024) - 2)
#endif

#ifndef INBUF_SIZE
#define INBUF_SIZE (1024 * 1024)
#endif

#ifndef OUTBUF_SIZE
#define OUTBUF_SIZE (5 * 1024 * 1024)
#endif

typedef struct _proto_conn {
    int tcp_fd;
    char *inbuf;
    char *outbuf;
    size_t inlen;
    size_t outlen;
    size_t pending_in;
    size_t pending_out;
    size_t in_ptr;
    size_t out_ptr;
} proto_conn;

// Takes address, port and return a file descriptor.
// int proto_listen(char *address, char *port);

int proto_flush(proto_conn *conn);
int proto_load(proto_conn *conn);
int proto_read(proto_conn *conn, char *buf, size_t len);
int proto_write(proto_conn *conn, char *buf, size_t len);
proto_conn *proto_new(int tcp_fd);
void proto_free(proto_conn *con);
