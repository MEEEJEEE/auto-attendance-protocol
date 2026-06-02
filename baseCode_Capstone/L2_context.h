#ifndef L2_CONTEXT_H
#define L2_CONTEXT_H

#include <stdint.h>

// ID definition
#define L2_ID_CU          0
#define L2_ID_BROADCAST   255

// shared L2 state
extern uint8_t myL2ID;
extern uint8_t destL2ID;
extern uint8_t seqNum;

#endif