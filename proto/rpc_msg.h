#ifndef RPC_MSG_H
#define RPC_MSG_H

#include <stdint.h>
#include <stddef.h>

/* Growable buffer for serialization */
typedef struct {
    uint8_t *data;
    uint32_t len;
    uint32_t cap;
} rpc_buf_t;

void rpc_buf_init(rpc_buf_t *b);
void rpc_buf_free(rpc_buf_t *b);
int  rpc_buf_ensure(rpc_buf_t *b, uint32_t extra);

/* Encoding */
int rpc_encode_int32(rpc_buf_t *b, int32_t val);
int rpc_encode_uint32(rpc_buf_t *b, uint32_t val);
int rpc_encode_handle(rpc_buf_t *b, uint32_t handle_id);
int rpc_encode_string(rpc_buf_t *b, const char *str);
int rpc_encode_buffer(rpc_buf_t *b, const void *data, uint32_t len);
int rpc_encode_null(rpc_buf_t *b);
int rpc_encode_raw(rpc_buf_t *b, const void *data, uint32_t len);

/* Decoding -- reads from *pos, advances *pos */
int rpc_decode_int32(const uint8_t *buf, uint32_t buflen, uint32_t *pos, int32_t *out);
int rpc_decode_uint32(const uint8_t *buf, uint32_t buflen, uint32_t *pos, uint32_t *out);
int rpc_decode_handle(const uint8_t *buf, uint32_t buflen, uint32_t *pos, uint32_t *out);
int rpc_decode_string(const uint8_t *buf, uint32_t buflen, uint32_t *pos,
                      const char **out, uint32_t *out_len);
int rpc_decode_buffer(const uint8_t *buf, uint32_t buflen, uint32_t *pos,
                      const uint8_t **out, uint32_t *out_len);
int rpc_decode_skip(const uint8_t *buf, uint32_t buflen, uint32_t *pos);

/* Request header: [uint32 seq][uint16 func_id] */
int rpc_encode_request_header(rpc_buf_t *b, uint32_t seq, uint16_t func_id);
int rpc_decode_request_header(const uint8_t *buf, uint32_t buflen, uint32_t *pos,
                              uint32_t *seq, uint16_t *func_id);

/* Response header: [uint32 seq][int32 retval][uint16 error] */
int rpc_encode_response_header(rpc_buf_t *b, uint32_t seq, int32_t retval, uint16_t error);
int rpc_decode_response_header(const uint8_t *buf, uint32_t buflen, uint32_t *pos,
                               uint32_t *seq, int32_t *retval, uint16_t *error);

#endif /* RPC_MSG_H */
