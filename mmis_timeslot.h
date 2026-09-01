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

/* ---- patch_timeslot_table / patch_timeslot_again ------------------------
 * Ports of Sensor.patch_timeslot_table()/patch_timeslot_again() from
 * sensor.py. Both mutate a copy of the Timeslot Table 2D (tag 0x34) chunk
 * in place before it's re-packed into the outgoing cmd_02.
 * -------------------------------------------------------------------- */

/* Scans from the start of the buffer patching Call instructions
 * (b[i]&0xf8==0x10): repeat *= mult (if >1), address += 1 (if inc_address),
 * skipping over leading NOOP (0x00) and Idle Rx (0x07 xx) instructions.
 * Stops at the first byte that isn't Call/NOOP/IdleRx (mirrors the `break`
 * in sensor.py -- this is NOT a full-buffer decode, only the leading run).
 * buf is mutated in place. */
void mmis_patch_timeslot_table(uint8_t *buf, size_t len, bool inc_address, uint8_t mult);

/* Finds the last Call instruction's destination (jump target) before the
 * first End-of-Table/Return/End-of-Data, then within the block starting at
 * that destination, finds the last Register Write to 0x8000203C and patches
 * its low value byte to factory_calibration_values[key_calibration_line].
 * buf is mutated in place. Returns false if either scan finds nothing
 * (buffer is left unmodified, mirrors sensor.py returning bytes(b) as-is). */
bool mmis_patch_timeslot_again(uint8_t *buf, size_t len,
                                const uint8_t *factory_calibration_values,
                                size_t factory_calibration_values_len,
                                uint8_t key_calibration_line);

/* ---- bitpack() -----------------------------------------------------------
 * Ports Sensor's module-level bitpack(): packs `count` bytes into a
 * little-endian, LSB-first bitstream where value[i] occupies bits
 * [u*i, u*i+u), each stored as (value[i] - min). u is the minimum number of
 * bits needed to represent (max-min). Returns u via *u_out, min via
 * *min_out, and writes ceil(u*count/8) bytes to out (out_max must be large
 * enough -- caller sizes it as (count+1) to be safe). Returns bytes written.
 * ------------------------------------------------------------------------ */
size_t mmis_bitpack(const uint8_t *values, size_t count,
                     uint8_t *u_out, uint8_t *min_out,
                     uint8_t *out, size_t out_max);

/* ---- get_key_line() -------------------------------------------------------
 * Ports Sensor.get_key_line(). If calib_data is non-empty, extracts the
 * `key_calibration_line`-th row (each row is len(calib_data)/lines_per_cal
 * bytes wide, first 8 bytes are a per-line header skipped here), takes
 * `line_width` bytes from offset 8 into that row, and replaces any byte
 * equal to 5 with 4 (sensor.py's `i - 1 if i == 5 else i` quirk). If
 * calib_data is empty, returns line_width zero bytes (pre-calibration case).
 * Writes line_width bytes to out. -------------------------------------- */
void mmis_get_key_line(const uint8_t *calib_data, size_t calib_data_len,
                        size_t lines_per_calibration_data, size_t key_calibration_line,
                        size_t line_width, uint8_t *out);

/* ---- line_update_type_1() ------------------------------------------------
 * Ports Sensor.line_update_type_1() for devices in line_update_type1_devices
 * (0x199 among them). Consumes the already-split chunk list (must contain
 * exactly one 0x34 Timeslot Table 2D chunk), mutates it in place (patches +
 * truncates the 0x34 payload per get_key_line()), appends the mode-specific
 * fixed fragments (0x17/0x4e-or-0x26/0x2e/0x44), builds the Line list
 * (calibration-blob line + bitpacked-factory-values line +, if calib_data is
 * non-empty, 28 additional 448-byte column lines), and appends the final
 * 0x30 (Line Update) and 0x43 (Line Update Transform) chunks.
 *
 * chunks_io is modified in place; *n_chunks_io is updated to the new count
 * (grows by up to 6). chunk_buf_pool/chunk_buf_pool_len is scratch space the
 * function carves new chunk payloads out of (must outlive chunks_io, since
 * chunk data pointers point into it) -- size it generously, a few KB is
 * plenty for one capture-program's worth of chunks.
 * Returns false on any internal inconsistency (missing 0x34 chunk, pool
 * exhausted, decode failure propagated from the timeslot patch functions). */
typedef enum { MMIS_CAPTURE_CALIBRATE, MMIS_CAPTURE_IDENTIFY, MMIS_CAPTURE_ENROLL } mmis_capture_mode_t;

bool mmis_line_update_type_1(mmis_capture_mode_t mode,
                              mmis_chunk_t *chunks_io, size_t *n_chunks_io, size_t max_chunks,
                              uint8_t repeat_multiplier, uint8_t key_calibration_line,
                              const uint8_t *factory_calibration_values, size_t factory_calibration_values_len,
                              const uint8_t *calib_data, size_t calib_data_len,
                              size_t lines_per_calibration_data, size_t line_width,
                              const uint8_t *calibration_blob, size_t calibration_blob_len,
                              uint8_t *scratch, size_t scratch_len);
