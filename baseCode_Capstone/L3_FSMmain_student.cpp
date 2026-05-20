#include "L3_FSMevent.h"
#include "L3_msg.h"
#include "L3_timer.h"
#include "L3_LLinterface.h"
#include "protocol_parameters.h"
#include "mbed.h"

//FSM state -------------------------------------------------
#define L3STATE_IDLE                0
#define L3STATE_ATTEND              1
#define L3STATE_LEAVE               2   // 수정: CHAT 상태 제거 (채팅 기능 CU 미경유로 변경됨)


//state variables
static uint8_t main_state = L3STATE_IDLE; //protocol state
static uint8_t prev_state = main_state;

//SDU (input)
static uint8_t originalWord[1030];
static uint8_t wordLen = 0;

// outbound packet assembly buffer
static packet_data_t txPacket;

//serial port interface
static Serial pc(USBTX, USBRX);
static uint8_t myDestId;   // CU's L2 ID
static uint8_t myId;       // this student's own L2 ID

//application event handler : generating SDU from keyboard input
static void L3service_processInputWord(void)
{
    char c = pc.getc();
    if (!L3_event_checkEventFlag(L3_event_dataToSend))
    {
        if (c == '\n' || c == '\r')
        {
            originalWord[wordLen++] = '\0';
            L3_event_setEventFlag(L3_event_dataToSend);
            debug_if(DBGMSG_L3, "word is ready! ::: %s\n", originalWord);
        }
        else
        {
            originalWord[wordLen++] = c;
            if (wordLen >= L3_MAXDATASIZE - 1)
            {
                originalWord[wordLen++] = '\0';
                L3_event_setEventFlag(L3_event_dataToSend);
                pc.printf("\n max reached! word forced to be ready :::: %s\n", originalWord);
            }
        }
    }
}


void L3_initFSM(uint8_t id, uint8_t destId)
{
    myId     = id;
    myDestId = destId;
    //initialize service layer
    pc.attach(&L3service_processInputWord, Serial::RxIrq);

    pc.printf("[IDLE] 출석 창이 열릴 때까지 대기 중입니다. Enter를 누르면 위치를 수동으로 전송합니다.\n");
}

void L3_FSMrun(void)
{
    if (prev_state != main_state)
    {
        debug_if(DBGMSG_L3, "[L3] State transition from %i to %i\n", prev_state, main_state);
        prev_state = main_state;
    }

    switch (main_state)
    {
        // =====================================================
        case L3STATE_IDLE:
        // 학생 대기 상태
        // - msgRcvd     : CU 브로드캐스트 처리
        //                 TYPE_ATTENDANCE_TIMEOUT(OPEN)    -> 위치 신호 자동 송신
        //                 TYPE_ATTENDANCE_TIMEOUT(WARNING) -> 경고 출력
        //                 TYPE_ATTENDANCE_TIMEOUT(CLOSED)  -> 마감 출력
        //                 TYPE_ATTENDANCE_APPROVAL(ok=1)   -> ATTEND 전이
        // - dataToSend  : 수동 위치 신호 송신 (Enter 입력 시)
        // =====================================================
            if (L3_event_checkEventFlag(L3_event_msgRcvd))
            {
                uint8_t*       dataPtr = L3_LLI_getMsgPtr();
                uint8_t        size    = L3_LLI_getSize();
                packet_data_t* pkt     = (packet_data_t*)dataPtr;

                (void)size;

                debug_if(DBGMSG_L3, "\n[L3][IDLE] RCVD type_id=0x%02X (len:%i)\n",
                         (unsigned)pkt->type_id, size);

                switch (pkt->type_id)
                {
                    case TYPE_ATTENDANCE_TIMEOUT:
                    {
                        attendance_timeout_t* info = (attendance_timeout_t*)pkt->data;

                        if (info->timeout_flag == TIMEOUT_FLAG_OPEN)
                        {
                            // 출석 창 열림: 위치 신호(TYPE_PRESENCE)를 CU에 자동 전송
                            // 수정: makeRSSIPacket → makePresencePacket 으로 변경
                            //       CU가 L3_LLI_getRssi()로 RSSI를 직접 읽으므로
                            //       학생은 student_id만 포함한 신호를 보내면 됨
                            pc.printf("[IDLE] 출석 창이 열렸습니다. 위치 신호를 자동 전송합니다.\n");
                            makePresencePacket(&txPacket, myId);
                            L3_LLI_sendPacket(&txPacket);
                            pc.printf("[IDLE] 위치 신호 전송 완료. 승인 대기 중...\n");
                        }
                        else if (info->timeout_flag == TIMEOUT_FLAG_WARNING)
                        {
                            // 5-minute pre-deadline warning from CU
                            pc.printf("[IDLE] ⚠ 출석 마감 5분 전입니다! 아직 미출석 상태입니다.\n");
                        }
                        else if (info->timeout_flag == TIMEOUT_FLAG_CLOSED)
                        {
                            // attendance window closed without check-in
                            pc.printf("[IDLE] 출석 창이 닫혔습니다. 결석 처리됩니다.\n");
                        }
                        break;
                    }

                    case TYPE_ATTENDANCE_APPROVAL:
                    {
                        attendance_approval_t* approval = (attendance_approval_t*)pkt->data;

                        // 내 ID로 발송된 승인 패킷인지 확인
                        // (unicast 전송이지만 L2 브로드캐스트로 잘못 수신된 경우 대비)
                        if (approval->student_id != myId)
                        {
                            debug_if(DBGMSG_L3,
                                     "[L3][IDLE] approval for student %i, I am %i, ignoring.\n",
                                     approval->student_id, myId);
                            break;
                        }

                        if (approval->attendance_ok == 1)
                        {
                            // CU approved attendance -> advance to ATTEND
                            pc.printf("[IDLE] 승인 수신 -> ATTEND 상태로 이동\n");
                            pc.printf("[ATTEND] 출석이 확인되었습니다. 'LEAVE' 입력 시 종료합니다.\n");
                            main_state = L3STATE_ATTEND;
                        }
                        else
                        {
                            // CU rejected (RSSI below threshold -> student is outside)
                            pc.printf("[IDLE] 출석 거부 (신호 부족). 다시 시도하려면 Enter를 누르세요.\n");
                        }
                        break;
                    }

                    default:
                        debug_if(DBGMSG_L3,
                                 "[L3][IDLE] unhandled type_id=0x%02X, ignoring.\n",
                                 (unsigned)pkt->type_id);
                        break;
                }

                L3_event_clearEventFlag(L3_event_msgRcvd);
            }
            else if (L3_event_checkEventFlag(L3_event_dataToSend))
            {
                // 수동 위치 신호 전송: Enter 입력 시 즉시 CU에 위치 신호 전송
                // 수정: makeRSSIPacket → makePresencePacket 으로 변경 (rssi 파라미터 제거)
                makePresencePacket(&txPacket, myId);
                debug_if(DBGMSG_L3, "[L3][IDLE] sending presence signal (len:%i)\n",
                         (int)sizeof(packet_data_t));
                L3_LLI_sendPacket(&txPacket);

                pc.printf("[IDLE] 위치 정보 전송 완료. 승인 대기 중...\n");
                wordLen = 0;

                L3_event_clearEventFlag(L3_event_dataToSend);
            }
            break;

        // =====================================================
        case L3STATE_ATTEND:
        // 출석 확인 완료 상태
        // - msgRcvd     : TYPE_ATTENDANCE_TIMEOUT(CLOSED)  -> IDLE 복귀
        //                 TYPE_ATTENDANCE_TIMEOUT(WARNING) -> 경고 출력
        // - dataToSend  : 'LEAVE' 입력 시 LEAVE 상태로 이동
        // 수정: TYPE_CHAT_MESSAGE case 제거 (채팅은 CU 경유 불필요 → 직접 통신)
        //       makeChatPacket 호출 제거 (L3_convertPacket.h에서 삭제됨)
        // =====================================================
            if (L3_event_checkEventFlag(L3_event_msgRcvd))
            {
                uint8_t*       dataPtr = L3_LLI_getMsgPtr();
                uint8_t        size    = L3_LLI_getSize();
                packet_data_t* pkt     = (packet_data_t*)dataPtr;

                (void)size;

                debug_if(DBGMSG_L3, "\n[L3][ATTEND] RCVD type_id=0x%02X (len:%i)\n",
                         (unsigned)pkt->type_id, size);

                switch (pkt->type_id)
                {
                    case TYPE_ATTENDANCE_TIMEOUT:
                    {
                        attendance_timeout_t* info = (attendance_timeout_t*)pkt->data;

                        if (info->timeout_flag == TIMEOUT_FLAG_CLOSED)
                        {
                            pc.printf("[ATTEND] 출석 창이 닫혔습니다. IDLE로 복귀합니다.\n");
                            L3_event_clearAllEventFlag();
                            wordLen = 0;
                            main_state = L3STATE_IDLE;
                        }
                        else if (info->timeout_flag == TIMEOUT_FLAG_WARNING)
                        {
                            pc.printf("[ATTEND] ⚠ 출석 마감 5분 전입니다!\n");
                        }
                        break;
                    }

                    default:
                        debug_if(DBGMSG_L3,
                                 "[L3][ATTEND] unhandled type_id=0x%02X, ignoring.\n",
                                 (unsigned)pkt->type_id);
                        break;
                }

                L3_event_clearEventFlag(L3_event_msgRcvd);
            }
            else if (L3_event_checkEventFlag(L3_event_dataToSend))
            {
                if (strcmp((char*)originalWord, "LEAVE") == 0)
                {
                    // 이탈 요청: LEAVE 상태로 전환
                    pc.printf("[ATTEND] LEAVE 입력 -> LEAVE 상태로 이동\n");
                    wordLen = 0;
                    L3_event_clearEventFlag(L3_event_dataToSend);
                    main_state = L3STATE_LEAVE;
                }
                else
                {
                    // 수정: 채팅 기능 제거로 인해 일반 텍스트 입력은 무시
                    // (이전: makeChatPacket으로 채팅 요청 전송, 현재: 해당 패킷 타입 없음)
                    pc.printf("[ATTEND] 출석 확인 완료 상태입니다. 종료하려면 'LEAVE'를 입력하세요.\n");
                    wordLen = 0;
                    L3_event_clearEventFlag(L3_event_dataToSend);
                }
            }
            break;

        // =====================================================
        case L3STATE_LEAVE:
        // 이탈 상태
        // 수정: makeStudentLeavePacket 제거 (L3_convertPacket.h에서 해당 타입 삭제됨)
        //       패킷 전송 없이 IDLE로 복귀 (출석 기록은 CU에 이미 저장됨)
        // =====================================================
        {
            debug_if(DBGMSG_L3, "[L3][LEAVE] leaving, returning to IDLE\n");
            pc.printf("[LEAVE] 이탈 처리 완료. IDLE로 복귀합니다.\n");
            pc.printf("[IDLE] 출석 창이 열릴 때까지 대기 중입니다.\n");

            L3_event_clearAllEventFlag();
            wordLen = 0;
            main_state = L3STATE_IDLE;
            break;
        }

        default:
            break;
    }
}
