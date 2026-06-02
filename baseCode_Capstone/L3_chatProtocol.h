#ifndef L3_CHAT_PROTOCOL_H
#define L3_CHAT_PROTOCOL_H

#include <stdint.h>
#include <string.h>

/*
========================================================
 Chat Protocol
 Student <-> Student Direct Messaging
========================================================
*/

/*
========================================================
 Chat Mode / Type
========================================================
*/
#define PACKET_MODE_STUDENT_TO_STUDENT   0x03U
#define TYPE_CHAT                      0x60U

/*
========================================================
 Configuration
========================================================
*/
#define CHAT_MAX_MESSAGE_LEN           32
#pragma pack(push, 1)

/*
========================================================
 Chat Packet Structure
========================================================
*/
typedef struct
{
    uint32_t mode;                         // CHAT_MODE_STUDENT_TO_STUDENT
    uint32_t type;                         // TYPE_CHAT

    uint8_t src_id;                       // sender student ID
    uint8_t dst_id;                       // destination student ID

    char message[CHAT_MAX_MESSAGE_LEN];   // null-terminated message

} chat_packet_t;

#pragma pack(pop)

/*
========================================================
 Chat Packet Create Function
========================================================
*/
static inline void makeChatPacket(
    chat_packet_t* pkt,
    uint8_t srcId,
    uint8_t dstId,
    const char* msg
)
{
    pkt->mode       = PACKET_MODE_STUDENT_TO_STUDENT;
    pkt->type       = TYPE_CHAT;

    pkt->src_id = srcId;
    pkt->dst_id = dstId;

    memset(pkt->message, 0, CHAT_MAX_MESSAGE_LEN);

    strncpy(pkt->message,
            msg,
            CHAT_MAX_MESSAGE_LEN - 1);
}

#endif