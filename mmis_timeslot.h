#ifndef MMIS_TIMESLOT_H
#define MMIS_TIMESLOT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Mirrors sensor.py's decode_insn() opcodes 0-15 (the *decoded* opcode,
 * not the raw instruction byte -- multiple byte patterns map to the same
 * decoded opcode via bitmasking, exactly as upstream does). */
typedef enum {
    MMIS_OP_NOOP            = 0,
    MMIS_OP_END_OF_TABLE    = 1,
    MMIS_OP_RETURN          = 2,
    MMIS_OP_CLEAR_SO        = 3,
    MMIS_OP_END_OF_DATA     = 4,
    MMIS_OP_MACRO           = 5,
    MMIS_OP_ENABLE_RX       = 6,
    MMIS_OP_IDLE_RX         = 7,
    MMIS_OP_ENABLE_SO       = 8,
    MMIS_OP_DISABLE_SO      = 9,
    MMIS_OP_INTERRUPT       = 10,
    MMIS_OP_CALL            = 11,
    MMIS_OP_FEATURES        = 12,
    MMIS_OP_REGISTER_WRITE  = 13,
    MMIS_OP_SAMPLE          = 14,
    MMIS_OP_SAMPLE_REPEAT   = 15,
} mmis_opcode_t;

typedef struct {
    mmis_opcode_t op;
    size_t        size;      /* bytes consumed by this instruction */
    uint32_t      operand[3];/* meaning depends on op, mirrors the *operands tuple */
} mmis_insn_t;

/* Decodes one instruction at b[0]. Returns false on an unhandled byte
 * (mirrors sensor.py raising Exception('Unhandled instruction ...')). */
bool mmis_decode_insn(const uint8_t *b, size_t len, mmis_insn_t *out);

/* find_nth_insn(): pc,len of the n-th (1-indexed) instruction with the given
 * decoded opcode. Returns false if not found (mirrors Python returning None). */
bool mmis_find_nth_insn(const uint8_t *b, size_t len, mmis_opcode_t opcode, int n,
                         size_t *pc_out, size_t *insn_len_out);

/* find_nth_regwrite(): same but specifically for REGISTER_WRITE to reg_addr. */
bool mmis_find_nth_regwrite(const uint8_t *b, size_t len, uint32_t reg_addr, int n,
                             size_t *pc_out, size_t *insn_len_out);

/* One chunk from split_chunks(): tag + payload pointer/len into the original buffer. */
typedef struct {
    uint16_t       tag;
    const uint8_t *data;
    uint16_t       len;
} mmis_chunk_t;

/* split_chunks(): parses <HH tag,len><payload> repeated. Returns number of
 * chunks written to out (up to max_chunks), or (size_t)-1 on malformed input. */
size_t mmis_split_chunks(const uint8_t *b, size_t len, mmis_chunk_t *out, size_t max_chunks);

/* merge_chunks(): inverse of split_chunks -- packs chunks back into
 * <HH tag,len><payload> form. Returns bytes written, or 0 if out_max too small. */
size_t mmis_merge_chunks(const mmis_chunk_t *chunks, size_t n_chunks, uint8_t *out, size_t out_max);

#endif /* MMIS_TIMESLOT_H */
