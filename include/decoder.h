#ifndef DECODER_H
#define DECODER_H

#define X86_MAX_INS_LEN     15u 
#define X86_MAX_OPCODE_LEN  3u 

typedef enum {
    /* Group 1 */
    X86_PREFIX_LOCK  = 0xF0,
    X86_PREFIX_REPNE = 0xF2,
    X86_PREFIX_REP   = 0xF3,
 
    /* Group 2 — segment override */
    X86_PREFIX_CS    = 0x2E,
    X86_PREFIX_SS    = 0x36,
    X86_PREFIX_DS    = 0x3E,
    X86_PREFIX_ES    = 0x26,
    X86_PREFIX_FS    = 0x64,
    X86_PREFIX_GS    = 0x65,
 
    /* Group 3 — operand size */
    X86_PREFIX_OPSIZE = 0x66,
 
    /* Group 4 — address size */
    X86_PREFIX_ADDRSIZE = 0x67,
} x86_prefix_byte_t;

typdef uint16 x86_prefix_mask_t; 

#define X86_PREFIX_MASK_LOCK      (1u << 0)
#define X86_PREFIX_MASK_REPNE     (1u << 1)
#define X86_PREFIX_MASK_REP       (1u << 2)
#define X86_PREFIX_MASK_CS        (1u << 3)
#define X86_PREFIX_MASK_SS        (1u << 4)
#define X86_PREFIX_MASK_DS        (1u << 5)
#define X86_PREFIX_MASK_ES        (1u << 6)
#define X86_PREFIX_MASK_FS        (1u << 7)
#define X86_PREFIX_MASK_GS        (1u << 8)
#define X86_PREFIX_MASK_OPSIZE    (1u << 9)
#define X86_PREFIX_MASK_ADDRSIZE  (1u << 10)

typedef struct {
    bool     present;   /**< True if any byte in 0x40–0x4F was seen.       */
    bool     w;         /**< REX.W — 64-bit operand size.                   */
    bool     r;         /**< REX.R — extend ModRM.reg to 4 bits.            */
    bool     x;         /**< REX.X — extend SIB.index to 4 bits.            */
    bool     b;         /**< REX.B — extend ModRM.r/m / SIB.base / op-reg. */
    uint8_t  raw;       /**< The raw REX byte (0x40–0x4F), or 0 if absent. */
} x86_rex_t;

#define REX_W(byte)  (((byte) >> 3) & 1)
#define REX_R(byte)  (((byte) >> 2) & 1)
#define REX_X(byte)  (((byte) >> 1) & 1)
#define REX_B(byte)  (((byte) >> 0) & 1)

/** Test whether a byte is a REX prefix (0x40–0x4F). */
#define IS_REX(byte)  (((byte) & 0xF0) == 0x40)

typedef enum {
    X86_VEX_NONE  = 0,  /**< No VEX prefix.                               */
    X86_VEX_2BYTE = 1,  /**< 2-byte form (C5h): limited extension.        */
    X86_VEX_3BYTE = 2,  /**< 3-byte form (C4h): full extension.           */
    X86_EVEX      = 3,  /**< 4-byte EVEX (62h): AVX-512.                  */
} x86_vex_kind_t;


#endif // 
