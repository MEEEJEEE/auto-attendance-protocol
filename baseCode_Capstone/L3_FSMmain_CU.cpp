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
//   1 OPEN   : attendance window active; LOCATION and CHAT packets handled
//   2 CLOSED : window expired; session ended; late packets discarded
// ---------------------------------------------------------------------------
#define L3STATE_WAIT    0
#define L3STATE_OPEN    1
#define L3STATE_CLOSED  2

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
// 1 = present (TYPE_RSSI_INFO received with rssi_value >= L3_RSSI_THRESHOLD), 0 = absent
static uint8_t attendanceTable[L3_MAX_STUDENTS];


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
    L3_LLI_dataReqFunc((uint8_t*)&txPacket, sizeof(packet_data_t), L3_BROADCAST_ID);
}

// Send a TYPE_ATTENDANCE_APPROVAL packet unicast to a specific student.
// attendance_ok : 1 = approved, 0 = rejected
// chat_enable   : 1 = student may join chat, 0 = chat not yet allowed
static void L3_CU_sendApproval(uint8_t studentId,
                                uint8_t attendance_ok,
                                uint8_t chat_enable)
{
    makeAttendanceApprovalPacket(&txPacket, attendance_ok, chat_enable);
    L3_LLI_dataReqFunc((uint8_t*)&txPacket, sizeof(packet_data_t), studentId);
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
                    // Action 1: activate attendance collection and LOCATION/CHAT reception
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
        //     TYPE_RSSI_INFO    -> compare rssi_value with threshold
        //                          if inside: mark present + unicast approval
        //                          if outside: send rejection so student stays in IDLE
        //     TYPE_CHAT_MESSAGE -> enable chat for sender + broadcast to all students
        //     TYPE_STUDENT_LEAVE-> log the leave; attendance already recorded, no change
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

                switch (pkt->type_id)
                {
                    case TYPE_RSSI_INFO:
                    {
                        // student is reporting its measured RSSI of the last CU signal
                        rssi_info_t* info = (rssi_info_t*)pkt->data;
                        int16_t rssi = info->rssi_value;

                        debug_if(DBGMSG_L3,
                                 "[L3] TYPE_RSSI_INFO from student %i, rssi=%i dBm\n",
                                 srcId, rssi);

                        if (rssi >= L3_RSSI_THRESHOLD)
                        {
                            // student is inside the classroom: mark present and approve
                            L3_CU_markPresent(srcId);
                        }
                        else
                        {
                            // student is outside the classroom: send rejection
                            debug_if(DBGMSG_L3,
                                     "[L3] Student %i RSSI=%i below threshold, outside.\n",
                                     srcId, rssi);
                            L3_CU_sendApproval(srcId, 0, 0);
                        }
                        break;
                    }
                    // 여기부터 수정 필요. chatting은 CU로 올 필요가 없기 때문
                    case TYPE_CHAT_MESSAGE:
                    {
                        // student is sending a chat message (also acts as chat-join request)
                        // enable chat for this student and broadcast the message to all
                        chat_message_t* chat = (chat_message_t*)pkt->data;

                        debug_if(DBGMSG_L3,
                                 "[L3] TYPE_CHAT_MESSAGE from student %i\n", srcId);
                        pc.printf("[CHAT] Student %i: %s\n", srcId, chat->message);

                        // grant chat permission to the sender
                        L3_CU_sendApproval(srcId, 1, 1);

                        // rebroadcast the original packet so all students receive it
                        L3_LLI_dataReqFunc(dataPtr, size, L3_BROADCAST_ID);
                        break;
                    }

                    case TYPE_STUDENT_LEAVE:
                    {
                        // student is leaving the classroom; attendance is already recorded
                        student_leave_t* leave = (student_leave_t*)pkt->data;
                        pc.printf("[CU] Student %i leaving (attendance retained).\n",
                                  leave->student_id);
                        break;
                    }

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
