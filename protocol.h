#ifndef PROTOCOL_H
#define PROTOCOL_H

#define MAX_USERNAME  32
#define MAX_PASSWORD  64
#define MAX_DOC_NAME  64
#define MAX_CONTENT   65536
#define MAX_USERS     16
#define HTTP_PORT     9090

/* Message type strings sent as JSON "type" field */
#define MSG_AUTH_REQ    "auth_req"
#define MSG_AUTH_OK     "auth_ok"
#define MSG_AUTH_FAIL   "auth_fail"
#define MSG_DOC_STATE   "doc_state"
#define MSG_EDIT        "edit"
#define MSG_CURSOR      "cursor"
#define MSG_PRESENCE    "presence"
#define MSG_STATS       "stats"
#define MSG_SAVE        "save"
#define MSG_LOCK_REQ    "lock_req"
#define MSG_LOCK_OK     "lock_ok"
#define MSG_LOCK_DENY   "lock_deny"
#define MSG_UNLOCK      "unlock"
#define MSG_ERROR       "error"
#define MSG_PING        "ping"
#define MSG_PONG        "pong"

/* Edit ops */
#define OP_INSERT  "insert"
#define OP_DELETE  "delete"
#define OP_FORMAT  "format"

#endif
