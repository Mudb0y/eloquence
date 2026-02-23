#include "shim_connection.h"
#include "shim_launch.h"
#include "eci_proto.h"
#include "rpc_io.h"
#include "rpc_msg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <pthread.h>

static shim_conn_t g_conn = {
    .rpc_fd = -1,
    .cb_fd = -1,
    .client_id = 0,
    .next_seq = 1,
    .connected = 0,
};
static pthread_once_t g_conn_once = PTHREAD_ONCE_INIT;
static int g_init_done = 0;

static void init_conn(void)
{
    pthread_mutex_init(&g_conn.rpc_lock, NULL);
    g_init_done = 1;
}

static int connect_to_socket(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int do_handshake(int fd, uint32_t client_id, uint32_t *assigned_id)
{
    /* Send handshake: [uint32 client_id] (0 = new client, >0 = callback channel) */
    uint32_t ncid = htonl(client_id);
    if (rpc_write_msg(fd, MSG_HANDSHAKE, (uint8_t *)&ncid, 4) < 0)
        return -1;

    /* Read ACK */
    uint8_t msg_type;
    uint8_t *buf = NULL;
    uint32_t buflen;
    if (rpc_read_msg(fd, &msg_type, &buf, &buflen) < 0)
        return -1;
    if (msg_type != MSG_HANDSHAKE_ACK || buflen < 4) {
        free(buf);
        return -1;
    }
    uint32_t ack_id;
    memcpy(&ack_id, buf, 4);
    ack_id = ntohl(ack_id);
    free(buf);
    if (assigned_id) *assigned_id = ack_id;
    return 0;
}

int shim_connect(shim_conn_t *conn)
{
    if (conn->connected) return 0;

    /* Launch bridge if needed */
    if (shim_launch_bridge() < 0) return -1;

    const char *path = shim_get_socket_path();

    /* Open RPC channel */
    conn->rpc_fd = connect_to_socket(path);
    if (conn->rpc_fd < 0) {
        fprintf(stderr, "shim: failed to connect RPC channel\n");
        return -1;
    }

    /* Handshake on RPC channel: client_id=0 means "new client" */
    uint32_t assigned_id = 0;
    if (do_handshake(conn->rpc_fd, 0, &assigned_id) < 0) {
        fprintf(stderr, "shim: RPC handshake failed\n");
        close(conn->rpc_fd);
        conn->rpc_fd = -1;
        return -1;
    }
    conn->client_id = assigned_id;

    /* Open callback channel */
    conn->cb_fd = connect_to_socket(path);
    if (conn->cb_fd < 0) {
        fprintf(stderr, "shim: failed to connect callback channel\n");
        close(conn->rpc_fd);
        conn->rpc_fd = -1;
        return -1;
    }

    /* Handshake on callback channel: send assigned client_id */
    if (do_handshake(conn->cb_fd, assigned_id, NULL) < 0) {
        fprintf(stderr, "shim: callback handshake failed\n");
        close(conn->cb_fd);
        close(conn->rpc_fd);
        conn->cb_fd = -1;
        conn->rpc_fd = -1;
        return -1;
    }

    conn->connected = 1;
    conn->next_seq = 1;
    return 0;
}

void shim_disconnect(shim_conn_t *conn)
{
    if (conn->rpc_fd >= 0) close(conn->rpc_fd);
    if (conn->cb_fd >= 0) close(conn->cb_fd);
    conn->rpc_fd = -1;
    conn->cb_fd = -1;
    conn->connected = 0;
}

shim_conn_t *shim_get_conn(void)
{
    pthread_once(&g_conn_once, init_conn);
    if (!g_conn.connected) {
        pthread_mutex_lock(&g_conn.rpc_lock);
        if (!g_conn.connected) {
            shim_connect(&g_conn);
        }
        pthread_mutex_unlock(&g_conn.rpc_lock);
    }
    return &g_conn;
}

int shim_rpc(shim_conn_t *conn, uint16_t func_id,
             const uint8_t *args, uint32_t args_len,
             int32_t *out_retval, uint8_t **out_buf, uint32_t *out_len)
{
    if (!conn || !conn->connected) return -1;

    pthread_mutex_lock(&conn->rpc_lock);

    /* Build request */
    rpc_buf_t req;
    rpc_buf_init(&req);
    uint32_t seq = conn->next_seq++;
    rpc_encode_request_header(&req, seq, func_id);
    if (args && args_len > 0)
        rpc_encode_raw(&req, args, args_len);

    /* Send */
    int rc = rpc_write_msg(conn->rpc_fd, MSG_RPC_REQUEST, req.data, req.len);
    rpc_buf_free(&req);
    if (rc < 0) {
        pthread_mutex_unlock(&conn->rpc_lock);
        conn->connected = 0;
        return -1;
    }

    /* Receive response */
    uint8_t msg_type;
    uint8_t *buf = NULL;
    uint32_t buflen;
    if (rpc_read_msg(conn->rpc_fd, &msg_type, &buf, &buflen) < 0 ||
        msg_type != MSG_RPC_RESPONSE) {
        free(buf);
        pthread_mutex_unlock(&conn->rpc_lock);
        conn->connected = 0;
        return -1;
    }

    pthread_mutex_unlock(&conn->rpc_lock);

    /* Parse response header */
    uint32_t pos = 0;
    uint32_t rseq;
    int32_t retval;
    uint16_t error;
    if (rpc_decode_response_header(buf, buflen, &pos, &rseq, &retval, &error) < 0) {
        free(buf);
        return -1;
    }

    if (out_retval) *out_retval = retval;

    /* Return remaining data */
    if (out_buf && out_len) {
        uint32_t remaining = buflen - pos;
        if (remaining > 0) {
            *out_buf = (uint8_t *)malloc(remaining);
            memcpy(*out_buf, buf + pos, remaining);
            *out_len = remaining;
        } else {
            *out_buf = NULL;
            *out_len = 0;
        }
    }
    free(buf);
    return 0;
}
