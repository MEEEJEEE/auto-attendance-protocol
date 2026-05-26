#include "mbed.h"
#include "L3_FSMevent_CU.h"
// #include "L3_FSMevent_student.h"
#include "protocol_parameters.h"

// ---------------------------------------------------------------------------
// Attendance window timers for the CU FSM
//
// Two one-shot Timeout objects are started together when the CU opens the
// attendance window (WAIT -> OPEN).  They fire at fixed offsets from that
// moment and post event flags that L3_FSMrun() consumes on the next tick.
// ---------------------------------------------------------------------------

// Pre-deadline timer: fires L3_PRE_DEADLINE_ALERT_SEC before the window closes
static Timeout preDeadlineTimer;
static uint8_t preDeadlineTimerStatus = 0;

// Deadline timer: fires when the full attendance window has elapsed (event b)
static Timeout deadlineTimer;
static uint8_t deadlineTimerStatus = 0;


// Interrupt context handler for the pre-deadline Timeout.
// Sets L3_event_preDeadlineAlert so the FSM broadcasts an absence warning.
static void L3_timer_preDeadlineHandler(void)
{
    preDeadlineTimerStatus = 0;
    L3_event_setEventFlag(L3_event_preDeadlineAlert);
}

// Interrupt context handler for the deadline Timeout.
// Sets L3_event_attendDeadline (event b) so the FSM closes the session.
static void L3_timer_deadlineHandler(void)
{
    deadlineTimerStatus = 0;
    L3_event_setEventFlag(L3_event_attendDeadline);
}


// Arm both timers immediately after the CU enters the OPEN state.
// The pre-deadline alert is skipped if the configured alert offset is <= 0
// (i.e., L3_PRE_DEADLINE_ALERT_SEC >= L3_ATTEND_WINDOW_SEC).
void L3_timer_startAttendanceWindow(void)
{
    float alertOffset = (float)(L3_ATTEND_WINDOW_SEC - L3_PRE_DEADLINE_ALERT_SEC);
    if (alertOffset > 0.0f)
    {
        preDeadlineTimer.attach(L3_timer_preDeadlineHandler, alertOffset);
        preDeadlineTimerStatus = 1;
    }

    deadlineTimer.attach(L3_timer_deadlineHandler, (float)L3_ATTEND_WINDOW_SEC);
    deadlineTimerStatus = 1;
}

// Disarm both timers; used when the session is aborted before natural expiry.
void L3_timer_stopAttendanceWindow(void)
{
    preDeadlineTimer.detach();
    deadlineTimer.detach();
    preDeadlineTimerStatus = 0;
    deadlineTimerStatus = 0;
}

// Returns 1 while the deadline timer is still counting down, 0 after it fires.
uint8_t L3_timer_getDeadlineTimerStatus(void)
{
    return deadlineTimerStatus;
}
