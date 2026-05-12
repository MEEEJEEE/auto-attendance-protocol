#include "mbed.h"
#include "PHYMAC_layer.h"
#include "L2_FSMevent.h"
#include "L2_msg.h"
#include "protocol_parameters.h"
#include "time.h"

#define L2_LLI_MAX_PDUSIZE          50
#define L2_LLI_PKT_LOSS             0

static uint8_t txType;
static uint8_t rcvdData[L2_LLI_MAX_PDUSIZE];
static uint8_t rcvdSrc;
static uint8_t rcvdSize;
static int16_t rcvdRssi;
static int8_t rcvdSnr;
static uint8_t isBroadcasted;

//interface event : DATA_CNF, TX done event
void L2_LLI_dataCnfFunc(int err) 
{
    if (txType == L2_MSG_TYPE_DATA || txType == L2_MSG_TYPE_DATA_CONT)
    {
        L2_event_setEventFlag(L2_event_dataTxDone);
    }
    else if (txType == L2_MSG_TYPE_ACK)
    {
        L2_event_setEventFlag(L2_event_ackTxDone);
    }
}

//interface event : DATA_IND, RX data has arrived
void L2_LLI_dataIndFunc(uint8_t srcId, uint8_t* dataPtr, uint8_t size, uint8_t BR)
{
    debug_if(DBGMSG_L2, "\n[L2]  --> DATA IND : src:%i, size:%i type : %i BR : %i\n", srcId, size, dataPtr[0], BR);

    if ((float)rand()/RAND_MAX > L2_LLI_PKT_LOSS)
    {
        memcpy(rcvdData, dataPtr, size*sizeof(uint8_t));
        rcvdSrc = srcId;
        rcvdSize = size;
        rcvdSnr = phymac_getDataSnr();
        rcvdRssi = phymac_getDataRssi();
        isBroadcasted = BR;

        //ready for ACK TX
        if (L2_msg_checkIfData(dataPtr))
        {
            L2_event_setEventFlag(L2_event_dataRcvd);
        }
        else if (L2_msg_checkIfAck(dataPtr))
        {
            L2_event_setEventFlag(L2_event_ackRcvd);
        }
    }
    else
    {
        debug_if(DBGMSG_L2, "\n\n PDU error!\n");
    }
}


void L2_LLI_initLowLayer(uint8_t srcId)
{
    srand(time(NULL));
    phymac_init(srcId, L1_FREQCHANNEL, L2_LLI_dataCnfFunc, L2_LLI_dataIndFunc);
}




//TX function
void L2_LLI_sendData(uint8_t* msg, uint8_t size, uint8_t dest)
{
    phymac_dataReq(msg, size, dest);
    txType = msg[L2_MSG_OFFSET_TYPE];
}


int L2_LLI_configSrcId(uint8_t srcId)
{
    int res;
    if ( (res=phymac_configSrcId(srcId)) != PHYMAC_ERR_NONE)
        debug("[L2] Failed to config Src ID at PHY (cause : %i)\n", res);

    return res;
}

//GET functions
uint8_t L2_LLI_getSrcId()
{
    return rcvdSrc;
}

uint8_t* L2_LLI_getRcvdDataPtr()
{
    return rcvdData;
}

uint8_t L2_LLI_getSize()
{
    return rcvdSize;
}


int16_t L2_LLI_getRssi(void)
{
    return rcvdRssi;
}

int8_t L2_LLI_getSnr(void)
{
    return rcvdSnr;
}

uint8_t L2_LLI_getIsBroadcasted(void)
{
    return isBroadcasted;
}