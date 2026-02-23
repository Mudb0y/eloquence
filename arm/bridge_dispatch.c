#include "bridge_dispatch.h"
#include "eci_proto.h"
#include "rpc_io.h"
#include "rpc_msg.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

/* Global client list for callback lookups */
client_state_t *g_clients = NULL;
pthread_mutex_t g_clients_lock = PTHREAD_MUTEX_INITIALIZER;

client_state_t *find_client_by_id(uint32_t cid)
{
    client_state_t *found = NULL;
    pthread_mutex_lock(&g_clients_lock);
    for (client_state_t *c = g_clients; c; c = c->next) {
        if (c->client_id == cid) { found = c; break; }
    }
    pthread_mutex_unlock(&g_clients_lock);
    return found;
}

void register_client(client_state_t *client)
{
    pthread_mutex_lock(&g_clients_lock);
    client->next = g_clients;
    g_clients = client;
    pthread_mutex_unlock(&g_clients_lock);
}

void unregister_client(client_state_t *client)
{
    pthread_mutex_lock(&g_clients_lock);
    client_state_t **pp = &g_clients;
    while (*pp) {
        if (*pp == client) {
            *pp = client->next;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_clients_lock);
}

cb_state_t *find_cb_state_by_eci_ptr(void *eci_hand)
{
    cb_state_t *found = NULL;
    pthread_mutex_lock(&g_clients_lock);
    for (client_state_t *c = g_clients; c; c = c->next) {
        for (int i = 0; i < MAX_HANDLES_PER_TYPE; i++) {
            if (c->handles.entries[HTYPE_ECI][i].in_use &&
                c->handles.entries[HTYPE_ECI][i].ptr == eci_hand) {
                /* Find matching cb_state */
                uint32_t id = c->handles.entries[HTYPE_ECI][i].id;
                for (int j = 0; j < MAX_HANDLES_PER_TYPE; j++) {
                    if (c->cb_states[j].eci_handle_id == id) {
                        found = &c->cb_states[j];
                        goto done;
                    }
                }
            }
        }
    }
done:
    pthread_mutex_unlock(&g_clients_lock);
    return found;
}

#define DLSYM(f, name) do { \
    f->name = dlsym(f->handle, #name); \
    if (!f->name) fprintf(stderr, "bridge: warning: dlsym(%s) failed: %s\n", #name, dlerror()); \
} while(0)

int eci_load(eci_funcs_t *f, const char *libpath)
{
    memset(f, 0, sizeof(*f));
    f->handle = dlopen(libpath, RTLD_NOW);
    if (!f->handle) {
        fprintf(stderr, "bridge: dlopen(%s) failed: %s\n", libpath, dlerror());
        return -1;
    }

    DLSYM(f, eciNew);
    DLSYM(f, eciNewEx);
    DLSYM(f, eciGetAvailableLanguages);
    DLSYM(f, eciDelete);
    DLSYM(f, eciReset);
    DLSYM(f, eciIsBeingReentered);
    DLSYM(f, eciVersion);
    DLSYM(f, eciProgStatus);
    DLSYM(f, eciErrorMessage);
    DLSYM(f, eciClearErrors);
    DLSYM(f, eciTestPhrase);
    DLSYM(f, eciSpeakText);
    DLSYM(f, eciSpeakTextEx);
    DLSYM(f, eciGetParam);
    DLSYM(f, eciSetParam);
    DLSYM(f, eciGetDefaultParam);
    DLSYM(f, eciSetDefaultParam);
    DLSYM(f, eciCopyVoice);
    DLSYM(f, eciGetVoiceName);
    DLSYM(f, eciSetVoiceName);
    DLSYM(f, eciGetVoiceParam);
    DLSYM(f, eciSetVoiceParam);
    DLSYM(f, eciAddText);
    DLSYM(f, eciInsertIndex);
    DLSYM(f, eciSynthesize);
    DLSYM(f, eciSynthesizeFile);
    DLSYM(f, eciClearInput);
    DLSYM(f, eciGeneratePhonemes);
    DLSYM(f, eciGetIndex);
    DLSYM(f, eciStop);
    DLSYM(f, eciSpeaking);
    DLSYM(f, eciSynchronize);
    DLSYM(f, eciSetOutputBuffer);
    DLSYM(f, eciSetOutputFilename);
    DLSYM(f, eciSetOutputDevice);
    DLSYM(f, eciPause);
    DLSYM(f, eciRegisterCallback);
    DLSYM(f, eciNewDict);
    DLSYM(f, eciGetDict);
    DLSYM(f, eciSetDict);
    DLSYM(f, eciDeleteDict);
    DLSYM(f, eciLoadDict);
    DLSYM(f, eciSaveDict);
    DLSYM(f, eciUpdateDict);
    DLSYM(f, eciDictFindFirst);
    DLSYM(f, eciDictFindNext);
    DLSYM(f, eciDictLookup);
    DLSYM(f, eciUpdateDictA);
    DLSYM(f, eciDictFindFirstA);
    DLSYM(f, eciDictFindNextA);
    DLSYM(f, eciDictLookupA);
    DLSYM(f, eciNewFilter);
    DLSYM(f, eciDeleteFilter);
    DLSYM(f, eciActivateFilter);
    DLSYM(f, eciDeactivateFilter);
    DLSYM(f, eciUpdateFilter);
    DLSYM(f, eciGetFilteredText);

    if (!f->eciNew) {
        fprintf(stderr, "bridge: critical: eciNew not found\n");
        dlclose(f->handle);
        return -1;
    }
    return 0;
}

/* Helper: allocate a cb_state slot for an ECI handle */
static cb_state_t *alloc_cb_state(client_state_t *client, uint32_t handle_id)
{
    for (int i = 0; i < MAX_HANDLES_PER_TYPE; i++) {
        if (client->cb_states[i].eci_handle_id == 0) {
            cb_state_t *cbs = &client->cb_states[i];
            memset(cbs, 0, sizeof(*cbs));
            cbs->eci_handle_id = handle_id;
            cbs->cb_fd = client->cb_fd;
            cbs->cb_write_lock = &client->cb_write_lock;
            return cbs;
        }
    }
    return NULL;
}

static void free_cb_state(client_state_t *client, uint32_t handle_id)
{
    for (int i = 0; i < MAX_HANDLES_PER_TYPE; i++) {
        if (client->cb_states[i].eci_handle_id == handle_id) {
            free(client->cb_states[i].output_buffer);
            memset(&client->cb_states[i], 0, sizeof(cb_state_t));
            return;
        }
    }
}

static cb_state_t *get_cb_state(client_state_t *client, uint32_t handle_id)
{
    for (int i = 0; i < MAX_HANDLES_PER_TYPE; i++) {
        if (client->cb_states[i].eci_handle_id == handle_id)
            return &client->cb_states[i];
    }
    return NULL;
}

/* Helper to make a NUL-terminated copy of a string that may not be terminated */
static char *make_cstr(const char *s, uint32_t len)
{
    if (!s) return NULL;
    char *c = (char *)malloc(len + 1);
    if (!c) return NULL;
    memcpy(c, s, len);
    c[len] = '\0';
    return c;
}

/* Send an RPC response */
static int send_response(int fd, uint32_t seq, int32_t retval, uint16_t error,
                         const uint8_t *extra, uint32_t extra_len)
{
    rpc_buf_t resp;
    rpc_buf_init(&resp);
    rpc_encode_response_header(&resp, seq, retval, error);
    if (extra && extra_len > 0)
        rpc_encode_raw(&resp, extra, extra_len);
    int rc = rpc_write_msg(fd, MSG_RPC_RESPONSE, resp.data, resp.len);
    rpc_buf_free(&resp);
    return rc;
}

int dispatch_rpc(client_state_t *client, eci_funcs_t *f,
                 const uint8_t *buf, uint32_t buflen)
{
    uint32_t pos = 0;
    uint32_t seq;
    uint16_t func_id;
    if (rpc_decode_request_header(buf, buflen, &pos, &seq, &func_id) < 0)
        return -1;

    switch (func_id) {

    case FN_ECI_NEW: {
        void *h = f->eciNew();
        uint32_t hid = hmap_add(&client->handles, HTYPE_ECI, h);
        if (hid == 0) {
            if (h) f->eciDelete(h);
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        }
        /* Register internal callback for this handle */
        alloc_cb_state(client, hid);
        if (f->eciRegisterCallback)
            f->eciRegisterCallback(h, (void *)bridge_internal_callback, NULL);
        /* Return handle ID */
        rpc_buf_t extra;
        rpc_buf_init(&extra);
        rpc_encode_handle(&extra, hid);
        int rc = send_response(client->rpc_fd, seq, (int32_t)hid, 0, extra.data, extra.len);
        rpc_buf_free(&extra);
        return rc;
    }

    case FN_ECI_NEW_EX: {
        int32_t lang;
        if (rpc_decode_int32(buf, buflen, &pos, &lang) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = f->eciNewEx ? f->eciNewEx(lang) : NULL;
        uint32_t hid = hmap_add(&client->handles, HTYPE_ECI, h);
        if (hid == 0) {
            if (h) f->eciDelete(h);
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        }
        alloc_cb_state(client, hid);
        if (f->eciRegisterCallback)
            f->eciRegisterCallback(h, (void *)bridge_internal_callback, NULL);
        rpc_buf_t extra;
        rpc_buf_init(&extra);
        rpc_encode_handle(&extra, hid);
        int rc = send_response(client->rpc_fd, seq, (int32_t)hid, 0, extra.data, extra.len);
        rpc_buf_free(&extra);
        return rc;
    }

    case FN_ECI_DELETE: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        if (h) {
            f->eciDelete(h);
            hmap_remove(&client->handles, HTYPE_ECI, hid);
            free_cb_state(client, hid);
        }
        return send_response(client->rpc_fd, seq, 0, 0, NULL, 0);
    }

    case FN_ECI_RESET: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        int ret = h && f->eciReset ? f->eciReset(h) : 0;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_VERSION: {
        char verbuf[256];
        memset(verbuf, 0, sizeof(verbuf));
        if (f->eciVersion) f->eciVersion(verbuf);
        rpc_buf_t extra;
        rpc_buf_init(&extra);
        rpc_encode_string(&extra, verbuf);
        int rc = send_response(client->rpc_fd, seq, 0, 0, extra.data, extra.len);
        rpc_buf_free(&extra);
        return rc;
    }

    case FN_ECI_PROG_STATUS: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        int ret = h && f->eciProgStatus ? f->eciProgStatus(h) : -1;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_ERROR_MESSAGE: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        char errbuf[512];
        memset(errbuf, 0, sizeof(errbuf));
        if (h && f->eciErrorMessage) f->eciErrorMessage(h, errbuf);
        rpc_buf_t extra;
        rpc_buf_init(&extra);
        rpc_encode_string(&extra, errbuf);
        int rc = send_response(client->rpc_fd, seq, 0, 0, extra.data, extra.len);
        rpc_buf_free(&extra);
        return rc;
    }

    case FN_ECI_CLEAR_ERRORS: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        if (h && f->eciClearErrors) f->eciClearErrors(h);
        return send_response(client->rpc_fd, seq, 0, 0, NULL, 0);
    }

    case FN_ECI_IS_BEING_REENTERED: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        int ret = h && f->eciIsBeingReentered ? f->eciIsBeingReentered(h) : 0;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_TEST_PHRASE: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        int ret = h && f->eciTestPhrase ? f->eciTestPhrase(h) : 0;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_GET_AVAILABLE_LANGS: {
        int langs[64];
        int nlangs = 64;
        int ret = f->eciGetAvailableLanguages ? f->eciGetAvailableLanguages(langs, &nlangs) : -1;
        rpc_buf_t extra;
        rpc_buf_init(&extra);
        rpc_encode_int32(&extra, nlangs);
        for (int i = 0; i < nlangs && i < 64; i++)
            rpc_encode_int32(&extra, langs[i]);
        int rc = send_response(client->rpc_fd, seq, ret, 0, extra.data, extra.len);
        rpc_buf_free(&extra);
        return rc;
    }

    /* Synthesis functions */
    case FN_ECI_ADD_TEXT: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        const char *text; uint32_t tlen;
        if (rpc_decode_string(buf, buflen, &pos, &text, &tlen) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        char *ctext = make_cstr(text, tlen);
        int ret = h && f->eciAddText ? f->eciAddText(h, ctext) : 0;
        free(ctext);
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_INSERT_INDEX: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t idx;
        if (rpc_decode_int32(buf, buflen, &pos, &idx) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        int ret = h && f->eciInsertIndex ? f->eciInsertIndex(h, idx) : 0;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_SYNTHESIZE: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        int ret = h && f->eciSynthesize ? f->eciSynthesize(h) : 0;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_SYNTHESIZE_FILE: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        const char *fname; uint32_t flen;
        if (rpc_decode_string(buf, buflen, &pos, &fname, &flen) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        char *cfname = make_cstr(fname, flen);
        int ret = h && f->eciSynthesizeFile ? f->eciSynthesizeFile(h, cfname) : 0;
        free(cfname);
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_CLEAR_INPUT: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        int ret = h && f->eciClearInput ? f->eciClearInput(h) : 0;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_GENERATE_PHONEMES: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t size;
        if (rpc_decode_int32(buf, buflen, &pos, &size) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        if (size <= 0 || size > 65536) size = 4096;
        char *pbuf = (char *)calloc(1, (size_t)size);
        int ret = h && f->eciGeneratePhonemes ? f->eciGeneratePhonemes(h, size, pbuf) : 0;
        rpc_buf_t extra;
        rpc_buf_init(&extra);
        rpc_encode_string(&extra, pbuf);
        int rc = send_response(client->rpc_fd, seq, ret, 0, extra.data, extra.len);
        rpc_buf_free(&extra);
        free(pbuf);
        return rc;
    }

    case FN_ECI_GET_INDEX: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        int ret = h && f->eciGetIndex ? f->eciGetIndex(h) : -1;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_STOP: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        int ret = h && f->eciStop ? f->eciStop(h) : 0;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_SPEAKING: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        int ret = h && f->eciSpeaking ? f->eciSpeaking(h) : 0;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_SYNCHRONIZE: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        int ret = h && f->eciSynchronize ? f->eciSynchronize(h) : 0;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_SPEAK_TEXT: {
        const char *text; uint32_t tlen;
        if (rpc_decode_string(buf, buflen, &pos, &text, &tlen) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t annot;
        if (rpc_decode_int32(buf, buflen, &pos, &annot) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        char *ctext = make_cstr(text, tlen);
        int ret = f->eciSpeakText ? f->eciSpeakText(ctext, annot) : 0;
        free(ctext);
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_SPEAK_TEXT_EX: {
        const char *text; uint32_t tlen;
        if (rpc_decode_string(buf, buflen, &pos, &text, &tlen) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t annot;
        if (rpc_decode_int32(buf, buflen, &pos, &annot) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t lang;
        if (rpc_decode_int32(buf, buflen, &pos, &lang) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        char *ctext = make_cstr(text, tlen);
        int ret = f->eciSpeakTextEx ? f->eciSpeakTextEx(ctext, annot, lang) : 0;
        free(ctext);
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    /* Params */
    case FN_ECI_GET_PARAM: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t param;
        if (rpc_decode_int32(buf, buflen, &pos, &param) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        int ret = h && f->eciGetParam ? f->eciGetParam(h, param) : -1;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_SET_PARAM: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t param, val;
        if (rpc_decode_int32(buf, buflen, &pos, &param) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_int32(buf, buflen, &pos, &val) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        int ret = h && f->eciSetParam ? f->eciSetParam(h, param, val) : -1;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_GET_DEFAULT_PARAM: {
        int32_t param;
        if (rpc_decode_int32(buf, buflen, &pos, &param) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int ret = f->eciGetDefaultParam ? f->eciGetDefaultParam(param) : -1;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_SET_DEFAULT_PARAM: {
        int32_t param, val;
        if (rpc_decode_int32(buf, buflen, &pos, &param) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_int32(buf, buflen, &pos, &val) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int ret = f->eciSetDefaultParam ? f->eciSetDefaultParam(param, val) : -1;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    /* Audio output */
    case FN_ECI_SET_OUTPUT_BUFFER: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t size;
        if (rpc_decode_int32(buf, buflen, &pos, &size) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        if (!h)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        /* Allocate ARM-side buffer */
        cb_state_t *cbs = get_cb_state(client, hid);
        if (cbs) {
            free(cbs->output_buffer);
            cbs->output_buffer = (short *)calloc((size_t)size, sizeof(short));
            cbs->output_buffer_size = size;
        }
        int ret = f->eciSetOutputBuffer ? f->eciSetOutputBuffer(h, size,
                    cbs ? cbs->output_buffer : NULL) : 0;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_SET_OUTPUT_FILENAME: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        const char *fname; uint32_t flen;
        if (rpc_decode_string(buf, buflen, &pos, &fname, &flen) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        char *cfname = make_cstr(fname, flen);
        int ret = h && f->eciSetOutputFilename ? f->eciSetOutputFilename(h, cfname) : 0;
        free(cfname);
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_SET_OUTPUT_DEVICE: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t dev;
        if (rpc_decode_int32(buf, buflen, &pos, &dev) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        int ret = h && f->eciSetOutputDevice ? f->eciSetOutputDevice(h, dev) : 0;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_PAUSE: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t on;
        if (rpc_decode_int32(buf, buflen, &pos, &on) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        int ret = h && f->eciPause ? f->eciPause(h, on) : 0;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_REGISTER_CALLBACK: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        /* Client registers callback -- we just mark cb_state as active.
         * Internal callback is already registered at eciNew time. */
        cb_state_t *cbs = get_cb_state(client, hid);
        if (cbs) cbs->has_callback = 1;
        return send_response(client->rpc_fd, seq, 0, 0, NULL, 0);
    }

    /* Voice */
    case FN_ECI_COPY_VOICE: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t from, to;
        if (rpc_decode_int32(buf, buflen, &pos, &from) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_int32(buf, buflen, &pos, &to) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        int ret = h && f->eciCopyVoice ? f->eciCopyVoice(h, from, to) : 0;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_GET_VOICE_NAME: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t voice;
        if (rpc_decode_int32(buf, buflen, &pos, &voice) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        char namebuf[ECI_VOICE_NAME_LENGTH + 1];
        memset(namebuf, 0, sizeof(namebuf));
        int ret = h && f->eciGetVoiceName ? f->eciGetVoiceName(h, voice, namebuf) : 0;
        rpc_buf_t extra;
        rpc_buf_init(&extra);
        rpc_encode_string(&extra, namebuf);
        int rc = send_response(client->rpc_fd, seq, ret, 0, extra.data, extra.len);
        rpc_buf_free(&extra);
        return rc;
    }

    case FN_ECI_SET_VOICE_NAME: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t voice;
        if (rpc_decode_int32(buf, buflen, &pos, &voice) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        const char *name; uint32_t nlen;
        if (rpc_decode_string(buf, buflen, &pos, &name, &nlen) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        char *cname = make_cstr(name, nlen);
        int ret = h && f->eciSetVoiceName ? f->eciSetVoiceName(h, voice, cname) : 0;
        free(cname);
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_GET_VOICE_PARAM: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t voice, param;
        if (rpc_decode_int32(buf, buflen, &pos, &voice) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_int32(buf, buflen, &pos, &param) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        int ret = h && f->eciGetVoiceParam ? f->eciGetVoiceParam(h, voice, param) : -1;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_SET_VOICE_PARAM: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t voice, param, val;
        if (rpc_decode_int32(buf, buflen, &pos, &voice) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_int32(buf, buflen, &pos, &param) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_int32(buf, buflen, &pos, &val) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        int ret = h && f->eciSetVoiceParam ? f->eciSetVoiceParam(h, voice, param, val) : -1;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    /* Dict */
    case FN_ECI_NEW_DICT: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        void *dh = h && f->eciNewDict ? f->eciNewDict(h) : NULL;
        uint32_t dhid = dh ? hmap_add(&client->handles, HTYPE_DICT, dh) : 0;
        rpc_buf_t extra;
        rpc_buf_init(&extra);
        rpc_encode_handle(&extra, dhid);
        int rc = send_response(client->rpc_fd, seq, (int32_t)dhid, 0, extra.data, extra.len);
        rpc_buf_free(&extra);
        return rc;
    }

    case FN_ECI_GET_DICT: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        void *dh = h && f->eciGetDict ? f->eciGetDict(h) : NULL;
        /* Find or add dict handle */
        uint32_t dhid = dh ? hmap_find_by_ptr(&client->handles, HTYPE_DICT, dh) : 0;
        if (dh && dhid == 0) dhid = hmap_add(&client->handles, HTYPE_DICT, dh);
        rpc_buf_t extra;
        rpc_buf_init(&extra);
        rpc_encode_handle(&extra, dhid);
        int rc = send_response(client->rpc_fd, seq, (int32_t)dhid, 0, extra.data, extra.len);
        rpc_buf_free(&extra);
        return rc;
    }

    case FN_ECI_SET_DICT: {
        uint32_t hid, dhid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_handle(buf, buflen, &pos, &dhid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        void *dh = hmap_get(&client->handles, HTYPE_DICT, dhid);
        int ret = h && f->eciSetDict ? f->eciSetDict(h, dh) : -1;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_DELETE_DICT: {
        uint32_t hid, dhid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_handle(buf, buflen, &pos, &dhid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        void *dh = hmap_get(&client->handles, HTYPE_DICT, dhid);
        if (h && dh && f->eciDeleteDict) {
            f->eciDeleteDict(h, dh);
            hmap_remove(&client->handles, HTYPE_DICT, dhid);
        }
        return send_response(client->rpc_fd, seq, 0, 0, NULL, 0);
    }

    case FN_ECI_LOAD_DICT: {
        uint32_t hid, dhid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_handle(buf, buflen, &pos, &dhid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t vol;
        if (rpc_decode_int32(buf, buflen, &pos, &vol) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        const char *fname; uint32_t flen;
        if (rpc_decode_string(buf, buflen, &pos, &fname, &flen) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        void *dh = hmap_get(&client->handles, HTYPE_DICT, dhid);
        char *cfname = make_cstr(fname, flen);
        int ret = h && dh && f->eciLoadDict ? f->eciLoadDict(h, dh, vol, cfname) : -1;
        free(cfname);
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_SAVE_DICT: {
        uint32_t hid, dhid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_handle(buf, buflen, &pos, &dhid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t vol;
        if (rpc_decode_int32(buf, buflen, &pos, &vol) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        const char *fname; uint32_t flen;
        if (rpc_decode_string(buf, buflen, &pos, &fname, &flen) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        void *dh = hmap_get(&client->handles, HTYPE_DICT, dhid);
        char *cfname = make_cstr(fname, flen);
        int ret = h && dh && f->eciSaveDict ? f->eciSaveDict(h, dh, vol, cfname) : -1;
        free(cfname);
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_UPDATE_DICT: {
        uint32_t hid, dhid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_handle(buf, buflen, &pos, &dhid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t vol;
        if (rpc_decode_int32(buf, buflen, &pos, &vol) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        const char *key; uint32_t klen;
        if (rpc_decode_string(buf, buflen, &pos, &key, &klen) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        const char *val; uint32_t vlen;
        if (rpc_decode_string(buf, buflen, &pos, &val, &vlen) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        void *dh = hmap_get(&client->handles, HTYPE_DICT, dhid);
        char *ckey = make_cstr(key, klen);
        char *cval = make_cstr(val, vlen);
        int ret = h && dh && f->eciUpdateDict ? f->eciUpdateDict(h, dh, vol, ckey, cval) : -1;
        free(ckey); free(cval);
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_DICT_LOOKUP: {
        uint32_t hid, dhid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_handle(buf, buflen, &pos, &dhid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t vol;
        if (rpc_decode_int32(buf, buflen, &pos, &vol) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        const char *key; uint32_t klen;
        if (rpc_decode_string(buf, buflen, &pos, &key, &klen) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        void *dh = hmap_get(&client->handles, HTYPE_DICT, dhid);
        char *ckey = make_cstr(key, klen);
        const char *result = h && dh && f->eciDictLookup ? f->eciDictLookup(h, dh, vol, ckey) : NULL;
        free(ckey);
        rpc_buf_t extra;
        rpc_buf_init(&extra);
        rpc_encode_string(&extra, result);
        int rc = send_response(client->rpc_fd, seq, result ? 0 : -1, 0, extra.data, extra.len);
        rpc_buf_free(&extra);
        return rc;
    }

    case FN_ECI_DICT_FIND_FIRST: {
        uint32_t hid, dhid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_handle(buf, buflen, &pos, &dhid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t vol;
        if (rpc_decode_int32(buf, buflen, &pos, &vol) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        void *dh = hmap_get(&client->handles, HTYPE_DICT, dhid);
        const void *pkey = NULL, *pval = NULL;
        int ret = h && dh && f->eciDictFindFirst ?
            f->eciDictFindFirst(h, dh, vol, &pkey, &pval) : -1;
        rpc_buf_t extra;
        rpc_buf_init(&extra);
        rpc_encode_string(&extra, pkey ? (const char *)pkey : "");
        rpc_encode_string(&extra, pval ? (const char *)pval : "");
        int rc = send_response(client->rpc_fd, seq, ret, 0, extra.data, extra.len);
        rpc_buf_free(&extra);
        return rc;
    }

    case FN_ECI_DICT_FIND_NEXT: {
        uint32_t hid, dhid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_handle(buf, buflen, &pos, &dhid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t vol;
        if (rpc_decode_int32(buf, buflen, &pos, &vol) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        void *dh = hmap_get(&client->handles, HTYPE_DICT, dhid);
        const void *pkey = NULL, *pval = NULL;
        int ret = h && dh && f->eciDictFindNext ?
            f->eciDictFindNext(h, dh, vol, &pkey, &pval) : -1;
        rpc_buf_t extra;
        rpc_buf_init(&extra);
        rpc_encode_string(&extra, pkey ? (const char *)pkey : "");
        rpc_encode_string(&extra, pval ? (const char *)pval : "");
        int rc = send_response(client->rpc_fd, seq, ret, 0, extra.data, extra.len);
        rpc_buf_free(&extra);
        return rc;
    }

    case FN_ECI_UPDATE_DICT_A: {
        uint32_t hid, dhid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_handle(buf, buflen, &pos, &dhid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t vol;
        if (rpc_decode_int32(buf, buflen, &pos, &vol) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        const char *key; uint32_t klen;
        if (rpc_decode_string(buf, buflen, &pos, &key, &klen) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        const char *val; uint32_t vlen;
        if (rpc_decode_string(buf, buflen, &pos, &val, &vlen) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t partOfSpeech;
        if (rpc_decode_int32(buf, buflen, &pos, &partOfSpeech) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        void *dh = hmap_get(&client->handles, HTYPE_DICT, dhid);
        char *ckey = make_cstr(key, klen);
        char *cval = make_cstr(val, vlen);
        int ret = h && dh && f->eciUpdateDictA ? f->eciUpdateDictA(h, dh, vol, ckey, cval, partOfSpeech) : -1;
        free(ckey); free(cval);
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_DICT_FIND_FIRST_A: {
        uint32_t hid, dhid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_handle(buf, buflen, &pos, &dhid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t vol;
        if (rpc_decode_int32(buf, buflen, &pos, &vol) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        void *dh = hmap_get(&client->handles, HTYPE_DICT, dhid);
        const void *pkey = NULL, *pval = NULL;
        int pos_val = 0;
        int ret = h && dh && f->eciDictFindFirstA ?
            f->eciDictFindFirstA(h, dh, vol, &pkey, &pval, &pos_val) : -1;
        rpc_buf_t extra;
        rpc_buf_init(&extra);
        rpc_encode_string(&extra, pkey ? (const char *)pkey : "");
        rpc_encode_string(&extra, pval ? (const char *)pval : "");
        rpc_encode_int32(&extra, pos_val);
        int rc = send_response(client->rpc_fd, seq, ret, 0, extra.data, extra.len);
        rpc_buf_free(&extra);
        return rc;
    }

    case FN_ECI_DICT_FIND_NEXT_A: {
        uint32_t hid, dhid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_handle(buf, buflen, &pos, &dhid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t vol;
        if (rpc_decode_int32(buf, buflen, &pos, &vol) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        void *dh = hmap_get(&client->handles, HTYPE_DICT, dhid);
        const void *pkey = NULL, *pval = NULL;
        int pos_val = 0;
        int ret = h && dh && f->eciDictFindNextA ?
            f->eciDictFindNextA(h, dh, vol, &pkey, &pval, &pos_val) : -1;
        rpc_buf_t extra;
        rpc_buf_init(&extra);
        rpc_encode_string(&extra, pkey ? (const char *)pkey : "");
        rpc_encode_string(&extra, pval ? (const char *)pval : "");
        rpc_encode_int32(&extra, pos_val);
        int rc = send_response(client->rpc_fd, seq, ret, 0, extra.data, extra.len);
        rpc_buf_free(&extra);
        return rc;
    }

    case FN_ECI_DICT_LOOKUP_A: {
        uint32_t hid, dhid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_handle(buf, buflen, &pos, &dhid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t vol;
        if (rpc_decode_int32(buf, buflen, &pos, &vol) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        const char *key; uint32_t klen;
        if (rpc_decode_string(buf, buflen, &pos, &key, &klen) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        void *dh = hmap_get(&client->handles, HTYPE_DICT, dhid);
        char *ckey = make_cstr(key, klen);
        const void *ptrans = NULL;
        int pos_val = 0;
        int ret = h && dh && f->eciDictLookupA ? f->eciDictLookupA(h, dh, vol, ckey, &ptrans, &pos_val) : -1;
        free(ckey);
        rpc_buf_t extra;
        rpc_buf_init(&extra);
        rpc_encode_string(&extra, ptrans ? (const char *)ptrans : "");
        rpc_encode_int32(&extra, pos_val);
        int rc = send_response(client->rpc_fd, seq, ret, 0, extra.data, extra.len);
        rpc_buf_free(&extra);
        return rc;
    }

    /* Filter */
    case FN_ECI_NEW_FILTER: {
        uint32_t hid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        uint32_t filterNum;
        if (rpc_decode_uint32(buf, buflen, &pos, &filterNum) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        int32_t bGlobal;
        if (rpc_decode_int32(buf, buflen, &pos, &bGlobal) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        void *fh = h && f->eciNewFilter ? f->eciNewFilter(h, filterNum, bGlobal) : NULL;
        uint32_t fhid = fh ? hmap_add(&client->handles, HTYPE_FILTER, fh) : 0;
        rpc_buf_t extra;
        rpc_buf_init(&extra);
        rpc_encode_handle(&extra, fhid);
        int rc = send_response(client->rpc_fd, seq, (int32_t)fhid, 0, extra.data, extra.len);
        rpc_buf_free(&extra);
        return rc;
    }

    case FN_ECI_DELETE_FILTER: {
        uint32_t hid, fhid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_handle(buf, buflen, &pos, &fhid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        void *fh = hmap_get(&client->handles, HTYPE_FILTER, fhid);
        if (h && fh && f->eciDeleteFilter) {
            f->eciDeleteFilter(h, fh);
            hmap_remove(&client->handles, HTYPE_FILTER, fhid);
        }
        return send_response(client->rpc_fd, seq, 0, 0, NULL, 0);
    }

    case FN_ECI_ACTIVATE_FILTER: {
        uint32_t hid, fhid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_handle(buf, buflen, &pos, &fhid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        void *fh = hmap_get(&client->handles, HTYPE_FILTER, fhid);
        int ret = h && fh && f->eciActivateFilter ? f->eciActivateFilter(h, fh) : -1;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_DEACTIVATE_FILTER: {
        uint32_t hid, fhid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_handle(buf, buflen, &pos, &fhid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        void *fh = hmap_get(&client->handles, HTYPE_FILTER, fhid);
        int ret = h && fh && f->eciDeactivateFilter ? f->eciDeactivateFilter(h, fh) : -1;
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_UPDATE_FILTER: {
        uint32_t hid, fhid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_handle(buf, buflen, &pos, &fhid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        const char *key; uint32_t klen;
        if (rpc_decode_string(buf, buflen, &pos, &key, &klen) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        const char *trans; uint32_t tlen;
        if (rpc_decode_string(buf, buflen, &pos, &trans, &tlen) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        void *fh = hmap_get(&client->handles, HTYPE_FILTER, fhid);
        char *ckey = make_cstr(key, klen);
        char *ctrans = make_cstr(trans, tlen);
        int ret = h && fh && f->eciUpdateFilter ? f->eciUpdateFilter(h, fh, ckey, ctrans) : -1;
        free(ckey); free(ctrans);
        return send_response(client->rpc_fd, seq, ret, 0, NULL, 0);
    }

    case FN_ECI_GET_FILTERED_TEXT: {
        uint32_t hid, fhid;
        if (rpc_decode_handle(buf, buflen, &pos, &hid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        if (rpc_decode_handle(buf, buflen, &pos, &fhid) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        const char *input; uint32_t ilen;
        if (rpc_decode_string(buf, buflen, &pos, &input, &ilen) < 0)
            return send_response(client->rpc_fd, seq, 0, 1, NULL, 0);
        void *h = hmap_get(&client->handles, HTYPE_ECI, hid);
        void *fh = hmap_get(&client->handles, HTYPE_FILTER, fhid);
        char *cinput = make_cstr(input, ilen);
        const void *filtered = NULL;
        int ret = h && fh && f->eciGetFilteredText ? f->eciGetFilteredText(h, fh, cinput, &filtered) : -1;
        free(cinput);
        rpc_buf_t extra;
        rpc_buf_init(&extra);
        rpc_encode_string(&extra, filtered ? (const char *)filtered : "");
        int rc = send_response(client->rpc_fd, seq, ret, 0, extra.data, extra.len);
        rpc_buf_free(&extra);
        return rc;
    }

    /* Voice registration (stub -- rarely used) */
    case FN_ECI_REGISTER_VOICE:
    case FN_ECI_UNREGISTER_VOICE:
        return send_response(client->rpc_fd, seq, -1, 1, NULL, 0);

    default:
        fprintf(stderr, "bridge: unknown func_id 0x%04x\n", func_id);
        return send_response(client->rpc_fd, seq, -1, 1, NULL, 0);
    }
}
