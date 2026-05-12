void L2_LLI_initLowLayer(uint8_t srcId);
void L2_LLI_sendData(uint8_t* msg, uint8_t size, uint8_t dest);
int L2_LLI_configSrcId(uint8_t);
uint8_t L2_LLI_getSrcId();
uint8_t* L2_LLI_getRcvdDataPtr();
uint8_t L2_LLI_getSize();
int16_t L2_LLI_getRssi(void);
int8_t L2_LLI_getSnr(void);
uint8_t L2_LLI_getIsBroadcasted(void);