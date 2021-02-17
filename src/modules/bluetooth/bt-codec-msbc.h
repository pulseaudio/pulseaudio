#pragma once

/*
 * Parameters for use with mSBC over eSCO link
 */

#define MSBC_H2_ID0	0x01
#define MSBC_H2_ID1	0x08
#define MSBC_FRAME_SIZE	57

#define MSBC_SYNC_BYTE	0xad

struct msbc_h2_id1_s {
    uint8_t id1:4;
    uint8_t sn0:2;
    uint8_t sn1:2;
} __attribute__ ((packed));

union msbc_h2_id1 {
    struct msbc_h2_id1_s s;
    uint8_t b;
};

struct msbc_h2_header {
    uint8_t id0;
    union msbc_h2_id1 id1;
} __attribute__ ((packed));

struct msbc_frame {
    struct msbc_h2_header hdr;
    uint8_t payload[MSBC_FRAME_SIZE];
    uint8_t padding;		/* must be zero */
} __attribute__ ((packed));

#define MSBC_PACKET_SIZE	sizeof(struct msbc_frame)
