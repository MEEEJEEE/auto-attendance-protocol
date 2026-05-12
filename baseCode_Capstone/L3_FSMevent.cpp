#include "mbed.h"
#include "L3_FSMevent.h"

static uint32_t eventFlag;


void L3_event_setEventFlag(L3_event_e event)
{
    eventFlag |= (0x01 << event);
}

void L3_event_clearEventFlag(L3_event_e event)
{
    eventFlag &= ~(0x01 << event);
}
void L3_event_clearAllEventFlag(void)
{
    eventFlag = 0;
}

int L3_event_checkEventFlag(L3_event_e event)
{
    return (eventFlag & (0x01 << event));
}