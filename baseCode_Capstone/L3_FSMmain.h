#ifndef L3_FSMMAIN_H
#define L3_FSMMAIN_H

#include <stdint.h>

// myId  : this node's own L2 source ID; 0 selects CU, all other IDs select student
// destId: destination node ID (CU ID for students; unused by the CU itself)
void L3_initFSM(uint8_t myId, uint8_t destId);
void L3_FSMrun(void);

#endif // L3_FSMMAIN_H
