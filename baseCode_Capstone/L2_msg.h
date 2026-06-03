#include "mbed.h"

#define L2_MSG_TYPE_ACK         0
#define L2_MSG_TYPE_DATA        1
#define L2_MSG_TYPE_DATA_CONT   2

#define L2_MSG_OFFSET_TYPE  0
#define L2_MSG_OFFSET_SEQ   1
#define L2_MSG_OFFSET_DATA  2

#define L2_MSG_ACKSIZE      3

// [수정] 26 → 100으로 변경
// 기존 26바이트는 chat_packet_t(68바이트)보다 작아 채팅 패킷이 3개 PDU로 분할되었음.
// 분할 전송(fragmentation) 중 CU 브로드캐스트가 pduBuffer를 초기화하면
// 조립 중이던 채팅 데이터가 소멸 → 학생간 채팅 수신 불가 버그 발생.
// 100으로 올려 chat_packet_t(68바이트)가 PDU 1개로 전송되도록 수정.
// (arqPdu[200], sduIn[200] 버퍼는 충분히 큼)
#define L2_MSG_MAXDATASIZE  100
// seqNum은 uint8_t(0~255)이고 패킷 SN 필드도 1바이트이므로
// 실효 범위는 0~255. 기존 1024는 uint8_t에서 dead code였음.
#define L2_MSSG_MAX_SEQNUM  256


int L2_msg_checkIfData(uint8_t* msg);
int L2_msg_checkIfAck(uint8_t* msg);
int L2_msg_checkIfEndData(uint8_t* msg);
uint8_t L2_msg_encodeAck(uint8_t* msg_ack, uint8_t seq);
uint8_t L2_msg_encodeData(uint8_t* msg_data, uint8_t* data, int seq, int len, uint8_t);
uint8_t L2_msg_getSeq(uint8_t* msg);
uint8_t* L2_msg_getWord(uint8_t* msg);