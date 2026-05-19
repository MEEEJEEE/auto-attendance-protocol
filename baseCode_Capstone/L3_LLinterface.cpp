#include "mbed.h"
#include "L3_FSMevent.h"
#include "L3_msg.h"
#include "L3_convertPacket.h"
#include "protocol_parameters.h"
#include "time.h"

# define CU_ID 0

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
    uint8_t destId = 0;

    // -----------------------------
    // Routing decision (ONLY here)
    // -----------------------------
    switch (pkt->mode)
    {
        // Student → CU (always fixed destination)
        case PACKET_MODE_STUDENT_TO_CU:
        {
            destId = CU_ID;
            break;
        }

        // CU → Student(s)
        case PACKET_MODE_CU_TO_STUDENT:
        {
            // 수정: 패킷 타입에 따라 브로드캐스트/유니캐스트 분기
            // - TYPE_ATTENDANCE_TIMEOUT  : 전체 브로드캐스트 (모든 학생 대상)
            // - TYPE_ATTENDANCE_APPROVAL : 특정 학생에게 유니캐스트
            //   (이전 버그: presence_t*로 캐스팅해 student_id를 읽었으나
            //    approval/timeout 패킷에는 presence_t 구조체가 없어 잘못된 값 참조)
            if (pkt->type_id == TYPE_ATTENDANCE_TIMEOUT)
            {
                // 출석 시간 알림(OPEN/WARNING/CLOSED)은 전체 브로드캐스트
                destId = L3_BROADCAST_ID;
            }
            else if (pkt->type_id == TYPE_ATTENDANCE_APPROVAL)
            {
                // 출석 승인 패킷은 대상 학생에게만 유니캐스트
                // attendance_approval_t.student_id 필드로 목적지 결정
                attendance_approval_t* approval = (attendance_approval_t*)pkt->data;
                destId = approval->student_id;
            }
            else
            {
                // 알 수 없는 CU→Student 타입: 안전하게 브로드캐스트로 폴백
                debug_if(DBGMSG_L3,
                         "[L3] unknown CU_TO_STUDENT type_id 0x%02X, fallback to broadcast\n",
                         (unsigned)pkt->type_id);
                destId = L3_BROADCAST_ID;
            }
            break;
        }

        default:
        {
            debug_if(DBGMSG_L3,
                     "[L3] invalid packet mode: %lu\n",
                     (unsigned long)pkt->mode);
            return;
        }
    }

    // -----------------------------
    // Debug info
    // -----------------------------
    debug_if(DBGMSG_L3,
             "[L3] SEND type:%lu mode:%lu dest:%u\n",
             (unsigned long)pkt->type_id,
             (unsigned long)pkt->mode,
             destId);

    // -----------------------------
    // Send down to L2
    // -----------------------------
    L3_LLI_dataReqFunc((uint8_t*)pkt,
                       sizeof(packet_data_t),
                       destId);
}