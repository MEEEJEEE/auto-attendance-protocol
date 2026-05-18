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
#define TYPE_RSSI_INFO                 0x20U   // Student -> CU
#define TYPE_ATTENDANCE_APPROVAL       0x30U   // CU -> Student

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


/* 2. RSSI 위치 정보 (Student -> CU) */
typedef struct {
    uint8_t student_id;     // 학생 구분
    int16_t rssi_value;     // RSSI value
} rssi_info_t;


/* 3. 출석 승인 및 채팅 승인 (CU -> Student) */
typedef struct {
    uint8_t attendance_ok;  // 0: fail, 1: success
    uint8_t chat_enable;    // 0: deny, 1: allow
} attendance_approval_t;

#pragma pack(pop)
ㄴ

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


/* 2. RSSI 정보 패킷 생성 */
static inline void makeRSSIPacket(
    packet_data_t* packet,
    int16_t rssi
)
{
    rssi_info_t info;

    info.rssi_value = rssi;

    packet->mode = PACKET_MODE_STUDENT_TO_CU;
    packet->type_id = TYPE_RSSI_INFO;

    memset(packet->data, 0, sizeof(packet->data));
    memcpy(packet->data, &info, sizeof(info));
}


/* 3. 출석 승인 및 채팅 승인 패킷 생성 */
static inline void makeAttendanceApprovalPacket(
    packet_data_t* packet,
    uint8_t attendance_ok,
    uint8_t chat_enable
)
{
    attendance_approval_t info;

    info.attendance_ok = attendance_ok;
    info.chat_enable = chat_enable;

    packet->mode = PACKET_MODE_CU_TO_STUDENT;
    packet->type_id = TYPE_ATTENDANCE_APPROVAL;

    memset(packet->data, 0, sizeof(packet->data));
    memcpy(packet->data, &info, sizeof(info));
}


/*
====================================================================
RX SIDE EXAMPLE
====================================================================

Received:
uint8_t* dataPtr

Convert received buffer into packet structure:

packet_data_t* packet =
    (packet_data_t*)dataPtr;

Check packet type:

switch(packet->type_id)
{
    case TYPE_RSSI_INFO:
    {
        rssi_info_t* rssi =
            (rssi_info_t*)packet->data;

        int16_t value = rssi->rssi_value;
        break;
    }

    case TYPE_ATTENDANCE_TIMEOUT:
    {
        attendance_timeout_t* info =
            (attendance_timeout_t*)packet->data;

        if(info->timeout_flag)
        {
            // attendance closed
        }
        break;
    }
}
*/

#endif