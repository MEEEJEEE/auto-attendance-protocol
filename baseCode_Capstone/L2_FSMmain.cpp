#include "L2_FSMevent.h"
#include "L2_msg.h"
#include "L2_timer.h"
#include "L2_LLinterface.h"
#include "L3_LLinterface.h"
#include "protocol_parameters.h"

//FSM state -------------------------------------------------
#define L2STATE_IDLE              0
#define L2STATE_TX                1
#ifndef DISABLE_ARQ
#define L2STATE_ACK               2
#endif

#define SDUBUFFER_SIZE              1024

//state variables
static uint8_t main_state = L2STATE_IDLE; //protocol state
static uint8_t prev_state = main_state;

//source/destination ID
static uint8_t myL2ID=1;
static uint8_t destL2ID=0;

//L2 PDU context/size
static uint8_t sduBuffer[SDUBUFFER_SIZE];
static uint8_t sduBufferSize;

static uint8_t arqPdu[200];
static uint8_t sduIn[200];
static uint8_t pduSize;
static uint8_t sduLen;

static uint8_t pduBuffer[SDUBUFFER_SIZE];
static uint8_t pduBufferSize;
//ARQ parameters -------------------------------------------------------------
static uint8_t seqNum = 0;     //ARQ sequence number
#ifndef DISABLE_ARQ
static uint8_t retxCnt = 0;    //ARQ retransmission counter
static uint8_t arqAck[5];      //ARQ ACK PDU
#define L2_BROADCAST_ID             255
#endif
static uint8_t reqestedId=0;

static uint8_t L2_validityCheck_ID(void)
{
    if (myL2ID == destL2ID)
    {
        debug("[WARNING] myID and destination ID is the same! my:%i, dest:%i\n", myL2ID, destL2ID);
        return 1;
    }

    return 0;
}


uint8_t L2_configDestId(uint8_t destId)
{
    if (L2_validityCheck_ID() == 1)
    {
        debug("[L2] Failed to config dest to ID %i\n", destId);
        return 1;
    }

    destL2ID = destId;
    return 0;
}


int L2_pullSduBuffer(uint8_t size)
{
    int res;

    if (size > sduBufferSize)
    {
        debug_if(DBGMSG_L2, "[L2][WARNING] sdu buffer size (%i) is less than request size (%i), truncating the requested size...\n", sduBufferSize, size);
        size = sduBufferSize;
    }

    memcpy(sduIn, sduBuffer, size);
    sduLen = size;


    sduBufferSize -= size;
    if (sduBufferSize > 0)
    {
        memcpy(sduBuffer, sduBuffer+size, sduBufferSize);
        res = 1;
    }  
    else
        res = 0;

    return res;
}

void L2_LLI_handleDataReq(uint8_t* sdu, uint8_t len, uint8_t destId)
{
    if (L2_configDestId(destId) == 1 && L2_event_checkEventFlag(L2_event_dataToSendBuffer))
    {
        debug_if(DBGMSG_L2, "[L2] Failed to handle DATA_REQ (dest ID is invalid or data TX is in progress...(SDU flag : %i)\n", L2_event_checkEventFlag(L2_event_dataToSendBuffer));
        return;
    }

    if (len < L2_MSG_MAXDATASIZE)
    {
        memcpy(sduIn, sdu, len);
        sduLen = len;
    }
    else
    {
        memcpy(sduBuffer, sdu, len);
        sduBufferSize = len;

        L2_pullSduBuffer(L2_MSG_MAXDATASIZE);
        L2_event_setEventFlag(L2_event_dataToSendBuffer);
    }

    L2_event_setEventFlag(L2_event_dataToSend);
}

void L2_LLI_reconfigSrcId(uint8_t myId)
{
    reqestedId = myId;
    L2_event_setEventFlag(L2_event_reconfigSrcId);
}


void L2_initFSM(uint8_t myId)
{
    myL2ID = myId;
    destL2ID = 0; 

    L2_event_clearAllEventFlag();

    L2_validityCheck_ID();

    L2_LLI_initLowLayer(myL2ID);
    L3_LLI_setDataReqFunc(L2_LLI_handleDataReq);
    L3_LLI_setReconfigSrcIdReqFunc(L2_LLI_reconfigSrcId);
}



int L2_aggregateData(uint8_t* dataPtr, uint8_t srcId, uint8_t size, uint8_t brflag, uint8_t flag_end)
{
    memcpy(pduBuffer+pduBufferSize,L2_msg_getWord(dataPtr), size);
    pduBufferSize+=size-L2_MSG_OFFSET_DATA;

    debug_if(DBGMSG_L2, "[L2] Aggregation PDU : size : %i end : %i\n", pduBufferSize, flag_end);
    if (brflag == 1 || flag_end == 1)
    {
        L3_LLI_dataInd(pduBuffer, srcId, pduBufferSize, L2_LLI_getSnr(), L2_LLI_getRssi());
        pduBufferSize = 0;

        return 0;
    }

    return 1;
}


void L2_FSMrun(void)
{
    //debug message
    if (prev_state != main_state)
    {
        debug_if(DBGMSG_L2, "[L2] State transition from %i to %i\n", prev_state, main_state);
        prev_state = main_state;
    }

    //FSM should be implemented here! ---->>>>
    switch (main_state)
    {
        case L2STATE_IDLE: //IDLE state description
            
            if (L2_event_checkEventFlag(L2_event_reconfigSrcId)) //if src id reconfiguration is requested
            {
                int res;
                res = L2_LLI_configSrcId(reqestedId);

                L3_LLI_reconfigSrcIdCnf(res==0);
                main_state = L2STATE_IDLE; //goto TX state
                L2_event_clearEventFlag(L2_event_reconfigSrcId);
            }
            else if (L2_event_checkEventFlag(L2_event_dataRcvd)) //if data reception event happens
            {
                //Retrieving data info.
#ifndef DISABLE_ARQ
                uint8_t srcId = L2_LLI_getSrcId();
#endif
                uint8_t* dataPtr = L2_LLI_getRcvdDataPtr();
                uint8_t size = L2_LLI_getSize();
                uint8_t brflag = L2_LLI_getIsBroadcasted();
                uint8_t flag_end = L2_msg_checkIfEndData(dataPtr);

                //L3_LLI_dataInd(L2_msg_getWord(dataPtr), srcId, size-L2_MSG_OFFSET_DATA, L2_LLI_getSnr(), L2_LLI_getRssi());
#ifndef DISABLE_ARQ                
                if (brflag == 0 && seqNum != L2_msg_getSeq(dataPtr))
                    debug("[L3][WARNING] Invalid PDU SN (%i) while (%i) is required! discarding it...\n", L2_msg_getSeq(dataPtr), seqNum);
                else
#endif
                    L2_aggregateData(dataPtr, srcId, size, brflag, flag_end);


#ifdef DISABLE_ARQ
                main_state = L2STATE_IDLE;
#else
                if (brflag)
                {
                    main_state = L2STATE_IDLE;
                }
                else
                {
                    //ACK transmission
                    if (brflag == 0 && seqNum == L2_msg_getSeq(dataPtr))
                        seqNum = (seqNum + 1)%L2_MSSG_MAX_SEQNUM;
                    L2_msg_encodeAck(arqAck, L2_msg_getSeq(dataPtr));
                    L2_LLI_sendData(arqAck, L2_MSG_ACKSIZE, srcId);

                    main_state = L2STATE_TX; //goto TX state
                }
#endif
                L2_event_clearEventFlag(L2_event_dataRcvd);
            }
            else if (L2_event_checkEventFlag(L2_event_dataToSend)) //if data needs to be sent (keyboard input)
            {
                //msg header setting
                pduSize = L2_msg_encodeData(arqPdu, sduIn, seqNum, sduLen, L2_event_checkEventFlag(L2_event_dataToSendBuffer) == 0);
                L2_LLI_sendData(arqPdu, pduSize, destL2ID);

#ifndef DISABLE_ARQ
                //Setting ARQ parameter 
                if (destL2ID != L2_BROADCAST_ID)
                    seqNum = (seqNum + 1)%L2_MSSG_MAX_SEQNUM;
                retxCnt = 0;
#endif
                debug_if(DBGMSG_L2, "[L2] sending to %i (seq:%i)\n", destL2ID, (seqNum-1)%L2_MSSG_MAX_SEQNUM);

                main_state = L2STATE_TX;

                L2_event_clearEventFlag(L2_event_dataToSend);
            }
            else if (L2_event_checkEventFlag(L2_event_dataToSendBuffer))
            {
                L2_event_setEventFlag(L2_event_dataToSend);

                if (L2_pullSduBuffer(L2_MSG_MAXDATASIZE) == 0)
                    L2_event_clearEventFlag(L2_event_dataToSendBuffer);
            }
#ifndef DISABLE_ARQ
            //ignore events (arqEvent_dataTxDone, arqEvent_ackTxDone, arqEvent_ackRcvd, arqEvent_arqTimeout)
            else if (L2_event_checkEventFlag(L2_event_dataTxDone)) //if data needs to be sent (keyboard input)
            {
                debug_if(DBGMSG_L2, "[L2][WARNING] cannot happen in IDLE state (event %i)\n", L2_event_dataTxDone);
                L2_event_clearEventFlag(L2_event_dataTxDone);
            }
            else if (L2_event_checkEventFlag(L2_event_ackTxDone)) //if data needs to be sent (keyboard input)
            {
                debug_if(DBGMSG_L2, "[L2][WARNING] cannot happen in IDLE state (event %i)\n", L2_event_ackTxDone);
                L2_event_clearEventFlag(L2_event_ackTxDone);
            }
            else if (L2_event_checkEventFlag(L2_event_dataTxDone)) //if data needs to be sent (keyboard input)
            {
                debug_if(DBGMSG_L2, "[L2][WARNING] cannot happen in IDLE state (event %i)\n", L2_event_ackRcvd);
                L2_event_clearEventFlag(L2_event_ackRcvd);
            }
            else if (L2_event_checkEventFlag(L2_event_arqTimeout)) //if data needs to be sent (keyboard input)
            {
                debug_if(DBGMSG_L2, "[WARNING] cannot happen in IDLE state (event %i)\n", L2_event_arqTimeout);
                L2_event_clearEventFlag(L2_event_arqTimeout);
            }   
#endif
            break;

        case L2STATE_TX: //TX state description

#ifndef DISABLE_ARQ
            if (L2_event_checkEventFlag(L2_event_ackTxDone)) //data TX finished
            {
                if (L2_timer_getTimerStatus() == 1 ||
                    L2_event_checkEventFlag(L2_event_arqTimeout))
                {
                    main_state = L2STATE_ACK;
                }
                else
                {
                    main_state = L2STATE_IDLE;
                }

                L2_event_clearEventFlag(L2_event_ackTxDone);
            }
            else 
#endif
            {
                if (L2_event_checkEventFlag(L2_event_dataTxDone)) //data TX finished
                {
#ifdef DISABLE_ARQ
                    main_state = L2STATE_IDLE;
                    L3_LLI_dataCnf(1);
#else
                    if (destL2ID == L2_BROADCAST_ID)
                    {
                        main_state = L2STATE_IDLE;
                         L3_LLI_dataCnf(1);
                    }
                    else
                    {
                        main_state = L2STATE_ACK;
                        L2_timer_startTimer(); //start ARQ timer for retransmission
                    }
#endif
                    L2_event_clearEventFlag(L2_event_dataTxDone);
                }
            }

            break;

#ifndef DISABLE_ARQ
        case L2STATE_ACK: //ACK state description

            if (L2_event_checkEventFlag(L2_event_ackRcvd)) //data TX finished
            {
                uint8_t* dataPtr = L2_LLI_getRcvdDataPtr();
                if ( L2_msg_getSeq(arqPdu) == L2_msg_getSeq(dataPtr) )
                {
                    debug_if(DBGMSG_L2, "[L2] ACK is correctly received! \n");
                    L2_timer_stopTimer();
                    main_state = L2STATE_IDLE;
                    L3_LLI_dataCnf(1);
                }
                else
                {
                    debug_if(DBGMSG_L2, "[L2]ACK seq number is weird! (expected : %i, received : %i\n", L2_msg_getSeq(arqPdu),L2_msg_getSeq(dataPtr));
                }

                L2_event_clearEventFlag(L2_event_ackRcvd);
            }
            else if (L2_event_checkEventFlag(L2_event_arqTimeout)) //data TX finished
            {
                if (retxCnt >= L2_ARQ_MAXRETRANSMISSION)
                {
                    debug("[L2][WARNING] Failed to send data %i, max retx cnt reached! \n", L2_msg_getSeq(arqPdu));
                    main_state = L2STATE_IDLE;
                    L3_LLI_dataCnf(0);
                    //arqPdu clear
                    //retxCnt clear
                }
                else //retx < max, then goto TX for retransmission
                {
                    debug_if(DBGMSG_L2, "[L2] timeout! retransmit\n");
                    L2_LLI_sendData(arqPdu, pduSize, destL2ID);
                    //Setting ARQ parameter 
                    retxCnt += 1;
                    main_state = L2STATE_TX;
                }

                L2_event_clearEventFlag(L2_event_arqTimeout);
            }
            else if (L2_event_checkEventFlag(L2_event_dataRcvd)) //data TX finished
            {
                //Retrieving data info.
                uint8_t srcId = L2_LLI_getSrcId();
                uint8_t* dataPtr = L2_LLI_getRcvdDataPtr();
                uint8_t size = L2_LLI_getSize();
                uint8_t brflag = L2_LLI_getIsBroadcasted();
                uint8_t flag_end = L2_msg_checkIfEndData(dataPtr);

                //L3_LLI_dataInd(L2_msg_getWord(dataPtr), srcId, size-L2_MSG_OFFSET_DATA, L2_LLI_getSnr(), L2_LLI_getRssi());
#ifndef DISABLE_ARQ                
                if (brflag == 0 && seqNum != L2_msg_getSeq(dataPtr))
                    debug("[L3][WARNING] Invalid PDU SN (%i) while (%i) is required! discarding it...\n", L2_msg_getSeq(dataPtr), seqNum);
                else
#endif
                    L2_aggregateData(dataPtr, srcId, size, brflag, flag_end);            

#ifdef DISABLE_ARQ
                main_state = L2STATE_IDLE;
#else
                if (brflag)
                {
                    main_state = L2STATE_IDLE;
                }
                else
                {
                    //ACK transmission
                    if (brflag == 0 && seqNum == L2_msg_getSeq(dataPtr))
                        seqNum = (seqNum + 1)%L2_MSSG_MAX_SEQNUM;
                    L2_msg_encodeAck(arqAck, L2_msg_getSeq(dataPtr));
                    L2_LLI_sendData(arqAck, L2_MSG_ACKSIZE, srcId);

                    main_state = L2STATE_TX; //goto TX state
                }
#endif
                L2_event_clearEventFlag(L2_event_dataRcvd);
            }
            else if (L2_event_checkEventFlag(L2_event_dataTxDone)) //data TX finished
            {
                debug_if(DBGMSG_L2, "[L2][WARNING] cannot happen in ACK state (event %i)\n", L2_event_dataTxDone);
                L2_event_clearEventFlag(L2_event_dataTxDone);
            }
            else if (L2_event_checkEventFlag(L2_event_ackTxDone)) //data TX finished
            {
                debug_if(DBGMSG_L2, "[L2][WARNING] cannot happen in ACK state (event %i)\n", L2_event_ackTxDone);
                L2_event_clearEventFlag(L2_event_ackTxDone);
            }

            break;
#endif
        default :
            break;
    }

}