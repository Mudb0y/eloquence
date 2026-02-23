#ifndef BRIDGE_DISPATCH_H
#define BRIDGE_DISPATCH_H

#include "bridge_handle.h"
#include "bridge_callback.h"
#include <pthread.h>

/* Per-client state */
typedef struct client_state {
    int              rpc_fd;
    int              cb_fd;
    uint32_t         client_id;
    handle_map_t     handles;
    cb_state_t       cb_states[MAX_HANDLES_PER_TYPE]; /* one per ECIHand */
    pthread_mutex_t  cb_write_lock;
    struct client_state *next;
} client_state_t;

/* ECI function pointer table -- filled by dlsym */
typedef struct {
    void *handle; /* dlopen handle */
    void *(*eciNew)(void);
    void *(*eciNewEx)(int);
    int   (*eciGetAvailableLanguages)(int *, int *);
    void *(*eciDelete)(void *);
    int   (*eciReset)(void *);
    int   (*eciIsBeingReentered)(void *);
    void  (*eciVersion)(char *);
    int   (*eciProgStatus)(void *);
    void  (*eciErrorMessage)(void *, void *);
    void  (*eciClearErrors)(void *);
    int   (*eciTestPhrase)(void *);
    int   (*eciSpeakText)(const void *, int);
    int   (*eciSpeakTextEx)(const void *, int, int);
    int   (*eciGetParam)(void *, int);
    int   (*eciSetParam)(void *, int, int);
    int   (*eciGetDefaultParam)(int);
    int   (*eciSetDefaultParam)(int, int);
    int   (*eciCopyVoice)(void *, int, int);
    int   (*eciGetVoiceName)(void *, int, void *);
    int   (*eciSetVoiceName)(void *, int, const void *);
    int   (*eciGetVoiceParam)(void *, int, int);
    int   (*eciSetVoiceParam)(void *, int, int, int);
    int   (*eciAddText)(void *, const void *);
    int   (*eciInsertIndex)(void *, int);
    int   (*eciSynthesize)(void *);
    int   (*eciSynthesizeFile)(void *, const void *);
    int   (*eciClearInput)(void *);
    int   (*eciGeneratePhonemes)(void *, int, void *);
    int   (*eciGetIndex)(void *);
    int   (*eciStop)(void *);
    int   (*eciSpeaking)(void *);
    int   (*eciSynchronize)(void *);
    int   (*eciSetOutputBuffer)(void *, int, short *);
    int   (*eciSetOutputFilename)(void *, const void *);
    int   (*eciSetOutputDevice)(void *, int);
    int   (*eciPause)(void *, int);
    void  (*eciRegisterCallback)(void *, void *, void *);
    void *(*eciNewDict)(void *);
    void *(*eciGetDict)(void *);
    int   (*eciSetDict)(void *, void *);
    void *(*eciDeleteDict)(void *, void *);
    int   (*eciLoadDict)(void *, void *, int, const void *);
    int   (*eciSaveDict)(void *, void *, int, const void *);
    int   (*eciUpdateDict)(void *, void *, int, const void *, const void *);
    int   (*eciDictFindFirst)(void *, void *, int, const void **, const void **);
    int   (*eciDictFindNext)(void *, void *, int, const void **, const void **);
    const char *(*eciDictLookup)(void *, void *, int, const void *);
    int   (*eciUpdateDictA)(void *, void *, int, const void *, const void *, int);
    int   (*eciDictFindFirstA)(void *, void *, int, const void **, const void **, int *);
    int   (*eciDictFindNextA)(void *, void *, int, const void **, const void **, int *);
    int   (*eciDictLookupA)(void *, void *, int, const void *, const void **, int *);
    int   (*eciRegisterVoice)(void *, int, void *, void *);
    int   (*eciUnregisterVoice)(void *, int, void *, void **);
    void *(*eciNewFilter)(void *, unsigned int, int);
    void *(*eciDeleteFilter)(void *, void *);
    int   (*eciActivateFilter)(void *, void *);
    int   (*eciDeactivateFilter)(void *, void *);
    int   (*eciUpdateFilter)(void *, void *, const void *, const void *);
    int   (*eciGetFilteredText)(void *, void *, const void *, const void **);
} eci_funcs_t;

/* Load ECI functions via dlopen/dlsym. Returns 0 on success. */
int eci_load(eci_funcs_t *f, const char *libpath);

/* Dispatch one RPC request. Returns 0 on success, -1 to disconnect. */
int dispatch_rpc(client_state_t *client, eci_funcs_t *f,
                 const uint8_t *req_buf, uint32_t req_len);

/* Client list management */
extern client_state_t *g_clients;
extern pthread_mutex_t g_clients_lock;

void register_client(client_state_t *client);
void unregister_client(client_state_t *client);
client_state_t *find_client_by_id(uint32_t cid);
cb_state_t *find_cb_state_by_eci_ptr(void *eci_hand);

#endif /* BRIDGE_DISPATCH_H */
