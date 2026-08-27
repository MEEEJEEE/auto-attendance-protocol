#ifndef L3_TIMER_H
#define L3_TIMER_H

#include <stdint.h>

// Start the attendance window timers.
// Two Timeout objects are armed:
//   1. pre-deadline timer  fires at (L3_ATTEND_WINDOW_SEC - L3_PRE_DEADLINE_ALERT_SEC)
//   2. deadline timer      fires at  L3_ATTEND_WINDOW_SEC
// Both set the corresponding event flags consumed by L3_FSMrun().
void L3_timer_startAttendanceWindow(void);

// Cancel both attendance window timers before they fire (e.g., manual abort).
void L3_timer_stopAttendanceWindow(void);

// Returns 1 while the deadline timer is still running, 0 after it has fired.
uint8_t L3_timer_getDeadlineTimerStatus(void);

#endif // L3_TIMER_H
