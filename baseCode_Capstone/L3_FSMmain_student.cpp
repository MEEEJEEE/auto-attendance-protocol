#include "L3_FSMevent_student.h"
#include "L3_msg.h"
#include "L3_timer.h"
#include "L3_LLinterface.h"
#include "L3_convertPacket.h"
#include "L3_chatProtocol.h"
#include "protocol_parameters.h"
#include "mbed.h"

//FSM state -------------------------------------------------
#define L3STATE_IDLE                0
#define L3STATE_ATTEND              1
#define L3STATE_LEAVE               2

// parameter
#define PRESENCE_INTERVAL_SEC       3.0f // presence 신호 송신 주기

//state variables
static uint8_t main_state = L3STATE_IDLE;
static uint8_t prev_state = main_state;

//SDU (input)
static uint8_t originalWord[1030];
static uint8_t wordLen = 0;
static uint8_t sdu[L3_MAXDATASIZE];

// outbound packet assembly buffer
static packet_data_t txPacket;

//serial port interface
static Serial pc(USBTX, USBRX);
static uint8_t myDestId;   // CU's L2 ID
static uint8_t myId;       // this student's own L2 ID

// presence 신호 송신용 ticker
static Ticker presenceTicker;

// Ticker Callback - 이벤트 플래그만 set
static void onPresenceTick(void)
{
    L3_event_setEventFlag(L3_event_periodicPresence);
}

// student <-> student chat packet parameter
#define PACKET_MODE_STUDENT_TO_STUDENT  0x03U
#define TYPE_CHAT                       0x60U


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

    // 모든 state에서 공통으로 동작하는 주기 타이머 시작
    presenceTicker.attach(&onPresenceTick, PRESENCE_INTERVAL_SEC);

    pc.printf("[IDLE] 출석 창이 열릴 때까지 대기 중입니다.\n");
}

void L3_FSMrun(void)
{
    if (prev_state != main_state)
    {
        debug_if(DBGMSG_L3, "[L3] State transition from %i to %i\n", prev_state, main_state);
        prev_state = main_state;

        if (main_state == L3STATE_ATTEND) // 채팅 시 메세지 형식 화면 출력
        {
            pc.printf("\n[CHAT FORMAT] <destId> <message>\n");
            pc.printf("example: 3 hello\n> ");
        }
    }

    // 공통: 주기적 presence 송신 (모든 state)
    // Ticker ISR이 set한 플래그를 메인루프에서 처리
    if (L3_event_checkEventFlag(L3_event_periodicPresence))
    {
        makePresencePacket(&txPacket, myId);
        L3_LLI_sendPacket(&txPacket);
        debug_if(DBGMSG_L3, "[L3] periodic presence sent (state=%i)\n", main_state);
        L3_event_clearEventFlag(L3_event_periodicPresence);
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
        // - msgRcvd     : TYPE_LEAVE_DETECTED -> LEAVE 전이 # 추가 필요!!
        //                 TYPE_ATTENDANCE_TIMEOUT(CLOSED)  -> IDLE 복귀
        //                 TYPE_ATTENDANCE_TIMEOUT(WARNING) -> 경고 출력
        //                 TYPE_CHAT                        -> 채팅 수신
        // - dataToSend  : 채팅 송신
        
        // =====================================================
        // RX
        // =====================================================
            if (L3_event_checkEventFlag(L3_event_msgRcvd))
            {
                uint8_t*       dataPtr = L3_LLI_getMsgPtr();
                uint8_t        size    = L3_LLI_getSize();

                // -------------------------------------------------
                // 1. Student <-> Student CHAT packet
                // -------------------------------------------------
                if (size >= 3) // smallest Chat header size (type + src + dest)
                {
                    chat_packet_t* chat = (chat_packet_t*)dataPtr;

                    // TYPE_CHAT 확인
                    if (chat->type == TYPE_CHAT)
                    {
                        // 내게 온 메시지만 처리
                        if (chat->dst_id == myId)
                        {
                            pc.printf("\n[CHAT] #%u: %s\n",
                                    (unsigned)chat->src_id,
                                    chat->message);
                        }

                        L3_event_clearEventFlag(L3_event_msgRcvd);
                        break;
                    }
                }
                        
                // -------------------------------------------------
                // 2. CU -> Student packet
                // -------------------------------------------------
                packet_data_t* pkt     = (packet_data_t*)dataPtr;

                switch (pkt->type_id)
                {
                    case TYPE_ATTENDANCE_APPROVAL: // 이탈 감지 신호 수신 필요!!
                    {
                        attendance_approval_t* approval =
                            (attendance_approval_t*)pkt->data;

                        if (approval->student_id != myId) break;

                        // CU가 이탈 판단 → LEAVE 전이
                        // ATTEND 상태인데 출석 실패 판정
                        if (approval->attendance_ok == 0)
                        {
                            pc.printf("[ATTEND] 이탈 감지 -> LEAVE\n");
                            main_state = L3STATE_LEAVE;
                        }
                        break;
                    }

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
                        break;
                }
                    
                L3_event_clearEventFlag(L3_event_msgRcvd);
            }

            // =====================================================
            // TX
            // =====================================================
            else if (L3_event_checkEventFlag(L3_event_dataToSend))
            {
                uint8_t destId;

                // 예:
                // 입력 형식 -> "2021 hello"
                sscanf((char*)originalWord, "%hhu %[^\t\n]",
                    &destId,
                    (char*)sdu);

                chat_packet_t chatPkt;

                makeChatPacket(&chatPkt,
                            myId,
                            destId,
                            (char*)sdu);

                L3_LLI_dataReqFunc(
                    (uint8_t*)&chatPkt,
                    sizeof(chat_packet_t),
                    destId
                );

                pc.printf("[CHAT] to #%u : %s\n",
                        (unsigned)destId,
                        chatPkt.message);

                pc.printf("> "); // TX complete 확인

                wordLen = 0;
                L3_event_clearEventFlag(L3_event_dataToSend);
            }

            break;

        // =====================================================
        case L3STATE_LEAVE:
        // 이탈 상태
        // - msgRcvd    : TYPE_RETURN_APPROVED      → ATTEND 복귀 (CU: RSSI 회복)
        //                TYPE_ATTENDANCE_TIMEOUT   → IDLE 복귀 (CU: 타이머 만료, 미출석 확정)
        // - dataToSend : X
        // =====================================================
            if (L3_event_checkEventFlag(L3_event_msgRcvd))
            {
                packet_data_t* pkt = (packet_data_t*)L3_LLI_getMsgPtr();

                debug_if(DBGMSG_L3, "[LEAVE] RCVD type=0x%02X\n", pkt->type_id);

                switch (pkt->type_id)
                {
                    case TYPE_ATTENDANCE_APPROVAL: // 복귀 허용 신호 수신 필요!!
                    {
                        attendance_approval_t* approval =
                            (attendance_approval_t*)pkt->data;

                        if (approval->student_id != myId) break;


                        // LEAVE 상태인데 다시 승인됨
                        if (approval->attendance_ok == 1)
                        {
                            pc.printf("[LEAVE] 복귀 승인 -> ATTEND\n");
                            main_state = L3STATE_ATTEND;
                        }
                        break;
                    }

                    case TYPE_ATTENDANCE_TIMEOUT: // 출석 시간 종료 -> IDLE 전이
                    {
                        attendance_timeout_t* info = (attendance_timeout_t*)pkt->data;

                        if (info->timeout_flag == TIMEOUT_FLAG_CLOSED)
                        {
                            pc.printf("[LEAVE] 미출석 확정 (CU) -> IDLE\n");
                            L3_event_clearAllEventFlag();
                            wordLen    = 0;
                            main_state = L3STATE_IDLE;
                        }
                        break;
                    }

                    default:
                        debug_if(DBGMSG_L3,
                                 "[LEAVE] unhandled type=0x%02X\n", pkt->type_id);
                    break;
                }
                L3_event_clearEventFlag(L3_event_msgRcvd);
            }
            break;

        default:
            break;
    }
}
