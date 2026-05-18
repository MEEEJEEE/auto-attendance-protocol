#define DBGMSG_L2                       0   //debug print control
#define DBGMSG_L3                       0   //debug print control

#define L3_MAXDATASIZE                  1024


#define L2_ARQ_MAXRETRANSMISSION        10
#define L2_ARQ_MAXWAITTIME              5
#define L2_ARQ_MINWAITTIME              2
#define L1_FREQCHANNEL                  1   // x 1MHz

// ---------------------------------------------------------------------------
// CU (Control Unit) protocol parameters
// ---------------------------------------------------------------------------

// RSSI boundary in dBm: a received LOCATION packet with RSSI >= this value
// indicates the student is within classroom range and is marked present.
#define L3_RSSI_THRESHOLD               -80

// Total duration of the attendance window in seconds (default: 10 minutes).
// The CU transitions OPEN -> CLOSED when this timer expires.
#define L3_ATTEND_WINDOW_SEC            600

// Seconds before the deadline at which the CU broadcasts an absence alert.
// Must be less than L3_ATTEND_WINDOW_SEC (default: 5 minutes).
#define L3_PRE_DEADLINE_ALERT_SEC       300

// L2 destination ID used for broadcast transmissions (reaches all students).
#define L3_BROADCAST_ID                 255

// Maximum number of distinct student IDs the CU attendance table can track.
#define L3_MAX_STUDENTS                 32
