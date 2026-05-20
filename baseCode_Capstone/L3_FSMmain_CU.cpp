#include "L3_FSMevent.h"
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
                L3_event_setEventFlag(L3_event_dataToSend);
                pc.printf("\n[CU] input buffer full, processing: %s\n", inputWord);
            }
        }
    }
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
                    // event a: class start time reached
                    // Action 1: activate attendance collection and PRESENCE reception
                    pc.printf("[CU] Attendance window OPEN (%i sec).\n",
                              L3_ATTEND_WINDOW_SEC);

                    // notify all students that the window is now open
                    L3_CU_broadcastTimeout(TIMEOUT_FLAG_OPEN);

                    // arm pre-deadline and deadline timers
                    L3_timer_startAttendanceWindow();

                    // transition: 0 (WAIT) -> 1 (OPEN)
                    main_state = L3STATE_OPEN;
                }
                else
                {
                    pc.printf("[CU][WAIT] Unknown command '%s'. Type 'start' to begin.\n",
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
                // event b: attendance window timer expired
                // Action 2: close attendance, broadcast CLOSED notification
                pc.printf("[CU] Attendance window CLOSED.\n");
                L3_CU_broadcastTimeout(TIMEOUT_FLAG_CLOSED);
                L3_CU_printAttendanceSummary();

                // transition: 1 (OPEN) -> 2 (CLOSED)
                main_state = L3STATE_CLOSED;
                L3_event_clearEventFlag(L3_event_attendDeadline);
            }
            else if (L3_event_checkEventFlag(L3_event_preDeadlineAlert))
            {
                // pre-deadline: broadcast a warning to all students still absent
                pc.printf("[CU] Broadcasting pre-deadline absence alert.\n");
                L3_CU_broadcastTimeout(TIMEOUT_FLAG_WARNING);
                L3_event_clearEventFlag(L3_event_preDeadlineAlert);
            }
            else if (L3_event_checkEventFlag(L3_event_msgRcvd))
            {
                // cast the raw buffer to the common packet frame
                uint8_t*       dataPtr = L3_LLI_getMsgPtr();
                uint8_t        size    = L3_LLI_getSize();
                uint8_t        srcId   = L3_LLI_getSrcId();
                packet_data_t* pkt     = (packet_data_t*)dataPtr;

                (void)size;

                switch (pkt->type_id)
                {
                    // 수정: TYPE_RSSI_INFO → TYPE_PRESENCE 로 변경
                    //       RSSI를 패킷 페이로드에서 읽지 않고
                    //       L3_LLI_getRssi()로 L2 레이어에서 직접 측정
                    //       (주석 "L2_LLI_getRssi() 활용 생각할 것" 반영)
                    case TYPE_PRESENCE:
                    {
                        // 학생이 위치 신호를 보냄: CU에서 L2 RSSI를 직접 읽어 판단
                        int16_t rssi = L3_LLI_getRssi();

                        debug_if(DBGMSG_L3,
                                 "[L3] TYPE_PRESENCE from student %i, rssi=%i dBm\n",
                                 srcId, rssi);

                        if (srcId < L3_MAX_STUDENTS)
                        {
                            // 이미 출석 승인된 학생이 위치 신호를 재전송하는 경우:
                            // RSSI 누적 없이 바로 재승인 (재전송으로 인한 불필요한 누적 방지)
                            if (attendanceTable[srcId])
                            {
                                debug_if(DBGMSG_L3,
                                         "[L3] Student %i already present, re-sending approval.\n",
                                         srcId);
                                L3_CU_sendApproval(srcId, 1, 0);
                                break;
                            }

                            // 신규 학생: RSSI 다중 측정 평균 로직
                            // L3_RSSI_SAMPLE_COUNT 회 수신 후 평균값으로 출석 판단
                            rssiSum[srcId]  += rssi;
                            rssiCount[srcId]++;

                            pc.printf("[CU] Student %i RSSI=%i dBm 수신 (%u/%u 샘플)\n",
                                      srcId, rssi,
                                      (unsigned)rssiCount[srcId],
                                      (unsigned)L3_RSSI_SAMPLE_COUNT);

                            if (rssiCount[srcId] >= L3_RSSI_SAMPLE_COUNT)
                            {
                                // 충분한 샘플 수집 완료 → 평균 RSSI 계산
                                int16_t avgRssi = (int16_t)(rssiSum[srcId] / rssiCount[srcId]);

                                pc.printf("[CU] Student %i 평균 RSSI=%i dBm (임계값=%i dBm)\n",
                                          srcId, avgRssi, L3_RSSI_THRESHOLD);

                                if (avgRssi >= L3_RSSI_THRESHOLD)
                                {
                                    // 강의실 내 위치 확인 → 출석 승인
                                    L3_CU_markPresent(srcId);
                                }
                                else
                                {
                                    // 평균 RSSI가 임계값 미만 → 강의실 외부로 판단, 출석 거부
                                    debug_if(DBGMSG_L3,
                                             "[L3] Student %i avgRSSI=%i below threshold, outside.\n",
                                             srcId, avgRssi);
                                    pc.printf("[CU] Student %i 출석 거부 (평균 RSSI %i < 임계값 %i)\n",
                                              srcId, avgRssi, L3_RSSI_THRESHOLD);
                                    L3_CU_sendApproval(srcId, 0, 0);
                                }

                                // 다음 측정 세션을 위해 누적값 초기화
                                rssiSum[srcId]  = 0;
                                rssiCount[srcId] = 0;
                            }
                        }
                        break;
                    }

                    // 수정: TYPE_CHAT_MESSAGE case 제거
                    // 채팅은 CU를 거칠 필요가 없으므로 해당 case 삭제
                    // (원래 주석 "여기부터 수정 필요. chatting은 CU로 올 필요가 없기 때문." 반영)
                    // → 학생 간 채팅은 직접 브로드캐스트 방식으로 변경됨

                    // 수정: TYPE_STUDENT_LEAVE case 제거
                    // L3_convertPacket.h에서 해당 패킷 타입이 제거되었으므로 삭제

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
                // late-arriving packets are dropped after the window closes
                debug_if(DBGMSG_L3,
                         "[L3][CLOSED] Late packet from student %i discarded.\n",
                         L3_LLI_getSrcId());
                L3_event_clearEventFlag(L3_event_msgRcvd);
            }
            break;

        default:
            break;
    }
}
