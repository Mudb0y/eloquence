#ifndef BRIDGE_CALLBACK_H
#define BRIDGE_CALLBACK_H

#include <stdint.h>
#include <pthread.h>

/* Per-handle callback state */
typedef struct {
    uint32_t eci_handle_id;       /* client-facing handle ID */
    int      cb_fd;               /* callback channel fd for this client */
    int      has_callback;        /* client registered a callback? */
    short   *output_buffer;       /* ARM-side output buffer for waveform */
    int      output_buffer_size;  /* in samples */
    pthread_mutex_t *cb_write_lock; /* serialize writes on cb channel */
} cb_state_t;

/* Internal ECI callback -- registered on every eciNew.
 * Serializes event, sends on callback channel, waits for return. */
#include "eci.h"
enum ECICallbackReturn bridge_internal_callback(ECIHand hEngine,
                                                enum ECIMessage msg,
                                                long lParam,
                                                void *pData);

#endif /* BRIDGE_CALLBACK_H */
