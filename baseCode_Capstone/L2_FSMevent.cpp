#include "mbed.h"
#include "L2_FSMevent.h"

static uint32_t eventFlag;


void L2_event_setEventFlag(L2_event_e event)
{
    eventFlag |= (0x01 << event);
}

void L2_event_clearEventFlag(L2_event_e event)
{
    eventFlag &= ~(0x01 << event);
}
void L2_event_clearAllEventFlag(void)
{
    eventFlag = 0;
}

int L2_event_checkEventFlag(L2_event_e event)
{
    return (eventFlag & (0x01 << event));
}