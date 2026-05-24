// ---------------------------------------------------------------------------
// L3 FSM event definitions for the CU (Control Unit)
// Each enumerator maps to a single bit in the 32-bit eventFlag bitmask.
// ---------------------------------------------------------------------------
typedef enum L3_event
{
    L3_event_msgRcvd          = 2,  // DATA_IND: L2 delivered an incoming message
    L3_event_dataToSend       = 4,  // operator keyboard input line is ready to process
    L3_event_dataSendCnf      = 5,  // DATA_CNF: L2 confirmed the last transmission
    L3_event_recfgSrcIdCnf    = 6,  // RECONFIG_SRCID_CNF from L2 layer
    L3_event_attendDeadline   = 7,  // event b: attendance window timer expired -> OPEN->CLOSED
    L3_event_preDeadlineAlert = 8   // fired L3_PRE_DEADLINE_ALERT_SEC before the deadline
} L3_event_e;


void L3_event_setEventFlag(L3_event_e event);
void L3_event_clearEventFlag(L3_event_e event);
void L3_event_clearAllEventFlag(void);
int  L3_event_checkEventFlag(L3_event_e event);
