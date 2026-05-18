#include "mbed.h"

// ---------------------------------------------------------------------------
// L3 packet type identifiers
// The first byte of every L3 payload must be one of these values so that the
// receiver can route the packet to the correct handler.
//
//   Student -> CU : LOCATION, CHAT
//   CU -> Students: NOTIFY
// ---------------------------------------------------------------------------
#define L3_PKT_TYPE_LOCATION    0x01  // periodic location signal sent by the student node
#define L3_PKT_TYPE_CHAT        0x02  // chat message body sent by an attending student
#define L3_PKT_TYPE_NOTIFY      0x03  // system notification broadcast from the CU

// ---------------------------------------------------------------------------
// NOTIFY packet subtype codes (second byte of a NOTIFY payload)
// ---------------------------------------------------------------------------
#define L3_NOTIFY_DEADLINE_ALERT    0x01  // absence warning: attendance closes soon
#define L3_NOTIFY_SESSION_CLOSED    0x02  // attendance window has ended; chat is terminated

// ---------------------------------------------------------------------------
// Packet layout reference (byte offsets within the L3 payload)
//
//   LOCATION packet  [0] type=0x01
//                    RSSI is read from the L2 metadata, not from the payload.
//
//   CHAT packet      [0] type=0x02
//                    [1..] null-terminated message string
//
//   NOTIFY packet    [0] type=0x03
//                    [1] notification subtype (L3_NOTIFY_*)
// ---------------------------------------------------------------------------
