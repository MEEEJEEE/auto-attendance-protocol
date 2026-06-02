#include "L3_chatSend.h"

#include "L2_connection.h"
#include "L3_LLinterface.h"

void L3_sendChatPacket(chat_packet_t* pkt)
{
    // connect to chat peer
    L2_connect(pkt->dst_id);

    debug_if(DBGMSG_L3,
             "[CHAT] SEND %u -> %u : %s\n",
             pkt->src_id,
             pkt->dst_id,
             pkt->message);

    L3_LLI_dataReqFunc(
        (uint8_t*)pkt,
        sizeof(chat_packet_t),
        pkt->dst_id
    );
}