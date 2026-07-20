/*
 * ntuapp.h - protocolo publico de surfaces NTUnix (apps <-> dispd).
 *
 * v1 continua aceito:
 *   APP-HELLO <w> <h> <title>
 *   APP-SURFACE <section> <w> <h>
 *   APP-COMMIT
 *
 * v2 separa role, configuracao e attach. Pixels sao BGRA premultiplicados.
 * O app so desenha num novo section depois de APP-CONFIGURE e responde
 * APP-ACK <serial>; APP-COMMIT <serial> publica o buffer confirmado.
 */
#ifndef NTUAPP_H
#define NTUAPP_H

#define NTUAPP_PROTO_VER 2

#define NTUAPP_ROLE_TOPLEVEL "toplevel"
#define NTUAPP_ROLE_LAYER    "layer"

#define NTUAPP_ANCHOR_TOP    0x01u
#define NTUAPP_ANCHOR_BOTTOM 0x02u
#define NTUAPP_ANCHOR_LEFT   0x04u
#define NTUAPP_ANCHOR_RIGHT  0x08u

#define NTUAPP_INTERACT_NONE      0
#define NTUAPP_INTERACT_ON_DEMAND 1
#define NTUAPP_INTERACT_EXCLUSIVE 2

#define APP_CMD_HELLO     "APP-HELLO"
#define APP_CMD_ACK       "APP-ACK"
#define APP_CMD_COMMIT    "APP-COMMIT"
#define APP_CMD_CLOSE     "APP-CLOSE"
#define APP_EVT_WELCOME   "APP-WELCOME"
#define APP_EVT_SURFACE   "APP-SURFACE"
#define APP_EVT_CONFIGURE "APP-CONFIGURE"
#define APP_EVT_KEY       "APP-KEY"
#define APP_EVT_POINTER   "APP-POINTER"
#define APP_EVT_MOUSE     "APP-MOUSE"
#define APP_EVT_CLOSE     "APP-CLOSE-REQUEST"
#define APP_EVT_ERROR     "APP-ERR"

#endif
