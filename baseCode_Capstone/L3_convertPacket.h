// 데이터 고정 길이는 가능, 포인터 사용 불가


/*
=== wanted data====

1. 출석 시간 초과 Flag          CU -> Student
2. 위치 RSSI                   Student -> CU
3. 출석확인 및 Chatting 승인    CU -> Student

===================
*/



#ifndef L3_CONVERTPACKET_H
#define L3_CONVERTPACKET_H

#include <stdint.h>
#include <string.h>

#pragma pack(push, 1)

/*
========================================================
 Packet Mode
========================================================
*/
#define PACKET_MODE_CU_TO_STUDENT      0x01U
#define PACKET_MODE_STUDENT_TO_CU      0x02U


/*
========================================================
 Packet Type ID
========================================================
*/
#define TYPE_ATTENDANCE_TIMEOUT        0x10U   // CU -> Student
#define TYPE_PRESENCE                  0x20U   // Student -> CU
#define TYPE_ATTENDANCE_APPROVAL       0x30U   // CU -> Student


/*
========================================================
 timeout_flag values for attendance_timeout_t
========================================================
*/
#define TIMEOUT_FLAG_OPEN              0x00U   // attendance window just opened
#define TIMEOUT_FLAG_WARNING           0x01U   // closing soon (5-min pre-deadline alert)
#define TIMEOUT_FLAG_CLOSED            0x02U   // attendance window has closed

/*
========================================================
 Common Packet Frame
========================================================
*/
typedef struct {
    uint32_t mode;
    uint32_t type_id;
    uint8_t  data[10];
} packet_data_t;

/*
========================================================
 Data Structures
========================================================
*/

/* 1. 출석 시간 종료 Flag (CU -> Student) */
typedef struct {
    uint8_t timeout_flag;   // 0: attendance available
                              // 1: attendance closed
} attendance_timeout_t;


/* 2. RSSI 위치 정보 (Student -> CU) CU에서 L2_LLI_getRssi로 판단 가능하므로 student는 시그널만 보냄*/
typedef struct {
    uint8_t student_id;     // 학생 구분
} presence_t;


/* 3. 출석 승인 및 채팅 승인 (CU -> Student)
   수정: student_id 필드 추가 - L3_LLI_sendPacket이 유니캐스트 목적지를
         패킷 페이로드에서 읽어 라우팅하기 위해 필요 */
typedef struct {
    uint8_t student_id;     // 수신 대상 학생 ID (L3_LLI_sendPacket 라우팅용)
    uint8_t attendance_ok;  // 0: fail, 1: success
    uint8_t chat_enable;    // 0: deny, 1: allow
} attendance_approval_t;


#pragma pack(pop)


/*
========================================================
 Packet Create Functions
========================================================
*/

/* 1. 출석 시간 종료 패킷 생성 */
static inline void makeAttendanceTimeoutPacket(
    packet_data_t* packet,
    uint8_t timeout_flag
)
{
    attendance_timeout_t info;

    info.timeout_flag = timeout_flag;

    packet->mode = PACKET_MODE_CU_TO_STUDENT;
    packet->type_id = TYPE_ATTENDANCE_TIMEOUT;

    memset(packet->data, 0, sizeof(packet->data));
    memcpy(packet->data, &info, sizeof(info));
}


/* 2. 위치 정보 패킷 생성 */
// student_id : the sending student's own L2 source ID
// student는 RSSI를 판단하지 않고 위치 정보만 보냄. CU 단에서 위치 정보 기반으로 RSSI 도출
// 수정: rssi 파라미터 제거 (CU가 L2_LLI_getRssi()로 직접 측정하므로 불필요)
//       rssi_info_t → presence_t 로 변경 (정의되지 않은 타입 사용 버그 수정)
static inline void makePresencePacket(
    packet_data_t* packet,
    uint8_t student_id
)
{
    presence_t info;

    info.student_id = student_id;

    packet->mode = PACKET_MODE_STUDENT_TO_CU;
    packet->type_id = TYPE_PRESENCE;

    memset(packet->data, 0, sizeof(packet->data));
    memcpy(packet->data, &info, sizeof(info));
}


/* 3. 출석 승인 및 채팅 승인 패킷 생성 */
// 수정: student_id 파라미터 추가 (L3_LLI_sendPacket 유니캐스트 라우팅을 위해 패킷에 포함)
static inline void makeAttendanceApprovalPacket(
    packet_data_t* packet,
    uint8_t student_id,
    uint8_t attendance_ok,
    uint8_t chat_enable
)
{
    attendance_approval_t info;

    info.student_id    = student_id;
    info.attendance_ok = attendance_ok;
    info.chat_enable   = chat_enable;

    packet->mode = PACKET_MODE_CU_TO_STUDENT;
    packet->type_id = TYPE_ATTENDANCE_APPROVAL;

    memset(packet->data, 0, sizeof(packet->data));
    memcpy(packet->data, &info, sizeof(info));
}

#endif // L3_CONVERTPACKET_H
