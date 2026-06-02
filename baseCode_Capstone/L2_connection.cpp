// ============================================================================
// L2_connection.cpp
// ----------------------------------------------------------------------------
// Implements lightweight L2 peer/session switching.
// ============================================================================

#include "L2_connection.h"
#include "L2_FSMmain.h"
#include "protocol_parameters.h"
#include "L2_context.h"

// ---------------------------------------------------------------------------
// External variables from L2_FSMmain.cpp
// NOTE:
//   These are currently global/static ARQ session variables.
//   Since the existing L2 design supports only one active ARQ session,
//   switching peer requires resetting the sequence synchronization state.
// ---------------------------------------------------------------------------
extern uint8_t destL2ID;    
extern uint8_t seqNum;     

// ---------------------------------------------------------------------------
// L2_connect()
// ---------------------------------------------------------------------------
// Switch current communication peer.
//
// Effects:
//   - updates destination L2 ID
//   - resets ARQ sequence synchronization
//
// Recommended usage:
//   - entering ATTEND state      -> connect to CU
//   - sending student chat       -> connect to target student
//   - chat finished              -> reconnect to CU
// ---------------------------------------------------------------------------
void L2_connect(uint8_t destId)
{
    destL2ID = destId;

    // Reset ARQ synchronization
    seqNum = 0;

    debug_if(DBGMSG_L2,
             "[L2_CONNECT] switched peer -> %u (seq reset)\n",
             destId);
}

// ---------------------------------------------------------------------------
// Returns currently connected peer.
// ---------------------------------------------------------------------------
uint8_t L2_getCurrentPeer(void)
{
    return destL2ID;
}