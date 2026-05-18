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

// scratch buffer for assembling outbound PDUs
static uint8_t sdu[L3_MAXDATASIZE];

// per-student attendance table indexed by L2 source ID [0 .. L3_MAX_STUDENTS-1]
// 1 = present (LOCATION received with RSSI >= L3_RSSI_THRESHOLD), 0 = absent
static uint8_t attendanceTable[L3_MAX_STUDENTS];


// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Build a two-byte NOTIFY PDU and send it to destId.
// Payload: [L3_PKT_TYPE_NOTIFY][notifyType]
static void L3_CU_sendNotify(uint8_t notifyType, uint8_t destId)
{
    sdu[0] = L3_PKT_TYPE_NOTIFY;
    sdu[1] = notifyType;
    L3_LLI_dataReqFunc(sdu, 2, destId);
}

// Mark the given student as present if not already recorded, and log it.
// studentId must be a valid index (< L3_MAX_STUDENTS).
static void L3_CU_markPresent(uint8_t studentId)
{
    if (studentId < L3_MAX_STUDENTS && !attendanceTable[studentId])
    {
        attendanceTable[studentId] = 1;
        pc.printf("[CU][ATTEND] Student %i marked PRESENT\n", studentId);
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
// destId is unused by the CU (it always broadcasts), kept for API compatibility.
void L3_initFSM(uint8_t destId)
{
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
        //   Action 1 - arm the attendance window timers, transition to OPEN.
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
        // The attendance window is active.  Three categories of events are
        // handled (checked in priority order):
        //
        //   event b (attendDeadline)   : close window, broadcast session-closed
        //   preDeadlineAlert           : broadcast absence warning
        //   msgRcvd                    : route by packet type
        //     LOCATION -> compare RSSI against threshold, update attendance table
        //     CHAT     -> forward message to all students (broadcast)
        // -------------------------------------------------------------------
        case L3STATE_OPEN:

            if (L3_event_checkEventFlag(L3_event_attendDeadline))
            {
                // event b: attendance window timer expired
                // Action 2: close attendance, broadcast session-closed notification
                pc.printf("[CU] Attendance window CLOSED.\n");
                L3_CU_sendNotify(L3_NOTIFY_SESSION_CLOSED, L3_BROADCAST_ID);
                L3_CU_printAttendanceSummary();

                // transition: 1 (OPEN) -> 2 (CLOSED)
                main_state = L3STATE_CLOSED;
                L3_event_clearEventFlag(L3_event_attendDeadline);
            }
            else if (L3_event_checkEventFlag(L3_event_preDeadlineAlert))
            {
                // pre-deadline: broadcast an absence warning to all students
                // so that students who have not yet checked in are notified
                pc.printf("[CU] Broadcasting pre-deadline absence alert.\n");
                L3_CU_sendNotify(L3_NOTIFY_DEADLINE_ALERT, L3_BROADCAST_ID);
                L3_event_clearEventFlag(L3_event_preDeadlineAlert);
            }
            else if (L3_event_checkEventFlag(L3_event_msgRcvd))
            {
                // retrieve metadata for the arriving packet
                uint8_t* dataPtr = L3_LLI_getMsgPtr();
                uint8_t  size    = L3_LLI_getSize();
                uint8_t  srcId   = L3_LLI_getSrcId();
                int16_t  rssi    = L3_LLI_getRssi();

                uint8_t pktType  = dataPtr[0];

                if (pktType == L3_PKT_TYPE_LOCATION)
                {
                    // LOCATION packet: the L2 RSSI of this frame indicates the
                    // student's physical distance from the CU.
                    // RSSI >= L3_RSSI_THRESHOLD -> student is inside the classroom.
                    // RSSI <  L3_RSSI_THRESHOLD -> student is outside; no update.
                    debug_if(DBGMSG_L3, "[L3] LOCATION from student %i, RSSI=%i dBm\n",
                             srcId, rssi);

                    if (rssi >= L3_RSSI_THRESHOLD)
                    {
                        L3_CU_markPresent(srcId);
                    }
                    else
                    {
                        debug_if(DBGMSG_L3,
                                 "[L3] Student %i RSSI=%i below threshold, outside classroom.\n",
                                 srcId, rssi);
                    }
                }
                else if (pktType == L3_PKT_TYPE_CHAT)
                {
                    // CHAT packet: rebroadcast the full payload (type byte + message)
                    // to all students so they can display it.
                    debug_if(DBGMSG_L3, "[L3] CHAT from student %i, forwarding broadcast.\n",
                             srcId);
                    pc.printf("[CHAT] Student %i: %s\n", srcId, (char*)(dataPtr + 1));

                    memcpy(sdu, dataPtr, size);
                    L3_LLI_dataReqFunc(sdu, size, L3_BROADCAST_ID);
                }
                else
                {
                    debug_if(DBGMSG_L3,
                             "[L3][WARNING] Unknown packet type 0x%02X from student %i, discarding.\n",
                             pktType, srcId);
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
