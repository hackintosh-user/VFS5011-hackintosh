#include "timeslot.h"
#include <string.h>

bool mmis_decode_insn(const uint8_t *b, size_t len, mmis_insn_t *out) {
    if (len < 1) return false;
    uint8_t b0 = b[0];

    if (b0 == 0) { out->op = MMIS_OP_NOOP; out->size = 1; return true; }
    if (b0 == 1) { out->op = MMIS_OP_END_OF_TABLE; out->size = 1; return true; }
    if (b0 == 2) { out->op = MMIS_OP_RETURN; out->size = 1; return true; }
    if (b0 == 3) { out->op = MMIS_OP_CLEAR_SO; out->size = 1; return true; }
    if (b0 == 4) { out->op = MMIS_OP_END_OF_DATA; out->size = 1; return true; }
    if (b0 == 5) {
        if (len < 2) return false;
        out->op = MMIS_OP_MACRO; out->size = 2; out->operand[0] = b[1]; return true;
    }
    if (b0 == 6) {
        if (len < 2) return false;
        out->op = MMIS_OP_ENABLE_RX; out->size = 2; out->operand[0] = b[1]; return true;
    }
    if (b0 == 7) {
        if (len < 2) return false;
        out->op = MMIS_OP_IDLE_RX; out->size = 2;
        out->operand[0] = (b[1] == 0) ? 0x100 : b[1];
        return true;
    }
    if ((b0 & 0xfe) == 8) {
        if (len < 2) return false;
        out->op = MMIS_OP_ENABLE_SO; out->size = 2;
        out->operand[0] = ((uint32_t)(b0 & 1) << 8) | b[1];
        return true;
    }
    if ((b0 & 0xfe) == 0xa) {
        if (len < 2) return false;
        out->op = MMIS_OP_DISABLE_SO; out->size = 2;
        out->operand[0] = ((uint32_t)(b0 & 1) << 8) | b[1];
        return true;
    }
    if ((b0 & 0xfc) == 0xc) {
        out->op = MMIS_OP_INTERRUPT; out->size = 1;
        out->operand[0] = b0 & 3;
        return true;
    }
    if ((b0 & 0xf8) == 0x10) {
        if (len < 3) return false;
        out->op = MMIS_OP_CALL; out->size = 3;
        out->operand[0] = b0 & 7;
        out->operand[1] = (uint32_t)b[1] << 2;
        out->operand[2] = (b[2] == 0) ? 0x100 : b[2];
        return true;
    }
    if ((b0 & 0xe0) == 0x20) {
        out->op = MMIS_OP_FEATURES; out->size = 1;
        out->operand[0] = b0 & 0x1f;
        return true;
    }
    if ((b0 & 0xc0) == 0x40) {
        if (len < 3) return false;
        out->op = MMIS_OP_REGISTER_WRITE; out->size = 3;
        out->operand[0] = (uint32_t)(b0 & 0x3f) * 4 + 0x80002000u;
        out->operand[1] = (uint32_t)b[1] | ((uint32_t)b[2] << 8);
        return true;
    }
    if ((b0 & 0xc0) == 0x80) {
        out->op = MMIS_OP_SAMPLE; out->size = 1;
        out->operand[0] = (b0 & 0x38) >> 3;
        out->operand[1] = b0 & 7;
        return true;
    }
    if ((b0 & 0xc0) == 0xc0) {
        if (len < 2) return false;
        out->op = MMIS_OP_SAMPLE_REPEAT; out->size = 2;
        out->operand[0] = (b0 & 0x38) >> 3;
        out->operand[1] = b0 & 7;
        out->operand[2] = (b[1] == 0) ? 0x100 : b[1];
        return true;
    }

    /* 0x18-0x1f: genuinely unhandled, matches upstream's raised Exception */
    return false;
}

bool mmis_find_nth_insn(const uint8_t *b, size_t len, mmis_opcode_t opcode, int n,
                         size_t *pc_out, size_t *insn_len_out) {
    size_t pc = 0;
    while (pc < len) {
        mmis_insn_t insn;
        if (!mmis_decode_insn(b + pc, len - pc, &insn)) return false;
        if (insn.size > len - pc) return false;
        if (insn.op == opcode) {
            n -= 1;
            if (n == 0) {
                *pc_out = pc;
                *insn_len_out = insn.size;
                return true;
            }
        }
        pc += insn.size;
    }
    return false;
}

bool mmis_find_nth_regwrite(const uint8_t *b, size_t len, uint32_t reg_addr, int n,
                             size_t *pc_out, size_t *insn_len_out) {
    size_t pc = 0;
    while (pc < len) {
        mmis_insn_t insn;
        if (!mmis_decode_insn(b + pc, len - pc, &insn)) return false;
        if (insn.size > len - pc) return false;
        if (insn.op == MMIS_OP_REGISTER_WRITE && insn.operand[0] == reg_addr) {
            n -= 1;
            if (n == 0) {
                *pc_out = pc;
                *insn_len_out = insn.size;
                return true;
            }
        }
        pc += insn.size;
    }
    return false;
}

size_t mmis_split_chunks(const uint8_t *b, size_t len, mmis_chunk_t *out, size_t max_chunks) {
    size_t off = 0;
    size_t n = 0;
    while (off < len) {
        if (len - off < 4) return (size_t)-1; /* malformed: truncated header */
        uint16_t tag = (uint16_t)b[off] | ((uint16_t)b[off + 1] << 8);
        uint16_t sz  = (uint16_t)b[off + 2] | ((uint16_t)b[off + 3] << 8);
        off += 4;
        if (len - off < sz) return (size_t)-1; /* malformed: truncated payload */
        if (n < max_chunks) {
            out[n].tag = tag;
            out[n].data = b + off;
            out[n].len = sz;
        }
        n++;
        off += sz;
    }
    return n;
}

size_t mmis_merge_chunks(const mmis_chunk_t *chunks, size_t n_chunks, uint8_t *out, size_t out_max) {
    size_t off = 0;
    for (size_t i = 0; i < n_chunks; i++) {
        size_t need = 4 + chunks[i].len;
        if (off + need > out_max) return 0;
        out[off]     = (uint8_t)(chunks[i].tag & 0xff);
        out[off + 1] = (uint8_t)(chunks[i].tag >> 8);
        out[off + 2] = (uint8_t)(chunks[i].len & 0xff);
        out[off + 3] = (uint8_t)(chunks[i].len >> 8);
        off += 4;
        memcpy(out + off, chunks[i].data, chunks[i].len);
        off += chunks[i].len;
    }
    return off;
}

void mmis_patch_timeslot_table(uint8_t *buf, size_t len, bool inc_address, uint8_t mult) {
    size_t i = 0;
    while (i + 3 < len) {
        if ((buf[i] & 0xf8) == 0x10) {
            if (buf[i + 2] > 1) {
                buf[i + 2] = (uint8_t)(buf[i + 2] * mult);
            }
            if (inc_address) {
                buf[i + 1] = (uint8_t)(buf[i + 1] + 1);
            }
            i += 3;
            continue;
        }
        if (buf[i] == 0) {
            i += 1;
            continue;
        }
        if (buf[i] == 7) {
            i += 2;
            continue;
        }
        break;
    }
}

bool mmis_patch_timeslot_again(uint8_t *buf, size_t len,
                                const uint8_t *factory_calibration_values,
                                size_t factory_calibration_values_len,
                                uint8_t key_calibration_line) {
    /* Pass 1: find the last Call before End-of-Table/Return/End-of-Data */
    size_t pc = 0;
    bool have_dest = false;
    size_t dest = 0;
    while (pc < len) {
        mmis_insn_t insn;
        if (!mmis_decode_insn(buf + pc, len - pc, &insn)) return false;
        if (insn.op == MMIS_OP_END_OF_TABLE || insn.op == MMIS_OP_RETURN ||
            insn.op == MMIS_OP_END_OF_DATA) {
            break;
        }
        if (insn.op == MMIS_OP_CALL) {
            dest = insn.operand[1]; /* destination address, b[1]<<2 */
            have_dest = true;
        }
        pc += insn.size;
    }
    if (!have_dest || dest >= len) return false;

    /* Pass 2: within [dest, ...), find the last Register Write to 0x8000203C */
    pc = dest;
    bool have_match = false;
    size_t match_pc = 0;
    while (pc < len) {
        mmis_insn_t insn;
        if (!mmis_decode_insn(buf + pc, len - pc, &insn)) return false;
        if (insn.op == MMIS_OP_END_OF_TABLE || insn.op == MMIS_OP_RETURN ||
            insn.op == MMIS_OP_END_OF_DATA) {
            break;
        }
        if (insn.op == MMIS_OP_REGISTER_WRITE && insn.operand[0] == 0x8000203cu) {
            match_pc = pc;
            have_match = true;
        }
        pc += insn.size;
    }
    if (!have_match) return false;
    if (key_calibration_line >= factory_calibration_values_len) return false;
    if (match_pc + 1 >= len) return false;

    buf[match_pc + 1] = factory_calibration_values[key_calibration_line];
    return true;
}

size_t mmis_bitpack(const uint8_t *values, size_t count,
                     uint8_t *u_out, uint8_t *min_out,
                     uint8_t *out, size_t out_max) {
    if (count == 0) { *u_out = 0; *min_out = 0; return 0; }

    uint8_t mn = values[0], mx = values[0];
    for (size_t i = 1; i < count; i++) {
        if (values[i] < mn) mn = values[i];
        if (values[i] > mx) mx = values[i];
    }
    unsigned x = (unsigned)(mx - mn);
    uint8_t u = 0;
    while (x > 0) { x >>= 1; u++; }
    *u_out = u;
    *min_out = mn;
    if (u == 0) return 0;

    size_t total_bits = (size_t)u * count;
    size_t total_bytes = (total_bits + 7) / 8;
    if (total_bytes > out_max) return (size_t)-1;
    memset(out, 0, total_bytes);

    for (size_t i = 0; i < count; i++) {
        uint32_t v = (uint32_t)(values[i] - mn);
        size_t bit_base = (size_t)u * i;
        for (uint8_t j = 0; j < u; j++) {
            if (v & (1u << j)) {
                size_t bitpos = bit_base + j;
                out[bitpos / 8] |= (uint8_t)(1u << (bitpos % 8));
            }
        }
    }
    return total_bytes;
}

void mmis_get_key_line(const uint8_t *calib_data, size_t calib_data_len,
                        size_t lines_per_calibration_data, size_t key_calibration_line,
                        size_t line_width, uint8_t *out) {
    if (calib_data_len > 0) {
        size_t bytes_per_calibration_line = calib_data_len / lines_per_calibration_data;
        size_t key_line_offset = 8 + bytes_per_calibration_line * key_calibration_line;
        for (size_t i = 0; i < line_width; i++) {
            uint8_t v = calib_data[key_line_offset + i];
            out[i] = (v == 5) ? 4 : v;
        }
    } else {
        memset(out, 0, line_width);
    }
}

/* Internal Line representation, mirrors sensor.py's Line class. */
typedef struct {
    uint32_t mask;
    uint32_t flags;
    const uint8_t *data;
    size_t data_len;
    uint8_t v0, v1;
    uint16_t v2;
} mmis__line_t;

static const uint8_t MMIS_IDENTIFY_4E[] = {
    0xfb,0xb2,0x0f,0x00,0x00,0x00,0x0f,0x00,0x30,0x00,0x00,0x00,0x87,0x00,0x02,0x00,
    0x67,0x00,0x0a,0x00,0x01,0x80,0x00,0x00,0x0a,0x02,0x00,0x00,0x0b,0x19,0x00,0x00,
    0x88,0x13,0xb8,0x0b,0x01,0x09,0x10,0x00
};
static const uint8_t MMIS_IDENTIFY_2E[] = {
    0x02,0x00,0x18,0x00,0x02,0x00,0x00,0x00,0x70,0x00,0x70,0x00,0x4d,0x01,0x00,0x00,
    0xa0,0x00,0x8c,0x00,0x3c,0x32,0x32,0x1e,0x3c,0x0a,0x02,0x02
};
static const uint8_t MMIS_ENROLL_26[] = {
    0xfb,0xb2,0x0f,0x00,0x00,0x00,0x0f,0x00,0x30,0x00,0x00,0x00,0x87,0x00,0x02,0x00,
    0x67,0x00,0x0a,0x00,0x01,0x80,0x00,0x00,0x0a,0x02,0x00,0x00,0x0b,0x19,0x00,0x00,
    0x50,0xc3,0x60,0xea,0x01,0x09,0x10,0x00
};
static const uint8_t MMIS_ENROLL_2E[] = {
    0x02,0x00,0x18,0x00,0x23,0x00,0x00,0x00,0x70,0x00,0x70,0x00,0x4d,0x01,0x00,0x00,
    0xa0,0x00,0x8c,0x00,0x3c,0x32,0x32,0x1e,0x3c,0x0a,0x02,0x02
};

static uint8_t *scratch_alloc(uint8_t **cursor, uint8_t *scratch_end, size_t n) {
    if (*cursor + n > scratch_end) return NULL;
    uint8_t *p = *cursor;
    *cursor += n;
    return p;
}

bool mmis_line_update_type_1(mmis_capture_mode_t mode,
                              mmis_chunk_t *chunks_io, size_t *n_chunks_io, size_t max_chunks,
                              uint8_t repeat_multiplier, uint8_t key_calibration_line,
                              const uint8_t *factory_calibration_values, size_t factory_calibration_values_len,
                              const uint8_t *calib_data, size_t calib_data_len,
                              size_t lines_per_calibration_data, size_t line_width,
                              const uint8_t *calibration_blob, size_t calibration_blob_len,
                              uint8_t *scratch, size_t scratch_len) {
    uint8_t *cursor = scratch;
    uint8_t *scratch_end = scratch + scratch_len;
    size_t n = *n_chunks_io;

    /* 1. locate and patch the 0x34 chunk */
    size_t idx34 = (size_t)-1;
    for (size_t i = 0; i < n; i++) {
        if (chunks_io[i].tag == 0x34) { idx34 = i; break; }
    }
    if (idx34 == (size_t)-1) return false;

    size_t tst_len = chunks_io[idx34].len;
    uint8_t *tst = scratch_alloc(&cursor, scratch_end, tst_len);
    if (!tst) return false;
    memcpy(tst, chunks_io[idx34].data, tst_len);

    mmis_patch_timeslot_table(tst, tst_len, true, repeat_multiplier);
    if (mode != MMIS_CAPTURE_CALIBRATE) {
        /* failure here is non-fatal, mirrors sensor.py returning the buffer unmodified */
        mmis_patch_timeslot_again(tst, tst_len, factory_calibration_values,
                                   factory_calibration_values_len, key_calibration_line);
    }

    if (line_width > tst_len) return false;
    uint8_t *new_c1 = scratch_alloc(&cursor, scratch_end, tst_len);
    if (!new_c1) return false;
    mmis_get_key_line(calib_data, calib_data_len, lines_per_calibration_data,
                       key_calibration_line, line_width, new_c1);
    memcpy(new_c1 + line_width, tst + line_width, tst_len - line_width);
    chunks_io[idx34].data = new_c1;
    chunks_io[idx34].len = (uint16_t)tst_len;

    /* 2. append fixed fragments */
    if (n >= max_chunks) return false;
    chunks_io[n].tag = 0x17; chunks_io[n].data = NULL; chunks_io[n].len = 0; n++;

    if (mode == MMIS_CAPTURE_IDENTIFY) {
        if (n + 2 > max_chunks) return false;
        chunks_io[n].tag = 0x4e; chunks_io[n].data = MMIS_IDENTIFY_4E; chunks_io[n].len = sizeof(MMIS_IDENTIFY_4E); n++;
        chunks_io[n].tag = 0x2e; chunks_io[n].data = MMIS_IDENTIFY_2E; chunks_io[n].len = sizeof(MMIS_IDENTIFY_2E); n++;
    } else if (mode == MMIS_CAPTURE_ENROLL) {
        if (n + 2 > max_chunks) return false;
        chunks_io[n].tag = 0x26; chunks_io[n].data = MMIS_ENROLL_26; chunks_io[n].len = sizeof(MMIS_ENROLL_26); n++;
        chunks_io[n].tag = 0x2e; chunks_io[n].data = MMIS_ENROLL_2E; chunks_io[n].len = sizeof(MMIS_ENROLL_2E); n++;
    }

    uint8_t *interleave_val = scratch_alloc(&cursor, scratch_end, 4);
    if (!interleave_val) return false;
    interleave_val[0] = 1; interleave_val[1] = 0; interleave_val[2] = 0; interleave_val[3] = 0;
    if (n >= max_chunks) return false;
    chunks_io[n].tag = 0x44; chunks_io[n].data = interleave_val; chunks_io[n].len = 4; n++;

    /* 3. build the Line list */
    mmis__line_t lines[64];
    size_t n_lines = 0;
    uint32_t cnt = 2;

    size_t pc0, ilen0;
    if (!mmis_find_nth_insn(tst, tst_len, MMIS_OP_ENABLE_RX, 2, &pc0, &ilen0)) return false;
    lines[n_lines].mask = 0xff;
    lines[n_lines].flags = (uint32_t)((pc0 + 1) | (cnt << 0x14) | 0x7000000u);
    lines[n_lines].data = calibration_blob;
    lines[n_lines].data_len = calibration_blob_len;
    lines[n_lines].v0 = 0xf; lines[n_lines].v1 = 0; lines[n_lines].v2 = 0;
    n_lines++; cnt++;

    size_t pc1, ilen1;
    if (!mmis_find_nth_regwrite(tst, tst_len, 0x8000203Cu, 1, &pc1, &ilen1)) return false;
    uint8_t *packed = scratch_alloc(&cursor, scratch_end, factory_calibration_values_len + 1);
    if (!packed) return false;
    uint8_t u, mn;
    size_t packed_len = mmis_bitpack(factory_calibration_values, factory_calibration_values_len,
                                      &u, &mn, packed, factory_calibration_values_len + 1);
    if (packed_len == (size_t)-1) return false;
    lines[n_lines].mask = 0xff;
    lines[n_lines].flags = (uint32_t)((pc1 + 1) | (cnt << 0x14) | 0x7000000u);
    lines[n_lines].data = packed;
    lines[n_lines].data_len = packed_len;
    lines[n_lines].v0 = (uint8_t)((u - 1) | 8);
    lines[n_lines].v1 = mn;
    lines[n_lines].v2 = 0;
    n_lines++; cnt++;

    if (calib_data_len > 0) {
        size_t bytes_per_calibration_line = calib_data_len / lines_per_calibration_data;
        for (size_t i = 0; i < 112; i += 4) {
            if (n_lines >= 64) return false;
            uint8_t *coldata = scratch_alloc(&cursor, scratch_end, 112 * 4);
            if (!coldata) return false;
            size_t off = 0;
            for (size_t j = 0; j < 112; j++) {
                size_t p = 8 + j * bytes_per_calibration_line + i;
                memcpy(coldata + off, calib_data + p, 4);
                off += 4;
            }
            lines[n_lines].mask = 0xffffffffu;
            lines[n_lines].flags = (uint32_t)(i | (0x85u << 24));
            lines[n_lines].data = coldata;
            lines[n_lines].data_len = 112 * 4;
            lines[n_lines].v0 = 0; lines[n_lines].v1 = 0; lines[n_lines].v2 = 0;
            n_lines++;
        }
    }

    /* pad each line's data to a multiple of 4 (dword alignment) */
    for (size_t i = 0; i < n_lines; i++) {
        size_t pad = lines[i].data_len % 4;
        if (pad > 0) {
            size_t newlen = lines[i].data_len + (4 - pad);
            uint8_t *nb = scratch_alloc(&cursor, scratch_end, newlen);
            if (!nb) return false;
            memcpy(nb, lines[i].data, lines[i].data_len);
            memset(nb + lines[i].data_len, 0, 4 - pad);
            lines[i].data = nb;
            lines[i].data_len = newlen;
        }
    }

    /* ---------------- Line Update (0x30) ---------------- */
    size_t lu_header_len = 4 + n_lines * 8;
    size_t lu_data_len = 0;
    for (size_t i = 0; i < n_lines; i++) {
        if (((lines[i].flags & 0x00f00000u) >> 0x14) <= 1) lu_data_len += lines[i].data_len;
    }
    uint8_t *lu = scratch_alloc(&cursor, scratch_end, lu_header_len + lu_data_len);
    if (!lu) return false;
    {
        size_t off = 0;
        lu[off++] = (uint8_t)(n_lines & 0xff); lu[off++] = (uint8_t)((n_lines>>8)&0xff);
        lu[off++] = (uint8_t)((n_lines>>16)&0xff); lu[off++] = (uint8_t)((n_lines>>24)&0xff);
        for (size_t i = 0; i < n_lines; i++) {
            uint32_t m = lines[i].mask, f = lines[i].flags;
            lu[off++] = (uint8_t)(m); lu[off++] = (uint8_t)(m>>8); lu[off++] = (uint8_t)(m>>16); lu[off++] = (uint8_t)(m>>24);
            lu[off++] = (uint8_t)(f); lu[off++] = (uint8_t)(f>>8); lu[off++] = (uint8_t)(f>>16); lu[off++] = (uint8_t)(f>>24);
        }
        for (size_t i = 0; i < n_lines; i++) {
            if (((lines[i].flags & 0x00f00000u) >> 0x14) <= 1) {
                memcpy(lu + off, lines[i].data, lines[i].data_len);
                off += lines[i].data_len;
            }
        }
    }
    if (n >= max_chunks) return false;
    chunks_io[n].tag = 0x30; chunks_io[n].data = lu; chunks_io[n].len = (uint16_t)(lu_header_len + lu_data_len); n++;

    /* ---------------- Line Update Transform (0x43) ---------------- */
    size_t ut_len = 0;
    for (size_t i = 0; i < n_lines; i++) {
        if (((lines[i].flags & 0x00f00000u) >> 0x14) > 1) ut_len += 4 + lines[i].data_len;
    }
    uint8_t *ut = scratch_alloc(&cursor, scratch_end, ut_len);
    if (!ut) return false;
    {
        size_t off = 0;
        for (size_t i = 0; i < n_lines; i++) {
            if (((lines[i].flags & 0x00f00000u) >> 0x14) > 1) {
                ut[off++] = lines[i].v0;
                ut[off++] = lines[i].v1;
                ut[off++] = (uint8_t)(lines[i].v2 & 0xff);
                ut[off++] = (uint8_t)(lines[i].v2 >> 8);
                memcpy(ut + off, lines[i].data, lines[i].data_len);
                off += lines[i].data_len;
            }
        }
    }
    if (n >= max_chunks) return false;
    chunks_io[n].tag = 0x43; chunks_io[n].data = ut; chunks_io[n].len = (uint16_t)ut_len; n++;

    *n_chunks_io = n;
    return true;
}
