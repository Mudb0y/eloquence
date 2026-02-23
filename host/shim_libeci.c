/*
 * Host-native shim libeci.so -- exports all ECI API symbols.
 * Translates calls to RPC over Unix socket to the ARM bridge daemon.
 */
#define _GNU_SOURCE
#include "eci.h"
#include "eci_proto.h"
#include "rpc_msg.h"
#include "rpc_io.h"
#include "shim_connection.h"
#include "shim_callback.h"
#include "shim_launch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>

/* Handle encoding: ECIHand on host is (void*)(uintptr_t)handle_id */
static uint32_t hand_to_id(ECIHand h) { return (uint32_t)(uintptr_t)h; }
static ECIHand id_to_hand(uint32_t id) { return (ECIHand)(uintptr_t)id; }

/* Convenience: do an RPC with encoded args, return retval */
static int do_rpc(uint16_t func_id, rpc_buf_t *args,
                  int32_t *retval, uint8_t **extra, uint32_t *extra_len)
{
    shim_conn_t *conn = shim_get_conn();
    if (!conn || !conn->connected) return -1;
    int32_t rv = 0;
    uint8_t *ebuf = NULL;
    uint32_t elen = 0;
    int rc = shim_rpc(conn, func_id,
                      args ? args->data : NULL,
                      args ? args->len : 0,
                      &rv, &ebuf, &elen);
    if (retval) *retval = rv;
    if (extra) *extra = ebuf; else free(ebuf);
    if (extra_len) *extra_len = elen;
    return rc;
}

/* --- Lifecycle --- */

ECIHand eciNew(void)
{
    int32_t rv;
    uint8_t *extra = NULL;
    uint32_t elen;
    if (do_rpc(FN_ECI_NEW, NULL, &rv, &extra, &elen) < 0)
        return NULL_ECI_HAND;

    /* Parse handle from extra data */
    uint32_t hid = 0;
    if (extra && elen >= 5) {
        uint32_t pos = 0;
        rpc_decode_handle(extra, elen, &pos, &hid);
    }
    free(extra);

    if (hid == 0) return NULL_ECI_HAND;

    /* Start callback thread if not already running */
    shim_conn_t *conn = shim_get_conn();
    if (conn && conn->cb_fd >= 0)
        shim_callback_start(conn->cb_fd);

    return id_to_hand(hid);
}

ECIHand eciNewEx(enum ECILanguageDialect Value)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_int32(&args, (int32_t)Value);

    int32_t rv;
    uint8_t *extra = NULL;
    uint32_t elen;
    int rc = do_rpc(FN_ECI_NEW_EX, &args, &rv, &extra, &elen);
    rpc_buf_free(&args);
    if (rc < 0) return NULL_ECI_HAND;

    uint32_t hid = 0;
    if (extra && elen >= 5) {
        uint32_t pos = 0;
        rpc_decode_handle(extra, elen, &pos, &hid);
    }
    free(extra);

    if (hid == 0) return NULL_ECI_HAND;

    shim_conn_t *conn = shim_get_conn();
    if (conn && conn->cb_fd >= 0)
        shim_callback_start(conn->cb_fd);

    return id_to_hand(hid);
}

int eciGetAvailableLanguages(enum ECILanguageDialect *aLanguages, int *nLanguages)
{
    int32_t rv;
    uint8_t *extra = NULL;
    uint32_t elen;
    if (do_rpc(FN_ECI_GET_AVAILABLE_LANGS, NULL, &rv, &extra, &elen) < 0)
        return -1;

    uint32_t pos = 0;
    int32_t count;
    if (rpc_decode_int32(extra, elen, &pos, &count) < 0) {
        free(extra);
        return -1;
    }
    if (nLanguages) {
        int max = *nLanguages;
        if (count < max) max = count;
        for (int i = 0; i < max; i++) {
            int32_t lang;
            if (rpc_decode_int32(extra, elen, &pos, &lang) < 0) break;
            if (aLanguages) aLanguages[i] = (enum ECILanguageDialect)lang;
        }
        *nLanguages = count;
    }
    free(extra);
    return (int)rv;
}

ECIHand eciDelete(ECIHand hEngine)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    do_rpc(FN_ECI_DELETE, &args, NULL, NULL, NULL);
    rpc_buf_free(&args);
    return NULL_ECI_HAND;
}

Boolean eciReset(ECIHand hEngine)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    int32_t rv;
    do_rpc(FN_ECI_RESET, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (Boolean)rv;
}

Boolean eciIsBeingReentered(ECIHand hEngine)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    int32_t rv;
    do_rpc(FN_ECI_IS_BEING_REENTERED, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (Boolean)rv;
}

void eciVersion(char *pBuffer)
{
    if (!pBuffer) return;
    int32_t rv;
    uint8_t *extra = NULL;
    uint32_t elen;
    do_rpc(FN_ECI_VERSION, NULL, &rv, &extra, &elen);
    if (extra && elen > 0) {
        uint32_t pos = 0;
        const char *str; uint32_t slen;
        if (rpc_decode_string(extra, elen, &pos, &str, &slen) == 0 && str) {
            memcpy(pBuffer, str, slen);
            pBuffer[slen] = '\0';
        }
    }
    free(extra);
}

int eciProgStatus(ECIHand hEngine)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    int32_t rv;
    do_rpc(FN_ECI_PROG_STATUS, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (int)rv;
}

void eciErrorMessage(ECIHand hEngine, void *buffer)
{
    if (!buffer) return;
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    int32_t rv;
    uint8_t *extra = NULL;
    uint32_t elen;
    do_rpc(FN_ECI_ERROR_MESSAGE, &args, &rv, &extra, &elen);
    rpc_buf_free(&args);
    if (extra && elen > 0) {
        uint32_t pos = 0;
        const char *str; uint32_t slen;
        if (rpc_decode_string(extra, elen, &pos, &str, &slen) == 0 && str) {
            memcpy(buffer, str, slen);
            ((char *)buffer)[slen] = '\0';
        }
    }
    free(extra);
}

void eciClearErrors(ECIHand hEngine)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    do_rpc(FN_ECI_CLEAR_ERRORS, &args, NULL, NULL, NULL);
    rpc_buf_free(&args);
}

Boolean eciTestPhrase(ECIHand hEngine)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    int32_t rv;
    do_rpc(FN_ECI_TEST_PHRASE, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (Boolean)rv;
}

/* --- Synthesis --- */

Boolean eciAddText(ECIHand hEngine, ECIInputText pText)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_string(&args, (const char *)pText);
    int32_t rv;
    do_rpc(FN_ECI_ADD_TEXT, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (Boolean)rv;
}

Boolean eciInsertIndex(ECIHand hEngine, int iIndex)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_int32(&args, iIndex);
    int32_t rv;
    do_rpc(FN_ECI_INSERT_INDEX, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (Boolean)rv;
}

Boolean eciSynthesize(ECIHand hEngine)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    int32_t rv;
    do_rpc(FN_ECI_SYNTHESIZE, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (Boolean)rv;
}

Boolean eciSynthesizeFile(ECIHand hEngine, const void *pFilename)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_string(&args, (const char *)pFilename);
    int32_t rv;
    do_rpc(FN_ECI_SYNTHESIZE_FILE, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (Boolean)rv;
}

Boolean eciClearInput(ECIHand hEngine)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    int32_t rv;
    do_rpc(FN_ECI_CLEAR_INPUT, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (Boolean)rv;
}

Boolean eciGeneratePhonemes(ECIHand hEngine, int iSize, void *pBuffer)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_int32(&args, iSize);
    int32_t rv;
    uint8_t *extra = NULL;
    uint32_t elen;
    do_rpc(FN_ECI_GENERATE_PHONEMES, &args, &rv, &extra, &elen);
    rpc_buf_free(&args);
    if (pBuffer && extra && elen > 0) {
        uint32_t pos = 0;
        const char *str; uint32_t slen;
        if (rpc_decode_string(extra, elen, &pos, &str, &slen) == 0 && str) {
            uint32_t copy = slen < (uint32_t)iSize ? slen : (uint32_t)(iSize - 1);
            memcpy(pBuffer, str, copy);
            ((char *)pBuffer)[copy] = '\0';
        }
    }
    free(extra);
    return (Boolean)rv;
}

int eciGetIndex(ECIHand hEngine)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    int32_t rv;
    do_rpc(FN_ECI_GET_INDEX, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (int)rv;
}

Boolean eciStop(ECIHand hEngine)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    int32_t rv;
    do_rpc(FN_ECI_STOP, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (Boolean)rv;
}

Boolean eciSpeaking(ECIHand hEngine)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    int32_t rv;
    do_rpc(FN_ECI_SPEAKING, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (Boolean)rv;
}

Boolean eciSynchronize(ECIHand hEngine)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    int32_t rv;
    do_rpc(FN_ECI_SYNCHRONIZE, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (Boolean)rv;
}

Boolean eciSpeakText(ECIInputText pText, Boolean bAnnotationsInTextPhrase)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_string(&args, (const char *)pText);
    rpc_encode_int32(&args, bAnnotationsInTextPhrase);
    int32_t rv;
    do_rpc(FN_ECI_SPEAK_TEXT, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (Boolean)rv;
}

Boolean eciSpeakTextEx(ECIInputText pText, Boolean bAnnotationsInTextPhrase,
                       enum ECILanguageDialect Value)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_string(&args, (const char *)pText);
    rpc_encode_int32(&args, bAnnotationsInTextPhrase);
    rpc_encode_int32(&args, (int32_t)Value);
    int32_t rv;
    do_rpc(FN_ECI_SPEAK_TEXT_EX, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (Boolean)rv;
}

/* --- Params --- */

int eciGetParam(ECIHand hEngine, enum ECIParam Param)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_int32(&args, (int32_t)Param);
    int32_t rv;
    do_rpc(FN_ECI_GET_PARAM, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (int)rv;
}

int eciSetParam(ECIHand hEngine, enum ECIParam Param, int iValue)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_int32(&args, (int32_t)Param);
    rpc_encode_int32(&args, iValue);
    int32_t rv;
    do_rpc(FN_ECI_SET_PARAM, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (int)rv;
}

int eciGetDefaultParam(enum ECIParam parameter)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_int32(&args, (int32_t)parameter);
    int32_t rv;
    do_rpc(FN_ECI_GET_DEFAULT_PARAM, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (int)rv;
}

int eciSetDefaultParam(enum ECIParam parameter, int value)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_int32(&args, (int32_t)parameter);
    rpc_encode_int32(&args, value);
    int32_t rv;
    do_rpc(FN_ECI_SET_DEFAULT_PARAM, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (int)rv;
}

/* --- Audio --- */

Boolean eciSetOutputBuffer(ECIHand hEngine, int iSize, short *psBuffer)
{
    uint32_t hid = hand_to_id(hEngine);

    /* Store host-side buffer locally */
    shim_set_output_buffer(hid, iSize, psBuffer);

    /* Tell bridge to allocate ARM-side buffer of same size */
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hid);
    rpc_encode_int32(&args, iSize);
    int32_t rv;
    do_rpc(FN_ECI_SET_OUTPUT_BUFFER, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (Boolean)rv;
}

Boolean eciSetOutputFilename(ECIHand hEngine, const void *pFilename)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_string(&args, (const char *)pFilename);
    int32_t rv;
    do_rpc(FN_ECI_SET_OUTPUT_FILENAME, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (Boolean)rv;
}

Boolean eciSetOutputDevice(ECIHand hEngine, int iDevNum)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_int32(&args, iDevNum);
    int32_t rv;
    do_rpc(FN_ECI_SET_OUTPUT_DEVICE, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (Boolean)rv;
}

Boolean eciPause(ECIHand hEngine, Boolean On)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_int32(&args, On);
    int32_t rv;
    do_rpc(FN_ECI_PAUSE, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (Boolean)rv;
}

void eciRegisterCallback(ECIHand hEngine, ECICallback Callback, void *pData)
{
    uint32_t hid = hand_to_id(hEngine);

    /* Store callback locally */
    shim_set_callback(hid, Callback, pData);

    /* Tell bridge this handle has an active callback */
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hid);
    do_rpc(FN_ECI_REGISTER_CALLBACK, &args, NULL, NULL, NULL);
    rpc_buf_free(&args);
}

/* --- Voice --- */

Boolean eciCopyVoice(ECIHand hEngine, int iVoiceFrom, int iVoiceTo)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_int32(&args, iVoiceFrom);
    rpc_encode_int32(&args, iVoiceTo);
    int32_t rv;
    do_rpc(FN_ECI_COPY_VOICE, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (Boolean)rv;
}

Boolean eciGetVoiceName(ECIHand hEngine, int iVoice, void *pBuffer)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_int32(&args, iVoice);
    int32_t rv;
    uint8_t *extra = NULL;
    uint32_t elen;
    do_rpc(FN_ECI_GET_VOICE_NAME, &args, &rv, &extra, &elen);
    rpc_buf_free(&args);
    if (pBuffer && extra && elen > 0) {
        uint32_t pos = 0;
        const char *str; uint32_t slen;
        if (rpc_decode_string(extra, elen, &pos, &str, &slen) == 0 && str) {
            uint32_t copy = slen < ECI_VOICE_NAME_LENGTH ? slen : ECI_VOICE_NAME_LENGTH;
            memcpy(pBuffer, str, copy);
            ((char *)pBuffer)[copy] = '\0';
        }
    }
    free(extra);
    return (Boolean)rv;
}

Boolean eciSetVoiceName(ECIHand hEngine, int iVoice, const void *pBuffer)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_int32(&args, iVoice);
    rpc_encode_string(&args, (const char *)pBuffer);
    int32_t rv;
    do_rpc(FN_ECI_SET_VOICE_NAME, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (Boolean)rv;
}

int eciGetVoiceParam(ECIHand hEngine, int iVoice, enum ECIVoiceParam Param)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_int32(&args, iVoice);
    rpc_encode_int32(&args, (int32_t)Param);
    int32_t rv;
    do_rpc(FN_ECI_GET_VOICE_PARAM, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (int)rv;
}

int eciSetVoiceParam(ECIHand hEngine, int iVoice, enum ECIVoiceParam Param, int iValue)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_int32(&args, iVoice);
    rpc_encode_int32(&args, (int32_t)Param);
    rpc_encode_int32(&args, iValue);
    int32_t rv;
    do_rpc(FN_ECI_SET_VOICE_PARAM, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (int)rv;
}

/* --- Dictionary --- */

ECIDictHand eciNewDict(ECIHand hEngine)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    int32_t rv;
    uint8_t *extra = NULL;
    uint32_t elen;
    do_rpc(FN_ECI_NEW_DICT, &args, &rv, &extra, &elen);
    rpc_buf_free(&args);
    uint32_t dhid = 0;
    if (extra && elen >= 5) {
        uint32_t pos = 0;
        rpc_decode_handle(extra, elen, &pos, &dhid);
    }
    free(extra);
    return (ECIDictHand)(uintptr_t)dhid;
}

ECIDictHand eciGetDict(ECIHand hEngine)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    int32_t rv;
    uint8_t *extra = NULL;
    uint32_t elen;
    do_rpc(FN_ECI_GET_DICT, &args, &rv, &extra, &elen);
    rpc_buf_free(&args);
    uint32_t dhid = 0;
    if (extra && elen >= 5) {
        uint32_t pos = 0;
        rpc_decode_handle(extra, elen, &pos, &dhid);
    }
    free(extra);
    return (ECIDictHand)(uintptr_t)dhid;
}

enum ECIDictError eciSetDict(ECIHand hEngine, ECIDictHand hDict)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_handle(&args, (uint32_t)(uintptr_t)hDict);
    int32_t rv;
    do_rpc(FN_ECI_SET_DICT, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (enum ECIDictError)rv;
}

ECIDictHand eciDeleteDict(ECIHand hEngine, ECIDictHand hDict)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_handle(&args, (uint32_t)(uintptr_t)hDict);
    do_rpc(FN_ECI_DELETE_DICT, &args, NULL, NULL, NULL);
    rpc_buf_free(&args);
    return NULL_DICT_HAND;
}

enum ECIDictError eciLoadDict(ECIHand hEngine, ECIDictHand hDict,
                              enum ECIDictVolume DictVol, ECIInputText pFilename)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_handle(&args, (uint32_t)(uintptr_t)hDict);
    rpc_encode_int32(&args, (int32_t)DictVol);
    rpc_encode_string(&args, (const char *)pFilename);
    int32_t rv;
    do_rpc(FN_ECI_LOAD_DICT, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (enum ECIDictError)rv;
}

enum ECIDictError eciSaveDict(ECIHand hEngine, ECIDictHand hDict,
                              enum ECIDictVolume DictVol, ECIInputText pFilename)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_handle(&args, (uint32_t)(uintptr_t)hDict);
    rpc_encode_int32(&args, (int32_t)DictVol);
    rpc_encode_string(&args, (const char *)pFilename);
    int32_t rv;
    do_rpc(FN_ECI_SAVE_DICT, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (enum ECIDictError)rv;
}

enum ECIDictError eciUpdateDict(ECIHand hEngine, ECIDictHand hDict,
                                enum ECIDictVolume DictVol,
                                ECIInputText pKey, ECIInputText pTranslationValue)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_handle(&args, (uint32_t)(uintptr_t)hDict);
    rpc_encode_int32(&args, (int32_t)DictVol);
    rpc_encode_string(&args, (const char *)pKey);
    rpc_encode_string(&args, (const char *)pTranslationValue);
    int32_t rv;
    do_rpc(FN_ECI_UPDATE_DICT, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (enum ECIDictError)rv;
}

/* Static buffers for dict lookup results */
static char g_dict_key_buf[4096];
static char g_dict_val_buf[4096];

const char *eciDictLookup(ECIHand hEngine, ECIDictHand hDict,
                          enum ECIDictVolume DictVol, ECIInputText pKey)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_handle(&args, (uint32_t)(uintptr_t)hDict);
    rpc_encode_int32(&args, (int32_t)DictVol);
    rpc_encode_string(&args, (const char *)pKey);
    int32_t rv;
    uint8_t *extra = NULL;
    uint32_t elen;
    do_rpc(FN_ECI_DICT_LOOKUP, &args, &rv, &extra, &elen);
    rpc_buf_free(&args);

    if (rv < 0 || !extra) { free(extra); return NULL; }

    uint32_t pos = 0;
    const char *str; uint32_t slen;
    if (rpc_decode_string(extra, elen, &pos, &str, &slen) == 0 && str) {
        uint32_t copy = slen < sizeof(g_dict_val_buf) - 1 ? slen : sizeof(g_dict_val_buf) - 1;
        memcpy(g_dict_val_buf, str, copy);
        g_dict_val_buf[copy] = '\0';
        free(extra);
        return g_dict_val_buf;
    }
    free(extra);
    return NULL;
}

/* We need to undef the macros from eci.h to define the actual functions */
#undef eciDictFindFirst
#undef eciDictFindNext

enum ECIDictError eciDictFindFirst(ECIHand hEngine, ECIDictHand hDict,
                                   enum ECIDictVolume DictVol,
                                   ECIInputText *ppKey, ECIInputText *ppTranslationValue)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_handle(&args, (uint32_t)(uintptr_t)hDict);
    rpc_encode_int32(&args, (int32_t)DictVol);
    int32_t rv;
    uint8_t *extra = NULL;
    uint32_t elen;
    do_rpc(FN_ECI_DICT_FIND_FIRST, &args, &rv, &extra, &elen);
    rpc_buf_free(&args);

    if (extra && elen > 0) {
        uint32_t pos = 0;
        const char *str; uint32_t slen;
        if (rpc_decode_string(extra, elen, &pos, &str, &slen) == 0 && str) {
            uint32_t copy = slen < sizeof(g_dict_key_buf) - 1 ? slen : sizeof(g_dict_key_buf) - 1;
            memcpy(g_dict_key_buf, str, copy);
            g_dict_key_buf[copy] = '\0';
            if (ppKey) *ppKey = g_dict_key_buf;
        }
        if (rpc_decode_string(extra, elen, &pos, &str, &slen) == 0 && str) {
            uint32_t copy = slen < sizeof(g_dict_val_buf) - 1 ? slen : sizeof(g_dict_val_buf) - 1;
            memcpy(g_dict_val_buf, str, copy);
            g_dict_val_buf[copy] = '\0';
            if (ppTranslationValue) *ppTranslationValue = g_dict_val_buf;
        }
    }
    free(extra);
    return (enum ECIDictError)rv;
}

enum ECIDictError eciDictFindNext(ECIHand hEngine, ECIDictHand hDict,
                                  enum ECIDictVolume DictVol,
                                  ECIInputText *ppKey, ECIInputText *ppTranslationValue)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_handle(&args, (uint32_t)(uintptr_t)hDict);
    rpc_encode_int32(&args, (int32_t)DictVol);
    int32_t rv;
    uint8_t *extra = NULL;
    uint32_t elen;
    do_rpc(FN_ECI_DICT_FIND_NEXT, &args, &rv, &extra, &elen);
    rpc_buf_free(&args);

    if (extra && elen > 0) {
        uint32_t pos = 0;
        const char *str; uint32_t slen;
        if (rpc_decode_string(extra, elen, &pos, &str, &slen) == 0 && str) {
            uint32_t copy = slen < sizeof(g_dict_key_buf) - 1 ? slen : sizeof(g_dict_key_buf) - 1;
            memcpy(g_dict_key_buf, str, copy);
            g_dict_key_buf[copy] = '\0';
            if (ppKey) *ppKey = g_dict_key_buf;
        }
        if (rpc_decode_string(extra, elen, &pos, &str, &slen) == 0 && str) {
            uint32_t copy = slen < sizeof(g_dict_val_buf) - 1 ? slen : sizeof(g_dict_val_buf) - 1;
            memcpy(g_dict_val_buf, str, copy);
            g_dict_val_buf[copy] = '\0';
            if (ppTranslationValue) *ppTranslationValue = g_dict_val_buf;
        }
    }
    free(extra);
    return (enum ECIDictError)rv;
}

enum ECIDictError eciUpdateDictA(ECIHand hEngine, ECIDictHand hDict,
                                 enum ECIDictVolume DictVol,
                                 ECIInputText pKey, ECIInputText pTranslationValue,
                                 enum ECIPartOfSpeech PartOfSpeech)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_handle(&args, (uint32_t)(uintptr_t)hDict);
    rpc_encode_int32(&args, (int32_t)DictVol);
    rpc_encode_string(&args, (const char *)pKey);
    rpc_encode_string(&args, (const char *)pTranslationValue);
    rpc_encode_int32(&args, (int32_t)PartOfSpeech);
    int32_t rv;
    do_rpc(FN_ECI_UPDATE_DICT_A, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (enum ECIDictError)rv;
}

enum ECIDictError eciDictFindFirstA(ECIHand hEngine, ECIDictHand hDict,
                                    enum ECIDictVolume DictVol,
                                    ECIInputText *ppKey, ECIInputText *ppTranslationValue,
                                    enum ECIPartOfSpeech *pPartOfSpeech)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_handle(&args, (uint32_t)(uintptr_t)hDict);
    rpc_encode_int32(&args, (int32_t)DictVol);
    int32_t rv;
    uint8_t *extra = NULL;
    uint32_t elen;
    do_rpc(FN_ECI_DICT_FIND_FIRST_A, &args, &rv, &extra, &elen);
    rpc_buf_free(&args);

    if (extra && elen > 0) {
        uint32_t pos = 0;
        const char *str; uint32_t slen;
        if (rpc_decode_string(extra, elen, &pos, &str, &slen) == 0 && str) {
            uint32_t copy = slen < sizeof(g_dict_key_buf) - 1 ? slen : sizeof(g_dict_key_buf) - 1;
            memcpy(g_dict_key_buf, str, copy);
            g_dict_key_buf[copy] = '\0';
            if (ppKey) *ppKey = g_dict_key_buf;
        }
        if (rpc_decode_string(extra, elen, &pos, &str, &slen) == 0 && str) {
            uint32_t copy = slen < sizeof(g_dict_val_buf) - 1 ? slen : sizeof(g_dict_val_buf) - 1;
            memcpy(g_dict_val_buf, str, copy);
            g_dict_val_buf[copy] = '\0';
            if (ppTranslationValue) *ppTranslationValue = g_dict_val_buf;
        }
        int32_t posval;
        if (pPartOfSpeech && rpc_decode_int32(extra, elen, &pos, &posval) == 0)
            *pPartOfSpeech = (enum ECIPartOfSpeech)posval;
    }
    free(extra);
    return (enum ECIDictError)rv;
}

enum ECIDictError eciDictFindNextA(ECIHand hEngine, ECIDictHand hDict,
                                   enum ECIDictVolume DictVol,
                                   ECIInputText *ppKey, ECIInputText *ppTranslationValue,
                                   enum ECIPartOfSpeech *pPartOfSpeech)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_handle(&args, (uint32_t)(uintptr_t)hDict);
    rpc_encode_int32(&args, (int32_t)DictVol);
    int32_t rv;
    uint8_t *extra = NULL;
    uint32_t elen;
    do_rpc(FN_ECI_DICT_FIND_NEXT_A, &args, &rv, &extra, &elen);
    rpc_buf_free(&args);

    if (extra && elen > 0) {
        uint32_t pos = 0;
        const char *str; uint32_t slen;
        if (rpc_decode_string(extra, elen, &pos, &str, &slen) == 0 && str) {
            uint32_t copy = slen < sizeof(g_dict_key_buf) - 1 ? slen : sizeof(g_dict_key_buf) - 1;
            memcpy(g_dict_key_buf, str, copy);
            g_dict_key_buf[copy] = '\0';
            if (ppKey) *ppKey = g_dict_key_buf;
        }
        if (rpc_decode_string(extra, elen, &pos, &str, &slen) == 0 && str) {
            uint32_t copy = slen < sizeof(g_dict_val_buf) - 1 ? slen : sizeof(g_dict_val_buf) - 1;
            memcpy(g_dict_val_buf, str, copy);
            g_dict_val_buf[copy] = '\0';
            if (ppTranslationValue) *ppTranslationValue = g_dict_val_buf;
        }
        int32_t posval;
        if (pPartOfSpeech && rpc_decode_int32(extra, elen, &pos, &posval) == 0)
            *pPartOfSpeech = (enum ECIPartOfSpeech)posval;
    }
    free(extra);
    return (enum ECIDictError)rv;
}

enum ECIDictError eciDictLookupA(ECIHand hEngine, ECIDictHand hDict,
                                 enum ECIDictVolume DictVol,
                                 ECIInputText pKey, ECIInputText *ppTranslationValue,
                                 enum ECIPartOfSpeech *pPartOfSpeech)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(hEngine));
    rpc_encode_handle(&args, (uint32_t)(uintptr_t)hDict);
    rpc_encode_int32(&args, (int32_t)DictVol);
    rpc_encode_string(&args, (const char *)pKey);
    int32_t rv;
    uint8_t *extra = NULL;
    uint32_t elen;
    do_rpc(FN_ECI_DICT_LOOKUP_A, &args, &rv, &extra, &elen);
    rpc_buf_free(&args);

    if (extra && elen > 0) {
        uint32_t pos = 0;
        const char *str; uint32_t slen;
        if (rpc_decode_string(extra, elen, &pos, &str, &slen) == 0 && str) {
            uint32_t copy = slen < sizeof(g_dict_val_buf) - 1 ? slen : sizeof(g_dict_val_buf) - 1;
            memcpy(g_dict_val_buf, str, copy);
            g_dict_val_buf[copy] = '\0';
            if (ppTranslationValue) *ppTranslationValue = g_dict_val_buf;
        }
        int32_t posval;
        if (pPartOfSpeech && rpc_decode_int32(extra, elen, &pos, &posval) == 0)
            *pPartOfSpeech = (enum ECIPartOfSpeech)posval;
    }
    free(extra);
    return (enum ECIDictError)rv;
}

/* --- Voice registration (rarely used, stub) --- */

enum ECIVoiceError eciRegisterVoice(ECIHand eciHand, int voiceNumber,
                                    void *vData, ECIVoiceAttrib *vAttrib)
{
    (void)eciHand; (void)voiceNumber; (void)vData; (void)vAttrib;
    return VoiceSystemError;
}

enum ECIVoiceError eciUnregisterVoice(ECIHand eciHand, int voiceNumber,
                                      ECIVoiceAttrib *vAttrib, void **vData)
{
    (void)eciHand; (void)voiceNumber; (void)vAttrib; (void)vData;
    return VoiceSystemError;
}

/* --- Filter --- */

ECIFilterHand eciNewFilter(ECIHand eciHandle, unsigned int filterNum, Boolean bGlobal)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(eciHandle));
    rpc_encode_uint32(&args, filterNum);
    rpc_encode_int32(&args, bGlobal);
    int32_t rv;
    uint8_t *extra = NULL;
    uint32_t elen;
    do_rpc(FN_ECI_NEW_FILTER, &args, &rv, &extra, &elen);
    rpc_buf_free(&args);
    uint32_t fhid = 0;
    if (extra && elen >= 5) {
        uint32_t pos = 0;
        rpc_decode_handle(extra, elen, &pos, &fhid);
    }
    free(extra);
    return (ECIFilterHand)(uintptr_t)fhid;
}

ECIFilterHand eciDeleteFilter(ECIHand eciHandle, ECIFilterHand whichFilterHand)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(eciHandle));
    rpc_encode_handle(&args, (uint32_t)(uintptr_t)whichFilterHand);
    do_rpc(FN_ECI_DELETE_FILTER, &args, NULL, NULL, NULL);
    rpc_buf_free(&args);
    return NULL_FILTER_HAND;
}

enum ECIFilterError eciActivateFilter(ECIHand eciHandle, ECIFilterHand whichFilterHand)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(eciHandle));
    rpc_encode_handle(&args, (uint32_t)(uintptr_t)whichFilterHand);
    int32_t rv;
    do_rpc(FN_ECI_ACTIVATE_FILTER, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (enum ECIFilterError)rv;
}

enum ECIFilterError eciDeactivateFilter(ECIHand eciHandle, ECIFilterHand pFilter)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(eciHandle));
    rpc_encode_handle(&args, (uint32_t)(uintptr_t)pFilter);
    int32_t rv;
    do_rpc(FN_ECI_DEACTIVATE_FILTER, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (enum ECIFilterError)rv;
}

enum ECIFilterError eciUpdateFilter(ECIHand eciHandle, ECIFilterHand whichFilterHand,
                                    ECIInputText key, ECIInputText translation)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(eciHandle));
    rpc_encode_handle(&args, (uint32_t)(uintptr_t)whichFilterHand);
    rpc_encode_string(&args, (const char *)key);
    rpc_encode_string(&args, (const char *)translation);
    int32_t rv;
    do_rpc(FN_ECI_UPDATE_FILTER, &args, &rv, NULL, NULL);
    rpc_buf_free(&args);
    return (enum ECIFilterError)rv;
}

enum ECIFilterError eciGetFilteredText(ECIHand eciHandle, ECIFilterHand whichFilterHand,
                                       ECIInputText input, ECIInputText *filteredText)
{
    rpc_buf_t args;
    rpc_buf_init(&args);
    rpc_encode_handle(&args, hand_to_id(eciHandle));
    rpc_encode_handle(&args, (uint32_t)(uintptr_t)whichFilterHand);
    rpc_encode_string(&args, (const char *)input);
    int32_t rv;
    uint8_t *extra = NULL;
    uint32_t elen;
    do_rpc(FN_ECI_GET_FILTERED_TEXT, &args, &rv, &extra, &elen);
    rpc_buf_free(&args);

    static char filtered_buf[MAX_STRING_LEN];
    if (filteredText && extra && elen > 0) {
        uint32_t pos = 0;
        const char *str; uint32_t slen;
        if (rpc_decode_string(extra, elen, &pos, &str, &slen) == 0 && str) {
            uint32_t copy = slen < sizeof(filtered_buf) - 1 ? slen : sizeof(filtered_buf) - 1;
            memcpy(filtered_buf, str, copy);
            filtered_buf[copy] = '\0';
            *filteredText = filtered_buf;
        }
    }
    free(extra);
    return (enum ECIFilterError)rv;
}
