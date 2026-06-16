// ============================================================
// [실습 파일] L2_FSMmain.cpp  ← Layer 2 ARQ 상태머신
//
// 이 파일은 LoRa 무선 통신의 Layer 2(데이터링크 계층)를 구현합니다.
// ARQ(Automatic Repeat reQuest)와 SN(Sequence Number)으로
// 패킷 손실을 감지하고 자동으로 재전송합니다.
//
// ── ARQ 동작 원리 ──────────────────────────────────────────
//
//   송신측                         수신측
//     │                               │
//     │──── 데이터(SN=N) ────────────▶│  SN 확인
//     │                               │──── ACK(SN=N) ────▶│
//     │  ACK 수신 → 다음 SN           │
//     │                               │
//     │──── 데이터(SN=N) ────────────▶│  (타임아웃 시 재전송)
//
// ── 상태 구조 ──────────────────────────────────────────────
//
//   IDLE  ──(데이터 전송 요청)──▶  TX  ──(TX 완료)──▶  ACK
//     ▲                                                   │
//     └───────────────────────── (ACK 수신 / 최대 재전송) ┘
//
// ── 실습 포인트 ────────────────────────────────────────────
//   1. seqNum: 각 패킷에 붙는 순서 번호. 중복/손실 감지에 사용.
//   2. retxCnt: 재전송 횟수. L2_ARQ_MAXRETRANSMISSION 초과 시 포기.
//   3. DISABLE_ARQ: Makefile에서 이 플래그를 켜면 ARQ 없이 동작 (테스트용)
//   4. Force-resync: SN 불일치 시 강제 동기화 로직 (아래 주석 참고)
// ============================================================

#include "L2_FSMevent.h"
#include "L2_msg.h"
#include "L2_timer.h"
#include "L2_LLinterface.h"
#include "L3_LLinterface.h"
#include "protocol_parameters.h"

// ============================================================
// [FSM 상태 정의]
// ============================================================
#define L2STATE_IDLE              0  // 대기: 데이터 수신 또는 전송 요청 대기
#define L2STATE_TX                1  // 전송 중: 데이터 또는 ACK 전송 완료 대기
#ifndef DISABLE_ARQ
#define L2STATE_ACK               2  // ACK 대기: 상대방의 ACK 수신 대기 (재전송 타이머 동작 중)
#endif

#define SDUBUFFER_SIZE              1024  // 대용량 데이터 분할 전송 버퍼 크기

// ============================================================
// [상태/ID 변수]
// ============================================================
static uint8_t main_state = L2STATE_IDLE; // 현재 FSM 상태
static uint8_t prev_state = main_state;   // 이전 상태 (전이 감지용)

static uint8_t myL2ID=1;    // 자신의 L2 노드 ID
static uint8_t destL2ID=0;  // 목적지 L2 노드 ID (전송 시마다 업데이트)

// ============================================================
// [PDU/SDU 버퍼]
// SDU(Service Data Unit): L3에서 내려온 원본 데이터
// PDU(Protocol Data Unit): L2 헤더(SN 등)가 붙은 전송 단위
// ============================================================
static uint8_t sduBuffer[SDUBUFFER_SIZE]; // 대용량 SDU 임시 저장 버퍼
static uint8_t sduBufferSize;             // 버퍼에 남아있는 데이터 크기

static uint8_t arqPdu[200];  // ARQ 전송/재전송용 PDU 버퍼 (헤더 + 데이터)
static uint8_t sduIn[200];   // 현재 처리 중인 SDU
static uint8_t pduSize;      // 현재 PDU 크기
static uint8_t sduLen;       // 현재 SDU 크기

static uint8_t pduBuffer[SDUBUFFER_SIZE]; // 분할 수신된 PDU 조립 버퍼
static uint8_t pduBufferSize;             // 조립된 데이터 크기

// ============================================================
// [ARQ 제어 변수]
//
// seqNum : 시퀀스 번호 (0~255 순환)
//          - 송신 시: 전송 패킷에 현재 SN을 붙이고 SN+1
//          - 수신 시: 받은 패킷의 SN이 예상 SN과 일치하는지 확인
//          - 불일치 시: Force-resync로 강제 동기화 (아래 참고)
// retxCnt: 재전송 횟수 누적. L2_ARQ_MAXRETRANSMISSION 도달 시 포기.
// arqAck : ACK 패킷 버퍼 (SN만 담은 소형 패킷)
// ============================================================
static uint8_t seqNum = 0;  // ARQ 시퀀스 번호 (0~255 순환, L2_msg.h의 L2_MSSG_MAX_SEQNUM)
#ifndef DISABLE_ARQ
static uint8_t retxCnt = 0; // 현재 패킷 재전송 횟수
static uint8_t arqAck[5];   // ACK PDU 버퍼
#define L2_BROADCAST_ID             255  // 브로드캐스트 목적지 ID (ACK 없음)
#endif
static uint8_t reqestedId=0; // ID 재설정 요청값

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
    // Fix: update destL2ID BEFORE the validity check.
    // Previously destL2ID was only written on success, so for the CU node
    // (myL2ID=0, initial destL2ID=0) the very first call always failed the
    // check (0==0) and left destL2ID=0, causing all outbound packets to be
    // addressed to the CU itself instead of the intended destination.
    destL2ID = destId;

    if (L2_validityCheck_ID() == 1)
    {
        debug("[L2] Warning: dest ID same as my ID (%i)\n", destId);
        return 1;
    }

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
    destL2ID = 0; // default destination (overwritten by L2_connect())

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


// ============================================================
// [메인 FSM 실행 함수] L2_FSMrun()
//
// main.cpp의 while(1)에서 매 반복마다 호출됩니다.
// 이벤트 플래그를 확인하여 ARQ 상태 기계를 구동합니다.
// ============================================================
void L2_FSMrun(void)
{
    // 상태 전이 감지 시 디버그 로그 출력
    if (prev_state != main_state)
    {
        debug_if(DBGMSG_L2, "[L2] State transition from %i to %i\n", prev_state, main_state);
        prev_state = main_state;
    }

    switch (main_state)
    {
        // =====================================================
        // [상태 0: IDLE - 대기 상태]
        //
        // 처리하는 이벤트 (우선순위 순):
        //   L2_event_reconfigSrcId  : ID 재설정 요청
        //   L2_event_dataRcvd       : 하위 계층(LoRa)에서 데이터 수신
        //   L2_event_dataToSend     : L3에서 데이터 전송 요청
        //   L2_event_dataToSendBuffer: 버퍼에 남은 데이터 전송
        // =====================================================
        case L2STATE_IDLE:

            if (L2_event_checkEventFlag(L2_event_reconfigSrcId)) // ID 재설정 요청
            {
                int res;
                res = L2_LLI_configSrcId(reqestedId);

                L3_LLI_reconfigSrcIdCnf(res==0);
                main_state = L2STATE_IDLE; //goto TX state
                L2_event_clearEventFlag(L2_event_reconfigSrcId);
            }
            else if (L2_event_checkEventFlag(L2_event_dataRcvd)) // 하위 계층에서 데이터 수신
            {
                // 수신 패킷 정보 추출
#ifndef DISABLE_ARQ
                uint8_t srcId = L2_LLI_getSrcId(); // 송신자 ID (ACK 전송 시 목적지로 사용)
#endif
                uint8_t* dataPtr = L2_LLI_getRcvdDataPtr(); // 수신된 PDU 포인터
                uint8_t size = L2_LLI_getSize();            // PDU 크기
                uint8_t brflag = L2_LLI_getIsBroadcasted(); // 브로드캐스트 여부 (1=브로드캐스트, ACK 불필요)
                uint8_t flag_end = L2_msg_checkIfEndData(dataPtr); // 마지막 분할 패킷 여부

#ifndef DISABLE_ARQ
                // ─────────────────────────────────────────────────────
                // [Force-resync: SN 강제 동기화]
                //
                // 문제 상황:
                //   LoRa 무선 환경에서 ACK가 손실되면 송신측은 SN을 증가시키고
                //   재전송하지만, 수신측은 이전 SN을 기대하고 있어 영구 desync 발생.
                //   기존 방식(불일치 시 폐기 + ACK 미전송)은 송신측이 포기할 때까지
                //   재전송을 반복하게 만들어 채널을 낭비하고 통신 단절 유발.
                //
                // 해결 방법:
                //   수신된 SN으로 seqNum을 강제로 업데이트하여 동기화 복구.
                //   이후 정상적으로 ACK를 전송하여 통신을 재개합니다.
                //
                // 실습 포인트:
                //   DBGMSG_L2=1로 빌드하면 "[L2][WARN] SN mismatch" 메시지로 확인 가능
                // ─────────────────────────────────────────────────────
                if (brflag == 0 && seqNum != L2_msg_getSeq(dataPtr))
                {
                    debug("[L2][WARN] SN mismatch (got %i, expected %i). Force-resync.\n",
                          L2_msg_getSeq(dataPtr), seqNum);
                    seqNum = L2_msg_getSeq(dataPtr); // 수신 SN으로 강제 동기화
                }
#endif
                // 수신 데이터를 PDU 버퍼에 조립하고 L3로 전달
                L2_aggregateData(dataPtr, srcId, size, brflag, flag_end);

#ifdef DISABLE_ARQ
                main_state = L2STATE_IDLE; // ARQ 비활성화 시 즉시 IDLE 복귀
#else
                if (brflag)
                {
                    // 브로드캐스트는 ACK 없이 IDLE로 복귀
                    main_state = L2STATE_IDLE;
                }
                else
                {
                    // 유니캐스트: ACK 패킷 전송 후 TX 상태로 전이
                    // resync 후에는 seqNum == getSeq(dataPtr)가 보장됨
                    if (brflag == 0 && seqNum == L2_msg_getSeq(dataPtr))
                        seqNum = (seqNum + 1) % L2_MSSG_MAX_SEQNUM; // 다음 기대 SN으로 증가
                    L2_msg_encodeAck(arqAck, L2_msg_getSeq(dataPtr)); // ACK 패킷 조립
                    L2_LLI_sendData(arqAck, L2_MSG_ACKSIZE, srcId);   // 송신자에게 ACK 전송

                    main_state = L2STATE_TX; // TX 완료 대기 상태로 전이
                }
#endif
                L2_event_clearEventFlag(L2_event_dataRcvd);
            }
            else if (L2_event_checkEventFlag(L2_event_dataToSend)) // L3에서 데이터 전송 요청
            {
                // [데이터 전송]
                // L2 헤더(SN 등)를 붙여 PDU로 인코딩한 후 하위 계층(LoRa)으로 전송
                pduSize = L2_msg_encodeData(arqPdu, sduIn, seqNum, sduLen,
                    L2_event_checkEventFlag(L2_event_dataToSendBuffer) == 0); // PDU 인코딩
                L2_LLI_sendData(arqPdu, pduSize, destL2ID); // LoRa 전송

#ifndef DISABLE_ARQ
                // 유니캐스트 전송 시 SN 증가 및 재전송 카운터 초기화
                // 브로드캐스트(255)는 ACK가 없으므로 SN 증가 불필요
                if (destL2ID != L2_BROADCAST_ID)
                    seqNum = (seqNum + 1)%L2_MSSG_MAX_SEQNUM; // SN 증가 (0~255 순환)
                retxCnt = 0; // 재전송 카운터 초기화
#endif
                debug_if(DBGMSG_L2, "[L2] sending to %i (seq:%i)\n", destL2ID, (seqNum-1)%L2_MSSG_MAX_SEQNUM);

                main_state = L2STATE_TX; // TX 완료 대기 상태로 전이

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
            // 복사-붙여넣기 오류 블록 제거:
            // dataTxDone을 체크하면서 ackRcvd를 삭제하던 잘못된 코드
            // dataTxDone은 261번 줄에서 이미 처리됨 (중복 + ackRcvd 오삭제 버그)
            else if (L2_event_checkEventFlag(L2_event_arqTimeout)) //if data needs to be sent (keyboard input)
            {
                debug_if(DBGMSG_L2, "[WARNING] cannot happen in IDLE state (event %i)\n", L2_event_arqTimeout);
                L2_event_clearEventFlag(L2_event_arqTimeout);
            }   
#endif
            break;

        // =====================================================
        // [상태 1: TX - 전송 중 상태]
        //
        // 데이터 또는 ACK 전송이 완료되길 기다리는 상태입니다.
        //
        // 처리하는 이벤트:
        //   L2_event_ackTxDone  : ACK 전송 완료 → IDLE 또는 ACK 대기로 전이
        //   L2_event_dataTxDone : 데이터 전송 완료 → 브로드캐스트면 IDLE, 유니캐스트면 ACK 대기
        // =====================================================
        case L2STATE_TX:

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
        // =====================================================
        // [상태 2: ACK - ACK 수신 대기 상태]
        //
        // 데이터를 전송한 후 상대방의 ACK를 기다리는 상태입니다.
        // ARQ 타이머가 동작 중이며, 타임아웃 시 재전송합니다.
        //
        // 처리하는 이벤트:
        //   L2_event_ackRcvd    : ACK 수신 → SN 확인 후 IDLE 복귀
        //   L2_event_arqTimeout : 타이머 만료 → 재전송 또는 포기
        //   L2_event_dataRcvd   : ACK 대기 중 다른 데이터 수신 (처리 후 ACK 전송)
        //
        // 실습 포인트:
        //   "[L2][WARNING] Failed to send data" 메시지가 뜨면
        //   L2_ARQ_MAXRETRANSMISSION 횟수만큼 재전송하다 포기한 것입니다.
        //   → 무선 환경 불량 또는 상대방이 꺼져있는 경우
        // =====================================================
        case L2STATE_ACK:

            if (L2_event_checkEventFlag(L2_event_ackRcvd)) // ACK 수신 이벤트
            {
                uint8_t* dataPtr = L2_LLI_getRcvdDataPtr();
                // ACK의 SN이 전송한 패킷의 SN과 일치하는지 확인
                if ( L2_msg_getSeq(arqPdu) == L2_msg_getSeq(dataPtr) )
                {
                    // 올바른 ACK 수신 → 전송 성공 확인, IDLE 복귀
                    debug_if(DBGMSG_L2, "[L2] ACK is correctly received! \n");
                    L2_timer_stopTimer(); // 재전송 타이머 정지
                    main_state = L2STATE_IDLE;
                    L3_LLI_dataCnf(1); // L3에 전송 성공 알림
                }
                else
                {
                    // SN 불일치 ACK 수신 → 무시 (재전송 타이머 계속 동작)
                    debug_if(DBGMSG_L2, "[L2]ACK seq number is weird! (expected : %i, received : %i\n", L2_msg_getSeq(arqPdu),L2_msg_getSeq(dataPtr));
                }

                L2_event_clearEventFlag(L2_event_ackRcvd);
            }
            else if (L2_event_checkEventFlag(L2_event_arqTimeout)) // ARQ 타이머 만료
            {
                if (retxCnt >= L2_ARQ_MAXRETRANSMISSION)
                {
                    // 최대 재전송 횟수 초과 → 전송 포기
                    // protocol_parameters.h의 L2_ARQ_MAXRETRANSMISSION 값으로 제어
                    debug("[L2][WARNING] Failed to send data %i, max retx cnt reached! \n", L2_msg_getSeq(arqPdu));
                    main_state = L2STATE_IDLE;
                    L3_LLI_dataCnf(0); // L3에 전송 실패 알림
                }
                else // 재전송 횟수 남음 → 동일 패킷 재전송
                {
                    debug_if(DBGMSG_L2, "[L2] timeout! retransmit\n");
                    L2_LLI_sendData(arqPdu, pduSize, destL2ID); // 동일 PDU 재전송
                    retxCnt += 1;      // 재전송 카운터 증가
                    main_state = L2STATE_TX;
                }

                L2_event_clearEventFlag(L2_event_arqTimeout);
            }
            else if (L2_event_checkEventFlag(L2_event_dataRcvd)) // ACK 대기 중 데이터 수신
            {
                // ACK를 기다리는 중에 상대방이 데이터를 보낸 경우
                // 데이터를 처리하고 ACK를 전송합니다 (동시 통신 지원)
                //Retrieving data info.
                uint8_t srcId = L2_LLI_getSrcId();
                uint8_t* dataPtr = L2_LLI_getRcvdDataPtr();
                uint8_t size = L2_LLI_getSize();
                uint8_t brflag = L2_LLI_getIsBroadcasted();
                uint8_t flag_end = L2_msg_checkIfEndData(dataPtr);

                //L3_LLI_dataInd(L2_msg_getWord(dataPtr), srcId, size-L2_MSG_OFFSET_DATA, L2_LLI_getSnr(), L2_LLI_getRssi());
#ifndef DISABLE_ARQ
                // IDLE 상태와 동일한 강제 resync 정책 적용
                if (brflag == 0 && seqNum != L2_msg_getSeq(dataPtr))
                {
                    debug("[L2][WARN] SN mismatch in ACK state (got %i, expected %i). Force-resync.\n",
                          L2_msg_getSeq(dataPtr), seqNum);
                    seqNum = L2_msg_getSeq(dataPtr); // 수신 SN으로 강제 동기화
                }
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