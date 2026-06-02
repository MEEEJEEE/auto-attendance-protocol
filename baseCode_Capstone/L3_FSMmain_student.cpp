// ============================================================
// [실습 파일] L3_FSMmain_student.cpp  ← 학생 단말 L3 FSM 메인
//
// 이 파일은 학생 단말의 Layer 3(응용 계층) 유한 상태 기계(FSM)를 구현합니다.
//
// ── 상태 구조 ──────────────────────────────────────────────
//
//   IDLE  ──(출석창 OPEN + PRESENCE 전송)──▶  ATTEND
//     ▲                                           │
//     │  (출석창 CLOSED)                          │ (이탈 감지)
//     └───────────────────────────────────────  LEAVE
//                                                 │ (복귀 승인)
//                                                 └──────▶ ATTEND
//
// ── 실습 시 주목할 부분 ────────────────────────────────────
//   1. 각 case L3STATE_XXX 블록: 해당 상태에서 처리하는 이벤트
//   2. L3_event_checkEventFlag(): 어떤 이벤트가 왔는지 확인
//   3. makePresencePacket() / makeChatPacket(): 전송 패킷 생성
//   4. main_state = L3STATE_XXX: 상태 전이(화살표) 구현 위치
// ============================================================

#include "L3_FSMevent_student.h"
#include "L3_msg.h"
#include "L3_timer.h"
#include "L3_LLinterface.h"
#include "L3_convertPacket.h"
#include "L3_chatProtocol.h"
#include "protocol_parameters.h"
#include "mbed.h"

// ============================================================
// [FSM 상태 정의]
// 학생 단말은 아래 세 가지 상태 중 하나에 항상 존재합니다.
// ============================================================
#define L3STATE_IDLE                0   // 대기 상태: 출석 창이 열리길 기다림
#define L3STATE_ATTEND              1   // 출석 완료 상태: 강의실 안에 있음 (채팅 가능)
#define L3STATE_LEAVE               2   // 이탈 상태: 출석 후 강의실 밖으로 나간 것으로 판단됨

// ============================================================
// [presence 신호 주기]
// 학생 단말은 이 간격(초)마다 자동으로 CU에 위치 신호(PRESENCE)를 전송합니다.
// 실습: 이 값을 줄이면 더 자주 전송 → CU RSSI 평균이 빨리 수집됨
// ============================================================
#define PRESENCE_INTERVAL_SEC       3.0f // presence 신호 자동 송신 주기 (초)

// ============================================================
// [전역 변수]
// ============================================================

// 현재 FSM 상태 및 이전 상태 (상태 전이 감지용)
static uint8_t main_state = L3STATE_IDLE;
static uint8_t prev_state = main_state;

// 키보드 입력 버퍼: 시리얼로 들어온 문자를 한 줄 단위로 누적
static uint8_t originalWord[1030]; // 입력 문자열 버퍼
static uint8_t wordLen = 0;        // 현재 버퍼에 저장된 문자 수
static uint8_t sdu[L3_MAXDATASIZE]; // 채팅 메시지 본문 임시 저장

// 송신 패킷 조립 버퍼 (PRESENCE, 채팅 등 모든 송신 패킷 공용)
static packet_data_t txPacket;

// 시리얼 포트 (PC와 USB 시리얼 통신)
static Serial pc(USBTX, USBRX);
static uint8_t myDestId;   // 목적지 ID: CU의 L2 ID (기본값 0)
static uint8_t myId;       // 자신의 L2 ID (main.cpp에서 input_thisId로 설정)

// ============================================================
// [Ticker: 주기적 PRESENCE 전송]
// Ticker는 지정한 시간마다 콜백을 자동 호출하는 mbed 타이머입니다.
// ISR(인터럽트 서비스 루틴) 내부이므로 printf 호출 금지 → 플래그만 set
// ============================================================
static Ticker presenceTicker; // PRESENCE_INTERVAL_SEC 마다 자동 발화

// [Ticker 콜백] ISR이므로 이벤트 플래그 set만 수행
// 실제 패킷 전송은 메인 루프(L3_FSMrun)에서 플래그를 확인하고 처리
static void onPresenceTick(void)
{
    // 주기 도래 이벤트 플래그 set → 메인 루프에서 PRESENCE 패킷 전송
    L3_event_setEventFlag(L3_event_periodicPresence);
}

// ============================================================
// [채팅 패킷 타입 상수]
// L3_chatProtocol.h에도 동일하게 정의되어 있습니다.
// 학생 ↔ 학생 직접 채팅 패킷을 식별하는 데 사용합니다.
// ============================================================
#define PACKET_MODE_STUDENT_TO_STUDENT  0x03U  // 채팅 패킷 모드 식별자
#define TYPE_CHAT                       0x60U  // 채팅 패킷 타입 식별자


// ============================================================
// [시리얼 RX ISR] 키보드 입력 누적
//
// Serial::RxIrq 에 등록된 인터럽트 핸들러입니다.
// 문자가 들어올 때마다 호출되며, Enter(\r 또는 \n)가 오면
// L3_event_dataToSend 플래그를 set하여 메인 루프에 알립니다.
//
// ★ 주의: ISR 내부에서 pc.printf() 호출 시 mbed에서 하드폴트 발생!
//          ISR에서는 반드시 플래그 set만 수행하고,
//          실제 출력은 L3_FSMrun() 메인 루프에서 처리합니다.
// ============================================================
static void L3service_processInputWord(void)
{
    char c = pc.getc(); // 수신된 문자 1개 읽기

    // 이전 입력이 아직 처리 중이면 새 입력 무시 (오버런 방지)
    if (!L3_event_checkEventFlag(L3_event_dataToSend))
    {
        if (c == '\n' || c == '\r')
        {
            // Enter 키: 입력 완료 → 문자열 종료 후 이벤트 플래그 set
            originalWord[wordLen++] = '\0';
            L3_event_setEventFlag(L3_event_dataToSend); // 메인 루프에 데이터 준비 알림
            debug_if(DBGMSG_L3, "word is ready! ::: %s\n", originalWord);
        }
        else
        {
            // 일반 문자: 버퍼에 누적
            originalWord[wordLen++] = c;
            if (wordLen >= L3_MAXDATASIZE - 1)
            {
                // 버퍼 가득 참: 강제로 입력 완료 처리
                // ISR 내부에서 printf 호출은 mbed에서 하드폴트를 유발할 수 있으므로
                // 플래그 설정만 수행하고 printf는 메인루프에서 처리함
                originalWord[wordLen++] = '\0';
                L3_event_setEventFlag(L3_event_dataToSend);
            }
        }
    }
}


// ============================================================
// [초기화 함수] L3_initFSM()
//
// main.cpp에서 한 번 호출합니다.
// - 자신의 ID(myId)와 CU ID(myDestId) 저장
// - 시리얼 RX 인터럽트 핸들러 등록
// - 주기적 PRESENCE 전송 Ticker 시작
// ============================================================
void L3_initFSM(uint8_t id, uint8_t destId)
{
    myId     = id;       // 자신의 학생 ID (main.cpp의 input_thisId)
    myDestId = destId;   // CU의 ID (main.cpp의 input_destId = 0)

    // 키보드 입력을 ISR로 처리 (문자가 들어올 때마다 자동 호출)
    pc.attach(&L3service_processInputWord, Serial::RxIrq);

    // PRESENCE_INTERVAL_SEC(3초)마다 onPresenceTick() 자동 호출 시작
    // → 메인 루프에서 L3_event_periodicPresence 이벤트 감지 후 패킷 전송
    presenceTicker.attach(&onPresenceTick, PRESENCE_INTERVAL_SEC);

    pc.printf("[IDLE] 출석 창이 열릴 때까지 대기 중입니다.\n");
}

// ============================================================
// [메인 FSM 실행 함수] L3_FSMrun()
//
// main.cpp의 while(1) 루프에서 매 반복마다 호출됩니다.
// 이벤트 플래그를 확인하고 현재 상태에 맞는 동작을 수행합니다.
//
// 실습 포인트:
//   - 상태 전이는 "main_state = L3STATE_XXX;" 줄에서 발생
//   - 이벤트는 L3_event_checkEventFlag()로 확인
//   - 패킷 수신: L3_event_msgRcvd
//   - 키보드 입력 완료: L3_event_dataToSend
//   - 3초 주기 타이머: L3_event_periodicPresence
// ============================================================
void L3_FSMrun(void)
{
    // [상태 전이 감지] 상태가 바뀌면 디버그 로그 출력 및 진입 동작 수행
    if (prev_state != main_state)
    {
        debug_if(DBGMSG_L3, "[L3] State transition from %i to %i\n", prev_state, main_state);
        prev_state = main_state;

        if (main_state == L3STATE_ATTEND) // ATTEND 상태 진입 시 채팅 사용법 안내
        {
            pc.printf("\n[CHAT FORMAT] <destId> <message>\n");
            pc.printf("example: 3 hello\n> ");
        }
    }

    // ============================================================
    // [공통 이벤트] 주기적 PRESENCE 신호 전송
    // 모든 상태에서 3초마다 CU에 위치 신호를 자동 전송합니다.
    // CU는 이 신호의 RSSI를 측정하여 학생의 강의실 내 위치를 판단합니다.
    //
    // 실습 포인트: PRESENCE 패킷 구조는 L3_convertPacket.h의 makePresencePacket() 참고
    // ============================================================
    if (L3_event_checkEventFlag(L3_event_periodicPresence))
    {
        makePresencePacket(&txPacket, myId); // PRESENCE 패킷 조립 (학생 ID 포함)
        L3_LLI_sendPacket(&txPacket);        // L2를 통해 CU로 전송
        debug_if(DBGMSG_L3, "[L3] periodic presence sent (state=%i)\n", main_state);
        L3_event_clearEventFlag(L3_event_periodicPresence); // 처리 완료 후 플래그 클리어
    }

    switch (main_state)
    {
        // =====================================================
        // [상태 0: IDLE - 대기 상태]
        //
        // 출석 창이 열리기를 기다리는 초기 상태입니다.
        //
        // 처리하는 이벤트:
        //   L3_event_msgRcvd  (CU로부터 패킷 수신 시)
        //     └─ TYPE_ATTENDANCE_TIMEOUT(OPEN)    → 출석 창 열림: PRESENCE 자동 전송
        //     └─ TYPE_ATTENDANCE_TIMEOUT(WARNING) → 마감 5분 전 경고 출력
        //     └─ TYPE_ATTENDANCE_TIMEOUT(CLOSED)  → 출석 창 닫힘 출력
        //     └─ TYPE_ATTENDANCE_APPROVAL(ok=1)   → 승인 → ATTEND 상태로 전이
        //     └─ TYPE_ATTENDANCE_APPROVAL(ok=0)   → 거부 → 다시 시도 요청
        //   L3_event_dataToSend (Enter 입력 시)
        //     └─ PRESENCE 패킷을 수동으로 CU에 전송
        //
        // 실습 포인트: 이 상태에서 Enter를 눌러 수동 PRESENCE 전송 가능
        // =====================================================
        case L3STATE_IDLE:
            if (L3_event_checkEventFlag(L3_event_msgRcvd))
            {
                uint8_t*       dataPtr = L3_LLI_getMsgPtr();
                uint8_t        size    = L3_LLI_getSize();
                packet_data_t* pkt     = (packet_data_t*)dataPtr;

                (void)size;

                debug_if(DBGMSG_L3, "\n[L3][IDLE] RCVD type_id=0x%02X (len:%i)\n",
                         (unsigned)pkt->type_id, size);

                // [IDLE] 수신 패킷의 type_id에 따라 분기 처리
                switch (pkt->type_id)
                {
                    // ─────────────────────────────────────────────
                    // TYPE_ATTENDANCE_TIMEOUT: CU가 보낸 타임아웃 알림 패킷
                    // timeout_flag 값으로 세부 상황을 구분합니다.
                    // ─────────────────────────────────────────────
                    case TYPE_ATTENDANCE_TIMEOUT:
                    {
                        attendance_timeout_t* info = (attendance_timeout_t*)pkt->data;

                        if (info->timeout_flag == TIMEOUT_FLAG_OPEN)
                        {
                            // CU가 출석 창을 열었음을 알림
                            // → 위치 신호(PRESENCE)를 즉시 CU로 전송
                            // CU는 이 신호의 RSSI를 3회 측정 후 평균으로 출석 여부 판단
                            pc.printf("[IDLE] 출석 창이 열렸습니다. 위치 신호를 자동 전송합니다.\n");
                            makePresencePacket(&txPacket, myId); // 나의 ID가 담긴 PRESENCE 패킷 생성
                            L3_LLI_sendPacket(&txPacket);        // CU로 전송
                            pc.printf("[IDLE] 위치 신호 전송 완료. 승인 대기 중...\n");
                        }
                        else if (info->timeout_flag == TIMEOUT_FLAG_WARNING)
                        {
                            // 출석 마감 5분 전 경고 (아직 IDLE 상태 = 미출석)
                            pc.printf("[IDLE] ⚠ 출석 마감 5분 전입니다! 아직 미출석 상태입니다.\n");
                        }
                        else if (info->timeout_flag == TIMEOUT_FLAG_CLOSED)
                        {
                            // 출석 창 닫힘 (미출석 확정)
                            pc.printf("[IDLE] 출석 창이 닫혔습니다. 결석 처리됩니다.\n");
                        }
                        break;
                    }

                    // ─────────────────────────────────────────────
                    // TYPE_ATTENDANCE_APPROVAL: CU가 보낸 출석 승인/거부 패킷
                    // attendance_ok == 1: 승인 → ATTEND 전이
                    // attendance_ok == 0: 거부 → IDLE 유지, 재시도 요청
                    // ─────────────────────────────────────────────
                    case TYPE_ATTENDANCE_APPROVAL:
                    {
                        attendance_approval_t* approval = (attendance_approval_t*)pkt->data;

                        // 내 ID로 발송된 승인 패킷인지 확인
                        // (유니캐스트 전송이지만 L2 브로드캐스트로 수신될 수도 있으므로 필터링)
                        if (approval->student_id != myId)
                        {
                            debug_if(DBGMSG_L3,
                                     "[L3][IDLE] approval for student %i, I am %i, ignoring.\n",
                                     approval->student_id, myId);
                            break;
                        }

                        if (approval->attendance_ok == 1)
                        {
                            // [상태 전이] IDLE → ATTEND
                            // CU가 RSSI 임계값 이상으로 판단 → 출석 승인
                            pc.printf("[IDLE] 승인 수신 -> ATTEND 상태로 이동\n");
                            main_state = L3STATE_ATTEND; // ← 상태 전이 포인트
                        }
                        else
                        {
                            // CU가 RSSI 부족으로 거부 (강의실 밖으로 판단)
                            // Enter 키를 눌러 수동으로 PRESENCE를 재전송할 수 있음
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
                // [수동 PRESENCE 전송]
                // Enter 키를 누르면 즉시 CU에 위치 신호를 전송합니다.
                // 자동 주기(3초)와 별도로, 승인이 늦거나 거부된 경우 재시도에 활용
                makePresencePacket(&txPacket, myId); // PRESENCE 패킷 생성
                debug_if(DBGMSG_L3, "[L3][IDLE] sending presence signal (len:%i)\n",
                         (int)sizeof(packet_data_t));
                L3_LLI_sendPacket(&txPacket); // CU로 전송

                pc.printf("[IDLE] 위치 정보 전송 완료. 승인 대기 중...\n");
                wordLen = 0; // 입력 버퍼 초기화

                L3_event_clearEventFlag(L3_event_dataToSend); // 이벤트 플래그 클리어
            }
            break;

        // =====================================================
        // [상태 1: ATTEND - 출석 완료 상태]
        //
        // CU로부터 출석 승인(approval_ok=1)을 받은 후 진입하는 상태입니다.
        // 학생 간 채팅이 가능하며, 이탈/마감 이벤트를 감시합니다.
        //
        // 처리하는 이벤트:
        //   L3_event_msgRcvd  (패킷 수신 시)
        //     └─ TYPE_CHAT                                → 채팅 수신 출력
        //     └─ TYPE_ATTENDANCE_APPROVAL(ok=0)           → 이탈 감지 → LEAVE 전이
        //     └─ TYPE_ATTENDANCE_TIMEOUT(CLOSED)          → 출석 창 닫힘 → IDLE 복귀
        //     └─ TYPE_ATTENDANCE_TIMEOUT(WARNING)         → 마감 5분 전 경고
        //   L3_event_dataToSend (Enter 입력 시)
        //     └─ "<destId> <message>" 형식 파싱 후 채팅 패킷 전송
        //
        // 실습 포인트:
        //   - 채팅 전송 형식: <상대방ID> <메시지>  예) "2 안녕"
        //   - 채팅 패킷 구조는 L3_chatProtocol.h 참고
        // =====================================================
        case L3STATE_ATTEND:
        // =====================================================
        // RX (수신 이벤트 처리)
        // =====================================================
            if (L3_event_checkEventFlag(L3_event_msgRcvd))
            {
                pc.printf("[RX EVENT]\n");
                uint8_t*       dataPtr = L3_LLI_getMsgPtr();
                uint8_t        size    = L3_LLI_getSize();

                // ─────────────────────────────────────────────────
                // 패킷 분류 1: 학생 ↔ 학생 채팅 패킷 (chat_packet_t)
                // chat_packet_t의 첫 필드(type)로 식별합니다.
                // 크기가 최소 헤더(3바이트)보다 작으면 채팅 패킷이 아님
                // ─────────────────────────────────────────────────
                if (size >= 3) // 채팅 헤더 최소 크기 (type + src + dest)
                {
                    chat_packet_t* chat = (chat_packet_t*)dataPtr;

                    if (chat->type == TYPE_CHAT) // 채팅 패킷인지 확인
                    {
                        // 내 ID(myId)를 목적지로 하는 메시지만 화면 출력
                        // (다른 학생 간 채팅은 L2 브로드캐스트로 수신되므로 필터링 필요)
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

                // ─────────────────────────────────────────────────
                // 패킷 분류 2: CU → 학생 패킷 (packet_data_t)
                // type_id 필드로 패킷 종류를 구분합니다.
                // ─────────────────────────────────────────────────
                packet_data_t* pkt = (packet_data_t*)dataPtr;

                switch (pkt->type_id)
                {
                    // CU가 주기적 PRESENCE 체크 결과로 승인/거부를 재전송
                    // ATTEND 상태에서 ok=0이면 RSSI 부족으로 이탈 판단
                    case TYPE_ATTENDANCE_APPROVAL:
                    {
                        attendance_approval_t* approval =
                            (attendance_approval_t*)pkt->data;

                        if (approval->student_id != myId) break; // 내 패킷이 아니면 무시

                        if (approval->attendance_ok == 0)
                        {
                            // [상태 전이] ATTEND → LEAVE
                            // CU가 RSSI 부족으로 이탈 판단 (강의실 밖으로 나간 것으로 간주)
                            pc.printf("[ATTEND] 이탈 감지 -> LEAVE\n");
                            main_state = L3STATE_LEAVE; // ← 상태 전이 포인트
                        }
                        break;
                    }

                    case TYPE_ATTENDANCE_TIMEOUT:
                    {
                        attendance_timeout_t* info = (attendance_timeout_t*)pkt->data;

                        if (info->timeout_flag == TIMEOUT_FLAG_CLOSED)
                        {
                            // [상태 전이] ATTEND → IDLE
                            // 출석 창 종료 (수업 시작 완료), 채팅 세션도 종료
                            pc.printf("[ATTEND] 출석 창이 닫혔습니다. IDLE로 복귀합니다.\n");
                            L3_event_clearAllEventFlag(); // 모든 이벤트 초기화
                            wordLen = 0;                  // 입력 버퍼 초기화
                            main_state = L3STATE_IDLE;    // ← 상태 전이 포인트
                        }
                        else if (info->timeout_flag == TIMEOUT_FLAG_WARNING)
                        {
                            // 마감 5분 전 경고 (ATTEND 상태이므로 이미 출석은 완료됨)
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
            // TX (전송 이벤트 처리)
            // Enter 입력 시 채팅 메시지를 상대방 학생에게 전송합니다.
            // =====================================================
            else if (L3_event_checkEventFlag(L3_event_dataToSend))
            {
                // [입력 파싱] 형식: "<destId> <message>"  예) "2 안녕하세요"
                //
                // 주의: ARM newlib-nano 라이브러리에서 %hhu(uint8_t 직접 파싱)는
                //       신뢰성 없이 동작할 수 있으므로
                //       %u(unsigned int)로 읽은 후 uint8_t로 캐스팅하여 처리
                unsigned int tmpDestId = 0;
                int parsed = sscanf((char*)originalWord, "%u %1023[^\t\n]",
                    &tmpDestId,
                    (char*)sdu);
                uint8_t destId = (uint8_t)tmpDestId; // unsigned int → uint8_t 안전 캐스팅

                // 파싱 실패 (공백 없이 숫자만 입력하거나 형식 오류) → 무시하고 재입력 요청
                if (parsed < 2)
                {
                    pc.printf("[ATTEND] 입력 형식 오류. 올바른 형식: <상대방ID> <메시지>\n> ");
                    wordLen = 0;
                    L3_event_clearEventFlag(L3_event_dataToSend);
                    break;
                }

                // [채팅 패킷 생성 및 전송]
                // makeChatPacket(): chat_packet_t 구조체에 mode/type/src/dst/message 채움
                // L3_chatProtocol.h에서 구조체 정의 확인 가능
                chat_packet_t chatPkt;
                makeChatPacket(&chatPkt,
                            myId,    // 송신자 ID (나)
                            destId,  // 수신자 ID (상대방 학생)
                            (char*)sdu); // 메시지 본문

                // L2를 통해 상대방 학생에게 채팅 패킷 전송
                // (L2가 ARQ/SN을 처리하여 신뢰성 있게 전달)
                L3_LLI_sendChatPacket(&chatPkt);

                pc.printf("[CHAT] to #%u : %s\n",
                        (unsigned)destId,
                        chatPkt.message);

                pc.printf("> "); // 전송 완료, 다음 입력 대기

                wordLen = 0; // 입력 버퍼 초기화
                L3_event_clearEventFlag(L3_event_dataToSend); // 이벤트 플래그 클리어
            }

            break;

        // =====================================================
        // [상태 2: LEAVE - 이탈 상태]
        //
        // 출석 후 강의실 밖으로 나간 것으로 CU가 판단한 상태입니다.
        // (ATTEND 중 PRESENCE의 RSSI가 임계값 미만이 되면 진입)
        //
        // 처리하는 이벤트:
        //   L3_event_msgRcvd  (패킷 수신 시)
        //     └─ TYPE_ATTENDANCE_APPROVAL(ok=1) → CU가 복귀 승인 → ATTEND 복귀
        //     └─ TYPE_ATTENDANCE_TIMEOUT(CLOSED) → 마감 → IDLE 복귀 (미출석 확정)
        //   L3_event_dataToSend: 이탈 상태에서는 채팅 불가 (무시)
        //
        // 실습 포인트:
        //   - 이 상태에서 강의실로 돌아오면 PRESENCE RSSI가 회복되고
        //     CU가 다시 승인 패킷을 보내 ATTEND로 복귀할 수 있습니다.
        // =====================================================
        case L3STATE_LEAVE:
            if (L3_event_checkEventFlag(L3_event_msgRcvd))
            {
                packet_data_t* pkt = (packet_data_t*)L3_LLI_getMsgPtr();

                debug_if(DBGMSG_L3, "[LEAVE] RCVD type=0x%02X\n", pkt->type_id);

                switch (pkt->type_id)
                {
                    // CU가 PRESENCE를 다시 받고 RSSI 회복을 확인하면 재승인 패킷 전송
                    // ok=1이면 강의실에 돌아온 것으로 판단 → ATTEND 복귀
                    case TYPE_ATTENDANCE_APPROVAL:
                    {
                        attendance_approval_t* approval =
                            (attendance_approval_t*)pkt->data;

                        if (approval->student_id != myId) break; // 내 패킷이 아니면 무시

                        if (approval->attendance_ok == 1)
                        {
                            // [상태 전이] LEAVE → ATTEND
                            // 강의실로 복귀하여 RSSI 회복됨 → CU가 재승인
                            pc.printf("[LEAVE] 복귀 승인 -> ATTEND\n");
                            main_state = L3STATE_ATTEND; // ← 상태 전이 포인트
                        }
                        break;
                    }

                    case TYPE_ATTENDANCE_TIMEOUT:
                    {
                        attendance_timeout_t* info = (attendance_timeout_t*)pkt->data;

                        if (info->timeout_flag == TIMEOUT_FLAG_CLOSED)
                        {
                            // [상태 전이] LEAVE → IDLE
                            // 이탈 중 출석 창 마감 → 결석으로 최종 처리
                            pc.printf("[LEAVE] 미출석 확정 (CU) -> IDLE\n");
                            L3_event_clearAllEventFlag(); // 모든 이벤트 초기화
                            wordLen    = 0;
                            main_state = L3STATE_IDLE; // ← 상태 전이 포인트
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
