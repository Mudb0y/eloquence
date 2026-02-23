#include "rpc_io.h"
#include "eci_proto.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>

int rpc_read_exact(int fd, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    size_t remaining = n;
    while (remaining > 0) {
        ssize_t r = read(fd, p, remaining);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1; /* EOF */
        p += r;
        remaining -= (size_t)r;
    }
    return 0;
}

int rpc_write_exact(int fd, const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = n;
    while (remaining > 0) {
        ssize_t w = write(fd, p, remaining);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        remaining -= (size_t)w;
    }
    return 0;
}

int rpc_read_msg(int fd, uint8_t *out_type, uint8_t **out_buf, uint32_t *out_len)
{
    uint32_t net_len;
    if (rpc_read_exact(fd, &net_len, 4) < 0) return -1;
    uint32_t payload_len = ntohl(net_len);
    if (payload_len < 1 || payload_len > MAX_MSG_SIZE) return -1;

    uint8_t msg_type;
    if (rpc_read_exact(fd, &msg_type, 1) < 0) return -1;
    *out_type = msg_type;

    uint32_t data_len = payload_len - 1;
    *out_len = data_len;
    if (data_len == 0) {
        *out_buf = NULL;
        return 0;
    }

    *out_buf = (uint8_t *)malloc(data_len);
    if (!*out_buf) return -1;

    if (rpc_read_exact(fd, *out_buf, data_len) < 0) {
        free(*out_buf);
        *out_buf = NULL;
        return -1;
    }
    return 0;
}

int rpc_write_msg(int fd, uint8_t msg_type, const uint8_t *buf, uint32_t len)
{
    uint32_t payload_len = 1 + len;
    uint32_t net_len = htonl(payload_len);
    if (rpc_write_exact(fd, &net_len, 4) < 0) return -1;
    if (rpc_write_exact(fd, &msg_type, 1) < 0) return -1;
    if (len > 0 && buf) {
        if (rpc_write_exact(fd, buf, len) < 0) return -1;
    }
    return 0;
}
