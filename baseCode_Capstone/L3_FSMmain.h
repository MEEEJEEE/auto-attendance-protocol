// myId  : this node's own L2 source ID (used in packet headers)
// destId: destination node ID (CU ID for students; unused by the CU itself)
void L3_initFSM(uint8_t myId, uint8_t destId);
void L3_FSMrun(void);
