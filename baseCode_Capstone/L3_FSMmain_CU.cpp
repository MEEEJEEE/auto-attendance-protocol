#include "L3_FSMevent_CU.h"
#include "L3_msg.h"
#include "L3_timer.h"
#include "L3_LLinterface.h"
#include "protocol_parameters.h"
#include "mbed.h"

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

// 수정: RSSI 다중 측정 평균을 위한 샘플 수 설정
// 학생 1명당 이 횟수만큼 RSSI를 수신한 후 평균값으로 출석 여부를 판단
#define L3_RSSI_SAMPLE_COUNT    3

// state variables
static uint8_t main_state = L3STATE_WAIT;
static uint8_t prev_state = main_state;

// serial port interface used for operator commands and status output
static Serial pc(USBTX, USBRX);

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
