#ifndef RPC_IO_H
#define RPC_IO_H

#include <stdint.h>
#include <stddef.h>

/* Framed I/O: [uint32_t payload_len][payload...]
 * Payload starts with uint8_t msg_type. */

/* Read exactly n bytes. Returns 0 on success, -1 on error/EOF. */
int rpc_read_exact(int fd, void *buf, size_t n);

/* Write exactly n bytes. Returns 0 on success, -1 on error. */
int rpc_write_exact(int fd, const void *buf, size_t n);

/* Read a framed message. Allocates *out_buf (caller must free).
 * Returns payload length, or -1 on error. */
int rpc_read_msg(int fd, uint8_t *out_type, uint8_t **out_buf, uint32_t *out_len);

/* Write a framed message. */
int rpc_write_msg(int fd, uint8_t msg_type, const uint8_t *buf, uint32_t len);

#endif /* RPC_IO_H */
