// ---------------------------------------------------------------------------
// Unified L3 FSM implementation
//
// Both CU and student implementations are compiled into one firmware image.
// L3_initFSM() selects the active runtime branch from the node ID:
//   node ID 0 -> CU, any other node ID -> student.
// ---------------------------------------------------------------------------

#include "L3_FSMevent.h"
#include "L3_msg.h"
#include "L3_timer.h"
#include "L3_LLinterface.h"
#include "L3_convertPacket.h"
#include "L3_chatProtocol.h"
#include "protocol_parameters.h"
#include "mbed.h"

// main.cpp reads startup IDs through this shared serial instance. Only the
// selected role attaches its RX interrupt handler during L3_initFSM().
Serial pc(USBTX, USBRX);

namespace L3_CU
{

// ---------------------------------------------------------------------------
// CU FSM state definitions
// Matches docs/FSM_design.md section "1. Control Unit FSM"
//
//   0 WAIT   : before class starts; waiting for operator "start" command
//   1 OPEN   : attendance window active; PRESENCE packets handled
//   2 CLOSED : window expired; session ended; late packets discarded
// ---------------------------------------------------------------------------
#define L3STATE_WAIT    0
#define L3STATE_OPEN    1
#define L3STATE_CLOSED  2

// state variables
static uint8_t main_state = L3STATE_WAIT;
static uint8_t prev_state = main_state;

// keyboard input buffer: accumulates characters typed by the CU operator
static uint8_t inputWord[L3_MAXDATASIZE];
static uint8_t inputWordLen = 0;

// packet assembly buffer for all outbound L3 PDUs
static packet_data_t txPacket;

// per-student attendance table indexed by L2 source ID [0 .. L3_MAX_STUDENTS-1]
// 1 = present (TYPE_PRESENCE received; CU RSSI >= L3_RSSI_THRESHOLD), 0 = absent
static uint8_t attendanceTable[L3_MAX_STUDENTS];

// 수정: 학생별 RSSI 다중 측정 평균을 위한 누적 변수
// rssiSum[]   : 학생별 RSSI 수신값의 누적 합 (int32_t로 오버플로 방지)
// rssiCount[] : 학생별 RSSI 수신 횟수
static int32_t rssiSum[L3_MAX_STUDENTS];
static uint8_t rssiCount[L3_MAX_STUDENTS];

static int16_t lastRssi[L3_MAX_STUDENTS];
// ============================================================
// [이탈 감지 변수] (add-leave-timer 브랜치 병합)
//
// leaveDetected[i] : 학생 i가 현재 이탈 상태인지 (1=이탈 중, 0=정상)
// leaveTimer[i]    : 학생 i의 이탈 지속 시간 측정용 타이머
//
// 동작 흐름:
//   PRESENCE RSSI < 임계값 + 이미 출석한 학생
//     → leaveDetected=1, leaveTimer 시작, ok=0 전송 (ATTEND→LEAVE)
//   이탈 중 계속 RSSI 미달
//     → leaveTimer 경과 확인 → L3_LEAVE_GRACE_SEC 초과 시 결석 확정
//   이탈 중 RSSI 회복
//     → leaveDetected=0, 타이머 리셋, ok=1 전송 (LEAVE→ATTEND 복귀)
// ============================================================
static uint8_t leaveDetected[L3_MAX_STUDENTS]; // 학생별 이탈 상태 플래그
static Timer   leaveTimer[L3_MAX_STUDENTS];    // 학생별 이탈 지속 시간 타이머


// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Broadcast a TYPE_ATTENDANCE_TIMEOUT packet with the given flag to all students.
// flag: TIMEOUT_FLAG_OPEN    -> window just opened
//       TIMEOUT_FLAG_WARNING -> closing soon (5-min pre-deadline alert)
//       TIMEOUT_FLAG_CLOSED  -> window has closed
static void L3_CU_broadcastTimeout(uint8_t flag)
{
    makeAttendanceTimeoutPacket(&txPacket, flag);
    L3_LLI_sendPacket(&txPacket);
}

// Send a TYPE_ATTENDANCE_APPROVAL packet unicast to a specific student.
// attendance_ok : 1 = approved, 0 = rejected
// chat_enable   : 1 = student may join chat, 0 = chat not yet allowed
// 수정: makeAttendanceApprovalPacket에 student_id 인자 추가
//       L3_LLI_sendPacket이 패킷 내 student_id를 읽어 유니캐스트 라우팅
static void L3_CU_sendApproval(uint8_t studentId,
                                uint8_t attendance_ok,
                                uint8_t chat_enable)
{
    makeAttendanceApprovalPacket(&txPacket, studentId, attendance_ok, chat_enable);
    L3_LLI_sendPacket(&txPacket);
}

// Mark the given student as present if not already recorded, and log it.
// Also unicasts an approval packet so the student's FSM can advance to ATTEND.
static void L3_CU_markPresent(uint8_t studentId)
{
    if (studentId < L3_MAX_STUDENTS)
    {
        if (!attendanceTable[studentId])
        {
            attendanceTable[studentId] = 1;
            pc.printf("[CU][ATTEND] Student %i marked PRESENT\n", studentId);
        }
        // send attendance approval; chat not yet enabled (student must request)
        L3_CU_sendApproval(studentId, 1, 0);
    }
}

// Print the final attendance roll to the serial port.
// Called once on the OPEN -> CLOSED transition.
static void L3_CU_printAttendanceSummary(void)
{
    pc.printf("\n===== Attendance Summary =====\n");
    for (int i = 0; i < L3_MAX_STUDENTS; i++)
    {
        if (attendanceTable[i])
            pc.printf("  Student %2i : PRESENT\n", i);
    }
    pc.printf("==============================\n");
}


// ---------------------------------------------------------------------------
// Serial RX interrupt handler
// Accumulates operator keyboard input one character at a time.
// Sets L3_event_dataToSend when a complete line (CR or LF) is received or
// when the buffer is full.
// ---------------------------------------------------------------------------
static void L3service_processInputWord(void)
{
    char c = pc.getc();
    if (!L3_event_checkEventFlag(L3_event_dataToSend))
    {
        if (c == '\n' || c == '\r')
        {
            inputWord[inputWordLen++] = '\0';
            L3_event_setEventFlag(L3_event_dataToSend);
            debug_if(DBGMSG_L3, "[L3] operator input ready: %s\n", inputWord);
        }
        else
        {
            inputWord[inputWordLen++] = c;
            if (inputWordLen >= L3_MAXDATASIZE - 1)
            {
                inputWord[inputWordLen++] = '\0';
                // ISR 내부에서 printf 호출은 mbed에서 하드폴트를 유발할 수 있으므로
                // 플래그 설정만 수행하고 printf는 메인루프에서 처리함
                L3_event_setEventFlag(L3_event_dataToSend);
            }
        }
    }
}


// ---------------------------------------------------------------------------
// Open (or re-open) the attendance window.
// 모든 학생 상태 초기화 후 OPEN 브로드캐스트 전송 및 타이머 시작.
// L3STATE_WAIT 과 L3STATE_CLOSED 양쪽에서 호출 가능.
// ---------------------------------------------------------------------------
static void L3_CU_startAttendanceWindow(void)
{
    // 모든 학생 상태를 미출석으로 초기화
    memset(attendanceTable, 0, sizeof(attendanceTable));
    memset(leaveDetected,   0, sizeof(leaveDetected));
    memset(rssiSum,         0, sizeof(rssiSum));
    memset(rssiCount,       0, sizeof(rssiCount));
    for (int i = 0; i < L3_MAX_STUDENTS; i++)
    {
        leaveTimer[i].stop();
        leaveTimer[i].reset();
    }

    // 잔여 타이머 이벤트 제거 후 재시작
    L3_timer_stopAttendanceWindow();
    L3_event_clearEventFlag(L3_event_attendDeadline);
    L3_event_clearEventFlag(L3_event_preDeadlineAlert);

    pc.printf("\n[단계 1] 'start' 명령 수신. 출석 창을 열겠습니다.\n");
    pc.printf("[CU] Attendance window OPEN (%i sec).\n", L3_ATTEND_WINDOW_SEC);

    pc.printf("[단계 2] 출석 창 열림 신호(OPEN) 전체 브로드캐스트 전송 중...\n");
    L3_CU_broadcastTimeout(TIMEOUT_FLAG_OPEN);
    pc.printf("[단계 2] 브로드캐스트 전송 완료.\n");

    pc.printf("[단계 3] 출석 타이머 시작 (마감: %d초, 사전경고: %d초).\n",
              L3_ATTEND_WINDOW_SEC, L3_PRE_DEADLINE_ALERT_SEC);
    L3_timer_startAttendanceWindow();

    main_state = L3STATE_OPEN;
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

// Initialize the CU FSM: clear attendance table and attach the serial handler.
// myId  : this CU node's own L2 source ID (unused by CU logic, kept for API symmetry)
// destId: unused by the CU (it always broadcasts to L3_BROADCAST_ID)
void L3_initFSM(uint8_t myId, uint8_t destId)
{
    (void)myId;
    (void)destId;

    memset(attendanceTable, 0, sizeof(attendanceTable));

    // 수정: RSSI 다중 측정 평균 변수 초기화
    memset(rssiSum,   0, sizeof(rssiSum));
    memset(rssiCount, 0, sizeof(rssiCount));

    // 이탈 감지 변수 초기화
    memset(leaveDetected, 0, sizeof(leaveDetected));
    for (int i = 0; i < L3_MAX_STUDENTS; i++)
    {
        leaveTimer[i].stop();
        leaveTimer[i].reset();
    }

    pc.attach(&L3service_processInputWord, Serial::RxIrq);

    pc.printf("[CU] Initialized. Type 'start' to open the attendance window.\n");
}

void L3_FSMrun(void)
{
    // log every state transition for debugging
    if (prev_state != main_state)
    {
        debug_if(DBGMSG_L3, "[L3] State transition: %i -> %i\n", prev_state, main_state);
        prev_state = main_state;
    }

    switch (main_state)
    {
        // -------------------------------------------------------------------
        // State 0: WAIT
        // The CU waits until the operator types "start" (event a).
        // On "start":
        //   Action 1 - broadcast window-open notification, arm timers,
        //              transition to OPEN.
        // -------------------------------------------------------------------
        case L3STATE_WAIT:

            if (L3_event_checkEventFlag(L3_event_dataToSend))
            {
                if (strcmp((char*)inputWord, "start") == 0)
                {
                    // [단계 1] 'start' 명령 입력 확인 → 출석 창 OPEN 전환 시작
                    pc.printf("\n[단계 1] 'start' 명령 수신. 출석 창을 열겠습니다.\n");

                    pc.printf("[CU] Attendance window OPEN (%i sec).\n",
                              L3_ATTEND_WINDOW_SEC);

                    // [단계 2] 모든 학생에게 TIMEOUT_FLAG_OPEN 브로드캐스트 전송
                    pc.printf("[단계 2] 출석 창 열림 신호(OPEN) 전체 브로드캐스트 전송 중...\n");
                    L3_CU_broadcastTimeout(TIMEOUT_FLAG_OPEN);
                    pc.printf("[단계 2] 브로드캐스트 전송 완료.\n");

                    // [단계 3] 출석 마감 타이머 및 사전 경고 타이머 시작
                    pc.printf("[단계 3] 출석 타이머 시작 (마감: %d초, 사전경고: %d초).\n",
                              L3_ATTEND_WINDOW_SEC, L3_PRE_DEADLINE_ALERT_SEC);
                    L3_timer_startAttendanceWindow();

                    // 상태 전이: 0 (WAIT) -> 1 (OPEN)
                    main_state = L3STATE_OPEN;
                }
                else
                {
                    pc.printf("[CU][WAIT] 알 수 없는 명령 '%s'. 'start'를 입력하세요.\n",
                              inputWord);
                }
                inputWordLen = 0;
                L3_event_clearEventFlag(L3_event_dataToSend);
            }
            break;

        // -------------------------------------------------------------------
        // State 1: OPEN
        // The attendance window is active.  Events are handled in priority order:
        //
        //   attendDeadline   (event b) : close window, broadcast CLOSED notification
        //   preDeadlineAlert           : broadcast WARNING notification
        //   msgRcvd                    : route by packet type_id
        //     TYPE_PRESENCE -> L2_LLI_getRssi()로 RSSI 측정, N회 평균 후 임계값 비교
        //                      평균 RSSI >= 임계값: mark present + unicast approval
        //                      평균 RSSI <  임계값: send rejection so student stays in IDLE
        // -------------------------------------------------------------------
        case L3STATE_OPEN:

            if (L3_event_checkEventFlag(L3_event_attendDeadline))
            {
                // [단계 9] 출석 마감 타이머 만료 → CLOSED 전환
                pc.printf("\n[단계 9] 출석 마감 타이머 만료. 출석 창을 닫습니다.\n");
                pc.printf("[CU] Attendance window CLOSED.\n");

                // 모든 학생에게 TIMEOUT_FLAG_CLOSED 브로드캐스트
                L3_CU_broadcastTimeout(TIMEOUT_FLAG_CLOSED);

                // 수정: 마감 시점 결석 판정을 "마지막 수신 RSSI" 기준으로 변경.
                //       마지막 RSSI < 임계값이면 (LEAVE 여부·유예 시간과 무관하게) 무조건 결석.
                for (int i = 0; i < L3_MAX_STUDENTS; i++)
                {
                    if (attendanceTable[i] == 1 && lastRssi[i] < L3_RSSI_THRESHOLD)
                    {
                        attendanceTable[i] = 0;   // PRESENT 취소 → 결석
                        pc.printf("[CU] 학생 %i 마감 시점 마지막 RSSI=%i < 임계값 %i -> 결석 처리\n",
                                  i, lastRssi[i], L3_RSSI_THRESHOLD);
                    }
                    // 상태 변수 정리
                    leaveDetected[i] = 0;
                    leaveTimer[i].stop();
                    leaveTimer[i].reset();
                }

                L3_CU_printAttendanceSummary();

                // 상태 전이: 1 (OPEN) -> 2 (CLOSED)
                main_state = L3STATE_CLOSED;
                L3_event_clearEventFlag(L3_event_attendDeadline);
            }
            else if (L3_event_checkEventFlag(L3_event_preDeadlineAlert))
            {
                // [단계 8] 사전 경고 타이머 만료 → WARNING 브로드캐스트
                pc.printf("\n[단계 8] 출석 마감 %d초 전 사전 경고 브로드캐스트 전송.\n",
                          L3_PRE_DEADLINE_ALERT_SEC);
                pc.printf("[CU] Broadcasting pre-deadline absence alert.\n");
                L3_CU_broadcastTimeout(TIMEOUT_FLAG_WARNING);
                L3_event_clearEventFlag(L3_event_preDeadlineAlert);
            }
            else if (L3_event_checkEventFlag(L3_event_msgRcvd))
            {
                // 수신 패킷을 공통 프레임으로 캐스팅
                uint8_t*       dataPtr = L3_LLI_getMsgPtr();
                uint8_t        size    = L3_LLI_getSize();
                uint8_t        srcId   = L3_LLI_getSrcId();
                packet_data_t* pkt     = (packet_data_t*)dataPtr;

                (void)size;

                switch (pkt->type_id)
                {
                    // ─────────────────────────────────────────────────────
                    // TYPE_PRESENCE: 학생 위치 신호 수신
                    //
                    // RSSI는 패킷 페이로드가 아닌 L2에서 직접 측정 (L3_LLI_getRssi() 사용)
                    // 신규·기존 학생 모두 RSSI 샘플을 L3_RSSI_SAMPLE_COUNT회 누적한 뒤
                    // 평균값으로 강의실 내/외 여부를 판단합니다.
                    //
                    // [강의실 안 - avgRssi >= 임계값]
                    //   - 이탈 중이었다면 복귀 처리 (leaveDetected 초기화, 타이머 정지)
                    //   - 출석 승인 패킷 전송 (IDLE→ATTEND 또는 LEAVE→ATTEND)
                    //
                    // [강의실 밖 - avgRssi < 임계값]
                    //   - 이미 출석한 학생: 이탈 감지 타이머 시작 → ATTEND→LEAVE
                    //       유예 시간(L3_LEAVE_GRACE_SEC) 초과 시 결석 확정
                    //   - 미출석 학생: 즉시 거부 (IDLE 유지)
                    // ─────────────────────────────────────────────────────
                    case TYPE_PRESENCE:
                    {
                        int16_t rssi = L3_LLI_getRssi();

                        if (srcId < L3_MAX_STUDENTS)
                        {
                            lastRssi[srcId] = rssi;
                        }

                        // [단계 4] PRESENCE 수신 확인 (학생 ID, RSSI 출력)
                        pc.printf("\n[단계 4] 학생 %i 위치 신호(PRESENCE) 수신. RSSI=%i dBm\n",
                                  srcId, rssi);
                        debug_if(DBGMSG_L3,
                                 "[L3] TYPE_PRESENCE from student %i, rssi=%i dBm\n",
                                 srcId, rssi);

                        if (srcId < L3_MAX_STUDENTS)
                        {
                            // 신규·기존 학생 공통: RSSI 샘플 누적
                            rssiSum[srcId]  += rssi;
                            rssiCount[srcId]++;

                            // [단계 5] RSSI 샘플 누적 중
                            pc.printf("[단계 5] 학생 %i RSSI=%i dBm 누적 중 (%u/%u 샘플)\n",
                                      srcId, rssi,
                                      (unsigned)rssiCount[srcId],
                                      (unsigned)L3_RSSI_SAMPLE_COUNT);

                            if (rssiCount[srcId] >= L3_RSSI_SAMPLE_COUNT)
                            {
                                // 샘플 수집 완료 → 평균 RSSI 계산
                                int16_t avgRssi = (int16_t)(rssiSum[srcId] / rssiCount[srcId]);

                                pc.printf("[단계 5] 학생 %i 샘플 수집 완료. 평균 RSSI=%i dBm (임계값=%i dBm)\n",
                                          srcId, avgRssi, L3_RSSI_THRESHOLD);

                                // ─── 강의실 안 ───────────────────────────
                                if (avgRssi >= L3_RSSI_THRESHOLD)
                                {
                                    // 이탈 중이었다면 복귀 처리
                                    if (leaveDetected[srcId] == 1)
                                    {
                                        leaveDetected[srcId] = 0;
                                        leaveTimer[srcId].stop();
                                        leaveTimer[srcId].reset();
                                        pc.printf("[CU] 학생 %i 복귀 확인 -> ATTEND 복귀\n", srcId);
                                        debug_if(DBGMSG_L3,
                                                 "[L3] Student %i LEAVE -> ATTEND (returned)\n",
                                                 srcId);
                                    }

                                    // [단계 6-승인] 출석 승인 (신규: IDLE→ATTEND, 복귀: LEAVE→ATTEND, 유지: ATTEND)
                                    pc.printf("[단계 6-승인] 학생 %i 평균 RSSI %i >= 임계값 %i. 출석 승인.\n",
                                              srcId, avgRssi, L3_RSSI_THRESHOLD);
                                    L3_CU_markPresent(srcId);
                                    // [단계 7] 승인 패킷 전송 (markPresent 내부에서 sendApproval 호출됨)
                                    pc.printf("[단계 7] 학생 %i 승인 패킷 전송 완료.\n", srcId);
                                }
                                // ─── 강의실 밖 ───────────────────────────
                                else
                                {
                                    if (attendanceTable[srcId] == 1)
                                    {
                                        // ── 이미 출석한 학생: 이탈 감지 ──
                                        if (leaveDetected[srcId] == 0)
                                        {
                                            // 처음 이탈 감지 → 타이머 시작, ATTEND→LEAVE 전이
                                            leaveDetected[srcId] = 1;
                                            leaveTimer[srcId].reset();
                                            leaveTimer[srcId].start();
                                            pc.printf("[CU] 학생 %i 이탈 감지 (RSSI=%i dBm). 유예 타이머 시작 (%d초).\n",
                                                      srcId, avgRssi, L3_LEAVE_GRACE_SEC);
                                            debug_if(DBGMSG_L3,
                                                     "[L3] Student %i ATTEND -> LEAVE\n", srcId);
                                            L3_CU_sendApproval(srcId, 0, 0); // 학생 ATTEND→LEAVE
                                        }
                                        else
                                        {
                                            // 이미 이탈 중 → 유예 시간 경과 확인
                                            float leaveSec = leaveTimer[srcId].read();
                                            pc.printf("[CU] 학생 %i 이탈 지속 중 (%.1f / %d초)\n",
                                                      srcId, leaveSec, L3_LEAVE_GRACE_SEC);

                                            if (leaveSec >= (float)L3_LEAVE_GRACE_SEC)
                                            {
                                                // 유예 시간 초과 → 결석 확정
                                                pc.printf("[CU] 학생 %i 이탈 유예 시간 초과 -> 결석 처리\n", srcId);
                                                attendanceTable[srcId] = 0;
                                                leaveDetected[srcId]   = 0;
                                                leaveTimer[srcId].stop();
                                                leaveTimer[srcId].reset();
                                                debug_if(DBGMSG_L3,
                                                         "[L3] Student %i LEAVE -> absent (timeout)\n",
                                                         srcId);
                                            }
                                            // 유예 시간 내: 학생은 LEAVE 상태 유지 (추가 패킷 불필요)
                                        }
                                    }
                                    else
                                    {
                                        // ── 미출석 학생: 즉시 거부 ──
                                        // [단계 6-거부] 평균 RSSI 임계값 미만 → 강의실 외부, 출석 거부
                                        pc.printf("[단계 6-거부] 학생 %i 평균 RSSI %i < 임계값 %i. 출석 거부.\n",
                                                  srcId, avgRssi, L3_RSSI_THRESHOLD);
                                        debug_if(DBGMSG_L3,
                                                 "[L3] Student %i avgRSSI=%i below threshold, outside.\n",
                                                 srcId, avgRssi);
                                        L3_CU_sendApproval(srcId, 0, 0);
                                        pc.printf("[단계 7] 학생 %i 거부 패킷 전송 완료.\n", srcId);
                                    }
                                }

                                // 다음 측정 세션을 위해 누적값 초기화
                                rssiSum[srcId]   = 0;
                                rssiCount[srcId] = 0;
                            }
                        }
                        break;
                    }

                    // 채팅은 학생 간 직접 브로드캐스트로 처리되므로 CU에서 수신 시 무시

                    default:
                        debug_if(DBGMSG_L3,
                                 "[L3][WARNING] Unknown type_id 0x%02X from student %i, discarding.\n",
                                 (unsigned)pkt->type_id, srcId);
                        break;
                }

                L3_event_clearEventFlag(L3_event_msgRcvd);
            }
            else if (L3_event_checkEventFlag(L3_event_dataSendCnf))
            {
                // L2 confirmed the previous transmission; no further action needed
                debug_if(DBGMSG_L3, "[L3] DATA_CNF received.\n");
                L3_event_clearEventFlag(L3_event_dataSendCnf);
            }
            break;

        // -------------------------------------------------------------------
        // State 2: CLOSED
        // The session has ended.  All incoming packets are silently discarded.
        // The CU remains in this state until the device is reset.
        // -------------------------------------------------------------------
        case L3STATE_CLOSED:

            if (L3_event_checkEventFlag(L3_event_msgRcvd))
            {
                // 출석 창 닫힌 후 수신된 패킷은 모두 폐기
                debug_if(DBGMSG_L3,
                         "[L3][CLOSED] Late packet from student %i discarded.\n",
                         L3_LLI_getSrcId());
                L3_event_clearEventFlag(L3_event_msgRcvd);
            }

            if (L3_event_checkEventFlag(L3_event_dataToSend))
            {
                if (strcmp((char*)inputWord, "start") == 0)
                {
                    // 모든 학생 상태 초기화 후 새 출석 창 오픈
                    pc.printf("[CU] 새 출석 창을 시작합니다. 모든 학생 상태 초기화.\n");
                    L3_CU_startAttendanceWindow();
                }
                else
                {
                    pc.printf("[CU][CLOSED] 'start'를 입력하면 새 출석을 시작합니다.\n");
                }
                inputWordLen = 0;
                L3_event_clearEventFlag(L3_event_dataToSend);
            }
            break;

        default:
            break;
    }
}

} // namespace L3_CU

namespace L3_Student
{

// ============================================================
// [학생 역할] 학생 단말 L3 FSM
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
static uint8_t main_state = L3STATE_IDLE;   //L3STATE_IDLE;
static uint8_t prev_state = main_state;     //main_state;

// 키보드 입력 버퍼: 시리얼로 들어온 문자를 한 줄 단위로 누적
static uint8_t originalWord[1030]; // 입력 문자열 버퍼
static uint8_t wordLen = 0;        // 현재 버퍼에 저장된 문자 수
static uint8_t sdu[L3_MAXDATASIZE]; // 채팅 메시지 본문 임시 저장

// 송신 패킷 조립 버퍼 (PRESENCE, 채팅 등 모든 송신 패킷 공용)
static packet_data_t txPacket;

// 선택된 학생 역할이 공용 시리얼 포트를 사용합니다.
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
                //pc.printf("[RX EVENT]\n");
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
                            pc.printf("\n[CHAT RECV] from #%u : %s\n",
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
                            //pc.printf("[ATTEND] ⚠ 출석 마감 5분 전입니다!\n");
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

                pc.printf("[CHAT SEND] to #%u : %s\n",
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
                        else if (info->timeout_flag == TIMEOUT_FLAG_WARNING)
                        {
                            // 마감 5분 전 경고 (ATTEND 상태이므로 이미 출석은 완료됨)
                            pc.printf("[LEAVE] ⚠ 출석 마감 5분 전입니다!\n");
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

} // namespace L3_Student

typedef enum
{
    L3_ROLE_CU,
    L3_ROLE_STUDENT
} L3_role_e;

static L3_role_e activeRole = L3_ROLE_STUDENT;

void L3_initFSM(uint8_t myId, uint8_t destId)
{
    if (myId == L3_CU_ID)
    {
        activeRole = L3_ROLE_CU;
        L3_CU::L3_initFSM(myId, destId);
    }
    else
    {
        activeRole = L3_ROLE_STUDENT;
        L3_Student::L3_initFSM(myId, destId);
    }
}

void L3_FSMrun(void)
{
    if (activeRole == L3_ROLE_CU)
        L3_CU::L3_FSMrun();
    else
        L3_Student::L3_FSMrun();
}
