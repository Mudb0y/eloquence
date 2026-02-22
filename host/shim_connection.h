#ifndef SHIM_CONNECTION_H
#define SHIM_CONNECTION_H

#include <stdint.h>
#include <pthread.h>

typedef struct {
    int       rpc_fd;
    int       cb_fd;
    uint32_t  client_id;
    uint32_t  next_seq;
    pthread_mutex_t rpc_lock;
    int       connected;
} shim_conn_t;

/* Get the global connection (auto-connects and auto-launches bridge) */
shim_conn_t *shim_get_conn(void);

/* Connect to bridge. Returns 0 on success. */
int shim_connect(shim_conn_t *conn);

/* Disconnect */
void shim_disconnect(shim_conn_t *conn);

/* Send an RPC request and receive the response.
 * Caller must free *out_buf. Returns 0 on success. */
int shim_rpc(shim_conn_t *conn, uint16_t func_id,
             const uint8_t *args, uint32_t args_len,
             int32_t *out_retval, uint8_t **out_buf, uint32_t *out_len);

#endif /* SHIM_CONNECTION_H */
