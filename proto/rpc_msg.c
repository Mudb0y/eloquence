#include "rpc_msg.h"
#include "eci_proto.h"
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

void rpc_buf_init(rpc_buf_t *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void rpc_buf_free(rpc_buf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

int rpc_buf_ensure(rpc_buf_t *b, uint32_t extra)
{
    uint32_t need = b->len + extra;
    if (need <= b->cap) return 0;
    uint32_t newcap = b->cap ? b->cap : 256;
    while (newcap < need) newcap *= 2;
    uint8_t *p = (uint8_t *)realloc(b->data, newcap);
    if (!p) return -1;
    b->data = p;
    b->cap = newcap;
    return 0;
}

int rpc_encode_raw(rpc_buf_t *b, const void *data, uint32_t len)
{
    if (rpc_buf_ensure(b, len) < 0) return -1;
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return 0;
}

static int encode_tag_i32(rpc_buf_t *b, uint8_t tag, int32_t val)
{
    if (rpc_buf_ensure(b, 5) < 0) return -1;
    b->data[b->len++] = tag;
    uint32_t nv = htonl((uint32_t)val);
    memcpy(b->data + b->len, &nv, 4);
    b->len += 4;
    return 0;
}

int rpc_encode_int32(rpc_buf_t *b, int32_t val)
{
    return encode_tag_i32(b, ARG_INT32, val);
}

int rpc_encode_uint32(rpc_buf_t *b, uint32_t val)
{
    return encode_tag_i32(b, ARG_UINT32, (int32_t)val);
}

int rpc_encode_handle(rpc_buf_t *b, uint32_t handle_id)
{
    return encode_tag_i32(b, ARG_HANDLE, (int32_t)handle_id);
}

int rpc_encode_string(rpc_buf_t *b, const char *str)
{
    if (!str) return rpc_encode_null(b);
    uint32_t slen = (uint32_t)strlen(str);
    if (rpc_buf_ensure(b, 5 + slen) < 0) return -1;
    b->data[b->len++] = ARG_STRING;
    uint32_t nlen = htonl(slen);
    memcpy(b->data + b->len, &nlen, 4);
    b->len += 4;
    memcpy(b->data + b->len, str, slen);
    b->len += slen;
    return 0;
}

int rpc_encode_buffer(rpc_buf_t *b, const void *data, uint32_t len)
{
    if (!data) return rpc_encode_null(b);
    if (rpc_buf_ensure(b, 5 + len) < 0) return -1;
    b->data[b->len++] = ARG_BUFFER;
    uint32_t nlen = htonl(len);
    memcpy(b->data + b->len, &nlen, 4);
    b->len += 4;
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return 0;
}

int rpc_encode_null(rpc_buf_t *b)
{
    if (rpc_buf_ensure(b, 1) < 0) return -1;
    b->data[b->len++] = ARG_NULL;
    return 0;
}

/* Decoding */
static int check_remaining(uint32_t buflen, uint32_t pos, uint32_t need)
{
    return (pos + need <= buflen) ? 0 : -1;
}

static int decode_tag_i32(const uint8_t *buf, uint32_t buflen, uint32_t *pos,
                          uint8_t expected_tag, int32_t *out)
{
    if (check_remaining(buflen, *pos, 5) < 0) return -1;
    uint8_t tag = buf[*pos];
    if (tag != expected_tag) return -1;
    (*pos)++;
    uint32_t nv;
    memcpy(&nv, buf + *pos, 4);
    *pos += 4;
    *out = (int32_t)ntohl(nv);
    return 0;
}

int rpc_decode_int32(const uint8_t *buf, uint32_t buflen, uint32_t *pos, int32_t *out)
{
    return decode_tag_i32(buf, buflen, pos, ARG_INT32, out);
}

int rpc_decode_uint32(const uint8_t *buf, uint32_t buflen, uint32_t *pos, uint32_t *out)
{
    int32_t v;
    if (decode_tag_i32(buf, buflen, pos, ARG_UINT32, &v) < 0) return -1;
    *out = (uint32_t)v;
    return 0;
}

int rpc_decode_handle(const uint8_t *buf, uint32_t buflen, uint32_t *pos, uint32_t *out)
{
    int32_t v;
    if (decode_tag_i32(buf, buflen, pos, ARG_HANDLE, &v) < 0) return -1;
    *out = (uint32_t)v;
    return 0;
}

int rpc_decode_string(const uint8_t *buf, uint32_t buflen, uint32_t *pos,
                      const char **out, uint32_t *out_len)
{
    if (check_remaining(buflen, *pos, 1) < 0) return -1;
    uint8_t tag = buf[*pos];
    if (tag == ARG_NULL) {
        (*pos)++;
        *out = NULL;
        *out_len = 0;
        return 0;
    }
    if (tag != ARG_STRING) return -1;
    if (check_remaining(buflen, *pos, 5) < 0) return -1;
    (*pos)++;
    uint32_t nlen;
    memcpy(&nlen, buf + *pos, 4);
    *pos += 4;
    uint32_t slen = ntohl(nlen);
    if (slen > MAX_STRING_LEN) return -1;
    if (check_remaining(buflen, *pos, slen) < 0) return -1;
    *out = (const char *)(buf + *pos);
    *out_len = slen;
    *pos += slen;
    return 0;
}

int rpc_decode_buffer(const uint8_t *buf, uint32_t buflen, uint32_t *pos,
                      const uint8_t **out, uint32_t *out_len)
{
    if (check_remaining(buflen, *pos, 1) < 0) return -1;
    uint8_t tag = buf[*pos];
    if (tag == ARG_NULL) {
        (*pos)++;
        *out = NULL;
        *out_len = 0;
        return 0;
    }
    if (tag != ARG_BUFFER) return -1;
    if (check_remaining(buflen, *pos, 5) < 0) return -1;
    (*pos)++;
    uint32_t nlen;
    memcpy(&nlen, buf + *pos, 4);
    *pos += 4;
    uint32_t dlen = ntohl(nlen);
    if (dlen > MAX_BUFFER_SIZE) return -1;
    if (check_remaining(buflen, *pos, dlen) < 0) return -1;
    *out = buf + *pos;
    *out_len = dlen;
    *pos += dlen;
    return 0;
}

int rpc_decode_skip(const uint8_t *buf, uint32_t buflen, uint32_t *pos)
{
    if (check_remaining(buflen, *pos, 1) < 0) return -1;
    uint8_t tag = buf[*pos];
    switch (tag) {
    case ARG_INT32: case ARG_UINT32: case ARG_HANDLE:
        *pos += 5;
        return check_remaining(buflen, *pos - 4, 4);
    case ARG_NULL:
        (*pos)++;
        return 0;
    case ARG_STRING: case ARG_BUFFER: {
        if (check_remaining(buflen, *pos, 5) < 0) return -1;
        uint32_t nlen;
        memcpy(&nlen, buf + *pos + 1, 4);
        uint32_t dlen = ntohl(nlen);
        *pos += 5 + dlen;
        return check_remaining(buflen, *pos - 1, 1) < 0 ? -1 : 0;
    }
    default:
        return -1;
    }
}

int rpc_encode_request_header(rpc_buf_t *b, uint32_t seq, uint16_t func_id)
{
    if (rpc_buf_ensure(b, 6) < 0) return -1;
    uint32_t ns = htonl(seq);
    memcpy(b->data + b->len, &ns, 4);
    b->len += 4;
    uint16_t nf = htons(func_id);
    memcpy(b->data + b->len, &nf, 2);
    b->len += 2;
    return 0;
}

int rpc_decode_request_header(const uint8_t *buf, uint32_t buflen, uint32_t *pos,
                              uint32_t *seq, uint16_t *func_id)
{
    if (check_remaining(buflen, *pos, 6) < 0) return -1;
    uint32_t ns;
    memcpy(&ns, buf + *pos, 4);
    *seq = ntohl(ns);
    *pos += 4;
    uint16_t nf;
    memcpy(&nf, buf + *pos, 2);
    *func_id = ntohs(nf);
    *pos += 2;
    return 0;
}

int rpc_encode_response_header(rpc_buf_t *b, uint32_t seq, int32_t retval, uint16_t error)
{
    if (rpc_buf_ensure(b, 10) < 0) return -1;
    uint32_t ns = htonl(seq);
    memcpy(b->data + b->len, &ns, 4);
    b->len += 4;
    uint32_t nr = htonl((uint32_t)retval);
    memcpy(b->data + b->len, &nr, 4);
    b->len += 4;
    uint16_t ne = htons(error);
    memcpy(b->data + b->len, &ne, 2);
    b->len += 2;
    return 0;
}

int rpc_decode_response_header(const uint8_t *buf, uint32_t buflen, uint32_t *pos,
                               uint32_t *seq, int32_t *retval, uint16_t *error)
{
    if (check_remaining(buflen, *pos, 10) < 0) return -1;
    uint32_t ns;
    memcpy(&ns, buf + *pos, 4);
    *seq = ntohl(ns);
    *pos += 4;
    uint32_t nr;
    memcpy(&nr, buf + *pos, 4);
    *retval = (int32_t)ntohl(nr);
    *pos += 4;
    uint16_t ne;
    memcpy(&ne, buf + *pos, 2);
    *error = ntohs(ne);
    *pos += 2;
    return 0;
}
