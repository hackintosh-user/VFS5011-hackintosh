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
