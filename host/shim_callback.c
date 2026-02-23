#include "shim_callback.h"
#include "eci_proto.h"
#include "rpc_io.h"
#include "rpc_msg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>

#define MAX_CB_HANDLES 64

typedef struct {
    uint32_t    handle_id;
    ECICallback callback;
    void       *user_data;
    short      *output_buffer;
    int         output_buffer_size;
    int         active;
} cb_entry_t;

static cb_entry_t g_callbacks[MAX_CB_HANDLES];
static pthread_mutex_t g_cb_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_cb_thread;
static int g_cb_fd = -1;
static volatile int g_cb_running = 0;

void shim_set_callback(uint32_t handle_id, ECICallback cb, void *pData)
{
    pthread_mutex_lock(&g_cb_lock);
    for (int i = 0; i < MAX_CB_HANDLES; i++) {
        if (g_callbacks[i].active && g_callbacks[i].handle_id == handle_id) {
            g_callbacks[i].callback = cb;
            g_callbacks[i].user_data = pData;
            pthread_mutex_unlock(&g_cb_lock);
            return;
        }
    }
    /* New entry */
    for (int i = 0; i < MAX_CB_HANDLES; i++) {
        if (!g_callbacks[i].active) {
            g_callbacks[i].handle_id = handle_id;
            g_callbacks[i].callback = cb;
            g_callbacks[i].user_data = pData;
            g_callbacks[i].active = 1;
            pthread_mutex_unlock(&g_cb_lock);
            return;
        }
    }
    pthread_mutex_unlock(&g_cb_lock);
    fprintf(stderr, "shim: callback table full\n");
}

void shim_set_output_buffer(uint32_t handle_id, int size, short *buffer)
{
    pthread_mutex_lock(&g_cb_lock);
    for (int i = 0; i < MAX_CB_HANDLES; i++) {
        if (g_callbacks[i].active && g_callbacks[i].handle_id == handle_id) {
            g_callbacks[i].output_buffer = buffer;
            g_callbacks[i].output_buffer_size = size;
            pthread_mutex_unlock(&g_cb_lock);
            return;
        }
    }
    /* Create entry even without callback yet */
    for (int i = 0; i < MAX_CB_HANDLES; i++) {
        if (!g_callbacks[i].active) {
            g_callbacks[i].handle_id = handle_id;
            g_callbacks[i].output_buffer = buffer;
            g_callbacks[i].output_buffer_size = size;
            g_callbacks[i].active = 1;
            pthread_mutex_unlock(&g_cb_lock);
            return;
        }
    }
    pthread_mutex_unlock(&g_cb_lock);
}

static void *callback_thread(void *arg)
{
    (void)arg;
    while (g_cb_running) {
        uint8_t msg_type;
        uint8_t *buf = NULL;
        uint32_t buflen;
        if (rpc_read_msg(g_cb_fd, &msg_type, &buf, &buflen) < 0)
            break;
        if (msg_type != MSG_CALLBACK_EVENT) {
            free(buf);
            continue;
        }

        /* Parse: [uint32 handle_id][int32 msg][int32 lparam][uint32 data_len][data...] */
        if (buflen < 16) { free(buf); continue; }

        uint32_t handle_id, data_len;
        int32_t eci_msg, lparam;
        uint32_t p = 0;

        memcpy(&handle_id, buf + p, 4); handle_id = ntohl(handle_id); p += 4;
        memcpy(&eci_msg, buf + p, 4); eci_msg = (int32_t)ntohl((uint32_t)eci_msg); p += 4;
        memcpy(&lparam, buf + p, 4); lparam = (int32_t)ntohl((uint32_t)lparam); p += 4;
        memcpy(&data_len, buf + p, 4); data_len = ntohl(data_len); p += 4;

        /* Find callback entry */
        cb_entry_t *entry = NULL;
        pthread_mutex_lock(&g_cb_lock);
        for (int i = 0; i < MAX_CB_HANDLES; i++) {
            if (g_callbacks[i].active && g_callbacks[i].handle_id == handle_id) {
                entry = &g_callbacks[i];
                break;
            }
        }

        enum ECICallbackReturn ret = eciDataProcessed;

        if (entry) {
            /* For waveform buffer: copy PCM data into user's buffer */
            if (eci_msg == eciWaveformBuffer && entry->output_buffer && data_len > 0) {
                uint32_t copy_bytes = data_len;
                uint32_t buf_bytes = (uint32_t)(entry->output_buffer_size * 2);
                if (copy_bytes > buf_bytes) copy_bytes = buf_bytes;
                if (p + copy_bytes <= buflen)
                    memcpy(entry->output_buffer, buf + p, copy_bytes);
            }

            if (entry->callback) {
                ECIHand fake_handle = (ECIHand)(uintptr_t)handle_id;
                ret = entry->callback(fake_handle, (enum ECIMessage)eci_msg,
                                      (long)lparam, entry->user_data);
            }
        }
        pthread_mutex_unlock(&g_cb_lock);
        free(buf);

        /* Send callback return */
        uint8_t retbuf[8];
        uint32_t nh = htonl(handle_id);
        uint32_t nr = htonl((uint32_t)ret);
        memcpy(retbuf, &nh, 4);
        memcpy(retbuf + 4, &nr, 4);
        rpc_write_msg(g_cb_fd, MSG_CALLBACK_RETURN, retbuf, 8);
    }
    return NULL;
}

int shim_callback_start(int cb_fd)
{
    if (g_cb_running) return 0;
    g_cb_fd = cb_fd;
    g_cb_running = 1;
    return pthread_create(&g_cb_thread, NULL, callback_thread, NULL);
}

void shim_callback_stop(void)
{
    if (!g_cb_running) return;
    g_cb_running = 0;
    if (g_cb_fd >= 0) {
        shutdown(g_cb_fd, SHUT_RDWR);
    }
    pthread_join(g_cb_thread, NULL);
}
