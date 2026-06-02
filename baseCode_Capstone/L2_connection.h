// ============================================================================
// L2_connection.h
// Lightweight L2 peer/session switch interface
// ----------------------------------------------------------------------------
// Purpose:
//   The current L2 ARQ layer supports only ONE reliable peer session at a time.
//   This wrapper centralizes peer switching and resets ARQ sequence state
//   whenever the communication target changes.
//
// Usage example:
//   L2_connect(CU_ID);        // connect back to CU
//   L2_connect(studentId);   // temporary student-to-student chat
// ============================================================================

#ifndef L2_CONNECTION_H
#define L2_CONNECTION_H

#include "mbed.h"

// Change current L2 communication peer.
// This resets ARQ sequence synchronization.
void L2_connect(uint8_t destId);

// Get currently connected peer ID.
uint8_t L2_getCurrentPeer(void);

#endif // L2_CONNECTION_H