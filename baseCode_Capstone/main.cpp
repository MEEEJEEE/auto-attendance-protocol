#include "mbed.h"
#include "string.h"
#include "L2_FSMmain.h"
#include "L3_FSMmain.h"

// NOTE: Serial pc is declared as 'static' inside each L3_FSMmain_*.cpp.
// Do NOT declare another Serial pc here — mbed does not support two Serial
// objects on the same USBTX/USBRX pins and will malfunction at runtime.
// Use printf/scanf which mbed retargets to the USB serial port automatically.

//GLOBAL variables (DO NOT TOUCH!) ------------------------------------------

// NODE_TYPE 빌드 플래그로 ID 자동 설정
// make NODE_TYPE=CU      → CU: ID=0, dest=255(브로드캐스트)
// make NODE_TYPE=student → 학생: ID=1, dest=0(CU)
#ifdef IS_CU
uint8_t input_thisId = 0;    // CU ID 고정
uint8_t input_destId = 255;  // CU는 브로드캐스트로 송신
#else
uint8_t input_thisId = 2;    // 학생 ID (여러 학생이면 각자 다르게 설정) //
uint8_t input_destId = 1;    // 목적지 = CU
#endif

//FSM operation implementation ------------------------------------------------
int main(void){

    printf("------------------ protocol stack starts! --------------------------\n");
    printf("endnode : %i, dest : %i\n", input_thisId, input_destId);

    L2_initFSM(input_thisId);
    L3_initFSM(input_thisId, input_destId);
    
    while(1){
        L2_FSMrun();
        L3_FSMrun();
 
   }
}
