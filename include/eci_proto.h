#ifndef ECI_PROTO_H
#define ECI_PROTO_H

#include <stdint.h>

/* Message types */
#define MSG_RPC_REQUEST     0x01
#define MSG_RPC_RESPONSE    0x02
#define MSG_CALLBACK_EVENT  0x03
#define MSG_CALLBACK_RETURN 0x04
#define MSG_HANDSHAKE       0x05
#define MSG_HANDSHAKE_ACK   0x06

/* Argument type tags */
#define ARG_INT32   0x01
#define ARG_UINT32  0x02
#define ARG_HANDLE  0x03
#define ARG_STRING  0x04
#define ARG_BUFFER  0x05
#define ARG_NULL    0x06

/* Function IDs -- lifecycle 0x00xx */
#define FN_ECI_NEW                  0x0001
#define FN_ECI_NEW_EX               0x0002
#define FN_ECI_DELETE               0x0003
#define FN_ECI_RESET                0x0004
#define FN_ECI_IS_BEING_REENTERED   0x0005
#define FN_ECI_VERSION              0x0006
#define FN_ECI_PROG_STATUS          0x0007
#define FN_ECI_ERROR_MESSAGE        0x0008
#define FN_ECI_CLEAR_ERRORS         0x0009
#define FN_ECI_TEST_PHRASE          0x000A
#define FN_ECI_GET_AVAILABLE_LANGS  0x000B

/* synthesis 0x01xx */
#define FN_ECI_ADD_TEXT             0x0100
#define FN_ECI_INSERT_INDEX        0x0101
#define FN_ECI_SYNTHESIZE          0x0102
#define FN_ECI_SYNTHESIZE_FILE     0x0103
#define FN_ECI_CLEAR_INPUT         0x0104
#define FN_ECI_GENERATE_PHONEMES   0x0105
#define FN_ECI_GET_INDEX           0x0106
#define FN_ECI_STOP                0x0107
#define FN_ECI_SPEAKING            0x0108
#define FN_ECI_SYNCHRONIZE         0x0109
#define FN_ECI_SPEAK_TEXT          0x010A
#define FN_ECI_SPEAK_TEXT_EX       0x010B

/* params 0x02xx */
#define FN_ECI_GET_PARAM           0x0200
#define FN_ECI_SET_PARAM           0x0201
#define FN_ECI_GET_DEFAULT_PARAM   0x0202
#define FN_ECI_SET_DEFAULT_PARAM   0x0203

/* audio 0x03xx */
#define FN_ECI_SET_OUTPUT_BUFFER   0x0300
#define FN_ECI_SET_OUTPUT_FILENAME 0x0301
#define FN_ECI_SET_OUTPUT_DEVICE   0x0302
#define FN_ECI_PAUSE               0x0303
#define FN_ECI_REGISTER_CALLBACK   0x0304

/* voice 0x04xx */
#define FN_ECI_COPY_VOICE          0x0400
#define FN_ECI_GET_VOICE_NAME      0x0401
#define FN_ECI_SET_VOICE_NAME      0x0402
#define FN_ECI_GET_VOICE_PARAM     0x0403
#define FN_ECI_SET_VOICE_PARAM     0x0404
#define FN_ECI_REGISTER_VOICE      0x0405
#define FN_ECI_UNREGISTER_VOICE    0x0406

/* dict 0x05xx */
#define FN_ECI_NEW_DICT            0x0500
#define FN_ECI_GET_DICT            0x0501
#define FN_ECI_SET_DICT            0x0502
#define FN_ECI_DELETE_DICT         0x0503
#define FN_ECI_LOAD_DICT           0x0504
#define FN_ECI_SAVE_DICT           0x0505
#define FN_ECI_UPDATE_DICT         0x0506
#define FN_ECI_DICT_FIND_FIRST     0x0507
#define FN_ECI_DICT_FIND_NEXT      0x0508
#define FN_ECI_DICT_LOOKUP         0x0509
#define FN_ECI_UPDATE_DICT_A       0x050A
#define FN_ECI_DICT_FIND_FIRST_A   0x050B
#define FN_ECI_DICT_FIND_NEXT_A    0x050C
#define FN_ECI_DICT_LOOKUP_A       0x050D

/* filter 0x07xx */
#define FN_ECI_NEW_FILTER          0x0700
#define FN_ECI_DELETE_FILTER       0x0701
#define FN_ECI_ACTIVATE_FILTER     0x0702
#define FN_ECI_DEACTIVATE_FILTER   0x0703
#define FN_ECI_UPDATE_FILTER       0x0704
#define FN_ECI_GET_FILTERED_TEXT   0x0705

/* Handshake: client sends client_id on callback channel to associate it */
typedef struct {
    uint32_t client_id;
} handshake_msg_t;

/* Max sizes */
#define MAX_MSG_SIZE       (4 * 1024 * 1024)
#define MAX_STRING_LEN     (64 * 1024)
#define MAX_BUFFER_SIZE    (2 * 1024 * 1024)
#define SOCKET_PATH_ENV    "ECI_BRIDGE_SOCKET"
#define DEFAULT_SOCKET_NAME "eci-bridge.sock"
#define PID_FILE_NAME      "eci-bridge.pid"

#endif /* ECI_PROTO_H */
