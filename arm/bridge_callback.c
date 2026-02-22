#include "bridge_callback.h"
#include "bridge_handle.h"
#include "eci_proto.h"
#include "rpc_io.h"
#include "rpc_msg.h"
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

/* Forward declarations from bridge_dispatch */
extern cb_state_t *find_cb_state_by_eci_ptr(void *eci_hand);

enum ECICallbackReturn bridge_internal_callback(ECIHand hEngine,
                                                enum ECIMessage msg,
                                                long lParam,
                                                void *pData)
{
    cb_state_t *cbs = find_cb_state_by_eci_ptr(hEngine);
    if (!cbs || !cbs->has_callback || cbs->cb_fd < 0)
        return eciDataProcessed;

    /* Build callback event message:
     * [uint32 handle_id][int32 msg][int32 lparam][uint32 data_len][data...] */
    rpc_buf_t buf;
    rpc_buf_init(&buf);

    uint32_t nh = htonl(cbs->eci_handle_id);
    rpc_encode_raw(&buf, &nh, 4);

    uint32_t nm = htonl((uint32_t)msg);
    rpc_encode_raw(&buf, &nm, 4);

    uint32_t nl = htonl((uint32_t)lParam);
    rpc_encode_raw(&buf, &nl, 4);

    /* For waveform buffer: send PCM data */
    if (msg == eciWaveformBuffer && cbs->output_buffer && lParam > 0) {
        uint32_t data_bytes = (uint32_t)(lParam * 2); /* lParam = num samples, 16-bit */
        uint32_t nd = htonl(data_bytes);
        rpc_encode_raw(&buf, &nd, 4);
        rpc_encode_raw(&buf, cbs->output_buffer, data_bytes);
    } else {
        uint32_t zero = 0;
        rpc_encode_raw(&buf, &zero, 4);
    }

    /* Send event on callback channel */
    pthread_mutex_lock(cbs->cb_write_lock);
    int rc = rpc_write_msg(cbs->cb_fd, MSG_CALLBACK_EVENT, buf.data, buf.len);
    pthread_mutex_unlock(cbs->cb_write_lock);
    rpc_buf_free(&buf);

    if (rc < 0)
        return eciDataProcessed;

    /* Wait for callback return from client */
    uint8_t rtype;
    uint8_t *rbuf = NULL;
    uint32_t rlen;
    if (rpc_read_msg(cbs->cb_fd, &rtype, &rbuf, &rlen) < 0 ||
        rtype != MSG_CALLBACK_RETURN) {
        free(rbuf);
        return eciDataProcessed;
    }

    /* Parse: [uint32 handle_id][int32 return_value] */
    enum ECICallbackReturn ret = eciDataProcessed;
    if (rlen >= 8) {
        uint32_t nret;
        memcpy(&nret, rbuf + 4, 4);
        ret = (enum ECICallbackReturn)ntohl(nret);
    }
    free(rbuf);
    return ret;
}
