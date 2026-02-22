#ifndef SHIM_CALLBACK_H
#define SHIM_CALLBACK_H

#include "eci.h"
#include <stdint.h>

/* Register a user callback for a handle */
void shim_set_callback(uint32_t handle_id, ECICallback cb, void *pData);

/* Set the output buffer for a handle (host-side) */
void shim_set_output_buffer(uint32_t handle_id, int size, short *buffer);

/* Start/stop the callback listener thread */
int shim_callback_start(int cb_fd);
void shim_callback_stop(void);

#endif /* SHIM_CALLBACK_H */
