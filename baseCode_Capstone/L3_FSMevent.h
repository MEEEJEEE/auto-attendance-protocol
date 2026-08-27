#ifndef L3_FSMEVENT_H
#define L3_FSMEVENT_H

// Shared L3 event bits. Role-specific events use distinct bits so that the
// CU attendance timers and the student presence ticker cannot alias.
typedef enum L3_event
{
    L3_event_msgRcvd          = 2,  // DATA_IND: L2 delivered an incoming message
    L3_event_dataToSend       = 4,  // operator keyboard input line is ready
    L3_event_dataSendCnf      = 5,  // DATA_CNF: L2 confirmed transmission
    L3_event_recfgSrcIdCnf    = 6,  // RECONFIG_SRCID_CNF from L2
    L3_event_periodicPresence = 7,  // student: periodic presence ticker
    L3_event_attendDeadline   = 8,  // CU: attendance window expired
    L3_event_preDeadlineAlert = 9   // CU: pre-deadline warning timer
} L3_event_e;

void L3_event_setEventFlag(L3_event_e event);
void L3_event_clearEventFlag(L3_event_e event);
void L3_event_clearAllEventFlag(void);
int  L3_event_checkEventFlag(L3_event_e event);

#endif // L3_FSMEVENT_H
