#include "mbed.h"
#include "L3_FSMevent.h"
#include "L3_msg.h"
#include "protocol_parameters.h"
#include "time.h"

static uint8_t rcvdMsg[L3_MAXDATASIZE];
static uint8_t rcvdSize;
static int16_t rcvdRssi = -120;  // initialized to clearly-out-of-range value
                                  // so students not yet in range cannot be auto-approved
static int8_t rcvdSnr;
static uint8_t rcvdSrcId;

//Downward primitives
//TX function
void (*L3_LLI_dataReqFunc)(uint8_t* msg, uint8_t size, uint8_t destId);
void (*L3_LLI_reconfigSrcIdReqFunc)(uint8_t myId);

//interface event : DATA_IND, RX data has arrived
void L3_LLI_dataInd(uint8_t* dataPtr, uint8_t srcId, uint8_t size, int8_t snr, int16_t rssi)
{
    debug_if(DBGMSG_L3, "\n[L3] --> DATA IND : size:%i, %s\n", size, dataPtr);

    memcpy(rcvdMsg, dataPtr, size*sizeof(uint8_t));
    rcvdSize = size;
    rcvdSnr = snr;
    rcvdRssi = rssi;
    rcvdSrcId = srcId;

    L3_event_setEventFlag(L3_event_msgRcvd);
}

void L3_LLI_dataCnf(uint8_t res)
{
    debug_if(DBGMSG_L3, "\n --> DATA CNF : res : %i\n", res);
    L3_event_setEventFlag(L3_event_dataSendCnf);
}
void L3_LLI_reconfigSrcIdCnf(uint8_t res)
{
    debug_if(DBGMSG_L3, "\n --> RECONFIG SRCID CNF : res : %i\n", res);
    L3_event_setEventFlag(L3_event_recfgSrcIdCnf);
}


uint8_t* L3_LLI_getMsgPtr()
{
    return rcvdMsg;
}
uint8_t L3_LLI_getSize()
{
    return rcvdSize;
}

uint8_t L3_LLI_getSrcId()
{
    return rcvdSrcId;
}

void L3_LLI_setDataReqFunc(void (*funcPtr)(uint8_t*, uint8_t, uint8_t))
{
    L3_LLI_dataReqFunc = funcPtr;
}


void L3_LLI_setReconfigSrcIdReqFunc(void (*funcPtr)(uint8_t))
{
    L3_LLI_reconfigSrcIdReqFunc = funcPtr;
}

// Returns the RSSI (dBm) of the most recently received L2 frame.
// Used by the CU FSM to determine whether a student is inside the classroom.
int16_t L3_LLI_getRssi(void)
{
    return rcvdRssi;
}

// ===== for L3_convertPacket.h sending === 

void L3_LLI_sendPacket(packet_data_t* pkt)
{
    uint8_t destId;

    switch (pkt->mode)
    {
        // Student -> CU
        case MODE_TO_CU:
        {
            destId = CU_ID;
            break;
        }

        // CU -> all Students
        case MODE_FROM_CU:
        {
            destId = L2_BROADCAST_ID;
            break;
        }

        default:
        {
            debug_if(DBGMSG_L3,
                     "[L3] invalid packet mode : %lu\n",
                     (unsigned long)pkt->mode);
            return;
        }
    }

    debug_if(DBGMSG_L3,
             "[L3] SEND type:%lu mode:%lu dest:%u\n",
             (unsigned long)pkt->type_id,
             (unsigned long)pkt->mode,
             destId);

    L3_LLI_dataReqFunc((uint8_t*)pkt,
                       sizeof(packet_data_t),
                       destId);
}