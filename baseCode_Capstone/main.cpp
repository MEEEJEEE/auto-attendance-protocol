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
uint8_t input_thisId=1;
uint8_t input_destId=0;


// ========== 수정 코드 추가 ============
uint8_t getID()
{
    char c;
    char rx_buffer[4];
    int rx_index = 0;

    while (1)
    {
        c = getchar();

        if (c == '\n' || c == '\r')
        {
            rx_buffer[rx_index] = '\0';
            break;
        }
        else
        {
            rx_buffer[rx_index++] = c;
        }
    }

    return (uint8_t)atoi(rx_buffer);
}


//FSM operation implementation ------------------------------------------------
int main(void){

    //initialization
    printf("------------------ protocol stack starts! --------------------------\n");
        //source & destination ID setting
    printf(":: ID for this node : ");
    input_thisId = getID();
    printf(":: ID for the destination : ");
    input_thisId = getID();
    getchar(); // consume trailing newline after scanf

    printf("endnode : %i, dest : %i\n", input_thisId, input_destId);
    
    

    //initialize lower layer stacks
    L2_initFSM(input_thisId);
    L3_initFSM(input_thisId, input_destId);  // myId + destId both passed to L3
    
    while(1)
    {
        L2_FSMrun();
        L3_FSMrun();
    }
}