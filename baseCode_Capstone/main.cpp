#include "mbed.h"
#include "string.h"
#include "L2_FSMmain.h"
#include "L3_FSMmain.h"

// NOTE: Serial pc is declared as 'static' inside each L3_FSMmain_*.cpp.
// Do NOT declare another Serial pc here — mbed does not support two Serial
// objects on the same USBTX/USBRX pins and will malfunction at runtime.
// Use printf/scanf which mbed retargets to the USB serial port automatically.

//GLOBAL variables (DO NOT TOUCH!) ------------------------------------------

//source/destination ID
uint8_t input_thisId = 1;  // 이 보드 ID
uint8_t input_destId = 0;  // 목적지 ID

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
