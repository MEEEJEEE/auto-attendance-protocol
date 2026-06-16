#include "mbed.h"
#include "string.h"
#include "L2_FSMmain.h"
#include "L3_FSMmain.h"

uint8_t input_thisId;
uint8_t input_destId;

extern Serial pc;

uint8_t getID() {
    char c;
    char rx_buffer[4];
    int rx_index = 0;
   
    while (1)
    {
        c= pc.getc();
        if (c == '\n' || c == '\r') {
            rx_buffer[rx_index] = '\0'; // 문자열 끝 표시
            rx_index = 0;
            break;
        } else {
            rx_buffer[rx_index++] = c;
        }
    }

    return (uint8_t)atoi(rx_buffer);
}

#ifndef IS_CU
// 학생 노드 번호 입력 (1-99, 최대 두 자리)
static uint8_t getNodeId(void)
{
    char buf[4];
    int  idx = 0;
    char c;

    printf("Enter node number (1-99): ");

    while (1)
    {
        c = (char)getchar();
        if (c == '\n' || c == '\r')
        {
            buf[idx] = '\0';
            break;
        }
        if (idx < 2 && c >= '0' && c <= '9')
            buf[idx++] = c;
    }

    int id = atoi(buf);
    if (id >= 1 && id <= 99)
        return (uint8_t)id;

    printf("[WARN] Invalid ID, using default 1\n");
    return 1;
}
#endif

//FSM operation implementation ------------------------------------------------
int main(void)
{
    unsigned int tempId;
    unsigned int tempDestId;

    printf("\n------------------ protocol stack starts! --------------------------\n");

    printf("Enter my ID: ");
    input_thisId = getID();

    printf("%d\n", input_thisId);

    printf("Enter destination ID: ");
    input_destId = getID();

    printf("%d\n", input_destId);

    printf("endnode : %u, dest : %u\n",
           input_thisId,
           input_destId);

    L2_initFSM(input_thisId);
    L2_configDestId(input_destId);
    L3_initFSM(input_thisId, input_destId);

    while (1)
    {
        L2_FSMrun();
        L3_FSMrun();
    }
}

