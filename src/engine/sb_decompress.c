/*
 * =SB= is the LH5-style stream used by Benefactor's level data.  The decoder is
 * kept native because the original routine builds and walks several Huffman
 * tables for every level; interpreting that routine is not a viable frame
 * boundary.  The format is bounded by the container's packed and unpacked
 * lengths and all reads/writes stay inside the authenticated guest image.
 */
#include "engine/sb_decompress.h"

#include "common/log.h"
#include "runtime/guest_runtime.h"

#include <stddef.h>
#include <string.h>

#define SB_DICBIT 13u
#define SB_DICSIZ (1u << SB_DICBIT)
#define SB_MAXMATCH 256u
#define SB_THRESHOLD 3u
#define SB_NC (255u + SB_MAXMATCH + 2u - SB_THRESHOLD)
#define SB_NT 19u
#define SB_NP (SB_DICBIT + 1u)
#define SB_TBIT 5u
#define SB_CBIT 9u
#define SB_PBIT 4u
#define SB_NPT 0x80u

typedef struct {
    const uint8_t *input;
    uint32_t input_size;
    uint32_t input_pos;
    uint16_t bit_buffer;
    uint8_t sub_bit_buffer;
    uint8_t bit_count;
    uint16_t left[2u * SB_NC - 1u];
    uint16_t right[2u * SB_NC - 1u];
    uint16_t character_table[4096];
    uint16_t position_table[256];
    uint8_t character_lengths[SB_NC];
    uint8_t position_lengths[SB_NPT];
    uint16_t block_size;
    uint8_t dictionary[SB_DICSIZ];
    uint32_t dictionary_position;
} SbState;

static uint32_t sb_read32(uint32_t address) {
    return ((uint32_t)g_mem[address] << 24) | ((uint32_t)g_mem[address + 1u] << 16) |
           ((uint32_t)g_mem[address + 2u] << 8) | g_mem[address + 3u];
}

static void sb_fill_bits(SbState *state, unsigned count) {
    while (count > state->bit_count) {
        count -= state->bit_count;
        state->bit_buffer = (uint16_t)((state->bit_buffer << state->bit_count) +
                                       (state->sub_bit_buffer >> (8u - state->bit_count)));
        state->sub_bit_buffer =
            state->input_pos < state->input_size ? state->input[state->input_pos++] : 0u;
        state->bit_count = 8u;
    }
    state->bit_count = (uint8_t)(state->bit_count - count);
    state->bit_buffer =
        (uint16_t)((state->bit_buffer << count) + (state->sub_bit_buffer >> (8u - count)));
    state->sub_bit_buffer = (uint8_t)(state->sub_bit_buffer << count);
}

static unsigned sb_get_bits(SbState *state, unsigned count) {
    unsigned value = state->bit_buffer >> (16u - count);
    sb_fill_bits(state, count);
    return value;
}

static unsigned sb_peek_bits(const SbState *state, unsigned count) {
    return state->bit_buffer >> (16u - count);
}

static void sb_init_bits(SbState *state) {
    state->bit_buffer = 0;
    state->sub_bit_buffer = 0;
    state->bit_count = 0;
    sb_fill_bits(state, 16u);
}

static int sb_make_table(SbState *state, unsigned count, const uint8_t *lengths,
                         unsigned table_bits, uint16_t *table) {
    uint16_t counts[17] = {0};
    uint16_t weights[17];
    uint16_t starts[17] = {0};
    uint16_t total = 0;
    unsigned i;
    int available = (int)count;
    int m = 16 - (int)table_bits;

    for (i = 1u; i <= 16u; ++i)
        weights[i] = (uint16_t)(1u << (16u - i));
    for (i = 0u; i < count; ++i) {
        if (lengths[i] > 16u) {
            benefactor_log_write(BENEFACTOR_LOG_ERROR, "sb-decrunch",
                                 "=SB= length=%u exceeds 16 at symbol=%u", lengths[i], i);
            return -1;
        }
        counts[lengths[i]]++;
    }
    for (i = 1u; i <= 16u; ++i) {
        starts[i] = total;
        total = (uint16_t)(total + weights[i] * counts[i]);
    }
    if (total != 0u) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "sb-decrunch",
                             "=SB= Huffman total=$%04X count=%u tablebits=%u", total, count,
                             table_bits);
        return -1;
    }

    for (i = 1u; i <= table_bits; ++i) {
        starts[i] >>= m;
        weights[i] >>= m;
    }
    int j = starts[table_bits + 1u] >> m;
    int table_limit = 1 << table_bits;
    if (j != 0) {
        for (i = (unsigned)j; i < (unsigned)table_limit; ++i)
            table[i] = 0;
    }
    for (j = 0; j < (int)count; ++j) {
        unsigned bits = lengths[j];
        if (bits == 0u)
            continue;
        unsigned end = starts[bits] + weights[bits];
        if (bits <= table_bits) {
            if (end > (unsigned)table_limit)
                end = (unsigned)table_limit;
            for (i = starts[bits]; i < end; ++i)
                table[i] = (uint16_t)j;
        } else {
            unsigned index = starts[bits];
            if ((index >> m) >= (unsigned)table_limit) {
                benefactor_log_write(BENEFACTOR_LOG_ERROR, "sb-decrunch",
                                     "=SB= Huffman table index=%u bits=%u limit=%u", index >> m,
                                     bits, (unsigned)table_limit);
                return -1;
            }
            uint16_t *entry = &table[index >> m];
            index <<= table_bits;
            int remaining = (int)bits - (int)table_bits;
            while (--remaining >= 0) {
                if (*entry == 0u) {
                    if (available >= (int)(2u * SB_NC - 1u))
                        return -1;
                    state->right[available] = 0;
                    state->left[available] = 0;
                    *entry = (uint16_t)available++;
                }
                entry = (index & 0x8000u) ? &state->right[*entry] : &state->left[*entry];
                index <<= 1u;
            }
            *entry = (uint16_t)j;
        }
        starts[bits] = (uint16_t)end;
    }
    return 0;
}

static int sb_read_position_lengths(SbState *state, unsigned count, unsigned bit_count,
                                    int special) {
    unsigned n = sb_get_bits(state, bit_count);
    if (n == 0u) {
        unsigned value = sb_get_bits(state, bit_count);
        for (unsigned i = 0; i < count; ++i)
            state->position_lengths[i] = 0;
        for (unsigned i = 0; i < 256u; ++i)
            state->position_table[i] = (uint16_t)value;
        return 0;
    }
    unsigned i = 0;
    while (i < n && i < SB_NPT) {
        unsigned value = sb_peek_bits(state, 3u);
        if (value != 7u) {
            sb_fill_bits(state, 3u);
        } else {
            unsigned mask = 1u << 12u;
            while ((state->bit_buffer & mask) != 0u) {
                mask >>= 1u;
                ++value;
            }
            sb_fill_bits(state, value - 3u);
        }
        state->position_lengths[i++] = (uint8_t)value;
        if ((int)i == special) {
            value = sb_get_bits(state, 2u);
            while (value-- != 0u && i < SB_NPT)
                state->position_lengths[i++] = 0;
        }
    }
    while (i < count)
        state->position_lengths[i++] = 0;
    int result = sb_make_table(state, count, state->position_lengths, 8u, state->position_table);
    return result;
}

static int sb_read_character_lengths(SbState *state) {
    unsigned n = sb_get_bits(state, SB_CBIT);
    if (n == 0u) {
        unsigned value = sb_get_bits(state, SB_CBIT);
        for (unsigned i = 0; i < SB_NC; ++i)
            state->character_lengths[i] = 0;
        for (unsigned i = 0; i < 4096u; ++i)
            state->character_table[i] = (uint16_t)value;
        return 0;
    }
    unsigned i = 0;
    while (i < n && i < SB_NC) {
        unsigned code = state->position_table[sb_peek_bits(state, 8u)];
        if (code >= SB_NT) {
            unsigned mask = 1u << 7u;
            do {
                code = (state->bit_buffer & mask) ? state->right[code] : state->left[code];
                mask >>= 1u;
            } while (code >= SB_NT && (mask != 0u || code != state->left[code]));
        }
        sb_fill_bits(state, state->position_lengths[code]);
        if (code <= 2u) {
            if (code == 0u)
                code = 1u;
            else if (code == 1u)
                code = sb_get_bits(state, 4u) + 3u;
            else
                code = sb_get_bits(state, SB_CBIT) + 20u;
            while (code-- != 0u && i < SB_NC)
                state->character_lengths[i++] = 0;
        } else {
            state->character_lengths[i++] = (uint8_t)(code - 2u);
        }
    }
    while (i < SB_NC)
        state->character_lengths[i++] = 0;
    return sb_make_table(state, SB_NC, state->character_lengths, 12u, state->character_table);
}

static int sb_decode_character(SbState *state) {
    if (state->block_size == 0u) {
        state->block_size = (uint16_t)sb_get_bits(state, 16u);
        if (sb_read_position_lengths(state, SB_NT, SB_TBIT, 3) != 0) {
            benefactor_log_write(BENEFACTOR_LOG_ERROR, "sb-decrunch",
                                 "=SB= tree-length table failed");
            return -1;
        }
        if (sb_read_character_lengths(state) != 0) {
            benefactor_log_write(BENEFACTOR_LOG_ERROR, "sb-decrunch",
                                 "=SB= character-length table failed");
            return -1;
        }
        if (sb_read_position_lengths(state, SB_NP, SB_PBIT, -1) != 0) {
            benefactor_log_write(BENEFACTOR_LOG_ERROR, "sb-decrunch",
                                 "=SB= position-length table failed");
            return -1;
        }
    }
    state->block_size--;
    unsigned code = state->character_table[sb_peek_bits(state, 12u)];
    if (code < SB_NC) {
        sb_fill_bits(state, state->character_lengths[code]);
    } else {
        sb_fill_bits(state, 12u);
        unsigned mask = 1u << 15u;
        do {
            code = (state->bit_buffer & mask) ? state->right[code] : state->left[code];
            mask >>= 1u;
        } while (code >= SB_NC && (mask != 0u || code != state->left[code]));
        sb_fill_bits(state, state->character_lengths[code] - 12u);
    }
    return (int)code;
}

static int sb_decode_position(SbState *state) {
    unsigned code = state->position_table[sb_peek_bits(state, 8u)];
    if (code < SB_NP) {
        sb_fill_bits(state, state->position_lengths[code]);
    } else {
        sb_fill_bits(state, 8u);
        unsigned mask = 1u << 15u;
        do {
            code = (state->bit_buffer & mask) ? state->right[code] : state->left[code];
            mask >>= 1u;
        } while (code >= SB_NP && (mask != 0u || code != state->left[code]));
        sb_fill_bits(state, state->position_lengths[code] - 8u);
    }
    return code == 0u ? 0 : (int)((1u << (code - 1u)) + sb_get_bits(state, code - 1u));
}

uint32_t sb_decompress(uint32_t source, uint32_t destination) {
    if (source > RT_MEM_SIZE - 12u || destination >= RT_MEM_SIZE ||
        sb_read32(source) != 0x3D53423Du)
        return 0;
    uint32_t unpacked = sb_read32(source + 4u);
    uint32_t packed = sb_read32(source + 8u);
    if (unpacked == 0u || unpacked > RT_MEM_SIZE - destination ||
        packed > RT_MEM_SIZE - (source + 12u))
        return 0;

    SbState state;
    memset(&state, 0, sizeof state);
    state.input = g_mem + source + 12u;
    state.input_size = packed;
    memset(state.dictionary, 0x20, sizeof state.dictionary);
    sb_init_bits(&state);

    uint32_t output = 0;
    while (output < unpacked) {
        int code = sb_decode_character(&state);
        if (code < 0) {
            benefactor_log_write(BENEFACTOR_LOG_ERROR, "sb-decrunch",
                                 "=SB= character table failed at output=%u input=%u", output,
                                 state.input_pos);
            return 0;
        }
        if ((unsigned)code < 256u) {
            uint8_t value = (uint8_t)code;
            g_mem[destination + output++] = value;
            state.dictionary[state.dictionary_position++ & (SB_DICSIZ - 1u)] = value;
        } else {
            unsigned length = (unsigned)code - 256u + SB_THRESHOLD;
            int position = sb_decode_position(&state);
            if (position < 0) {
                benefactor_log_write(BENEFACTOR_LOG_ERROR, "sb-decrunch",
                                     "=SB= position table failed at output=%u input=%u", output,
                                     state.input_pos);
                return 0;
            }
            unsigned source_position =
                (state.dictionary_position - (unsigned)position - 1u) & (SB_DICSIZ - 1u);
            while (length-- != 0u && output < unpacked) {
                uint8_t value = state.dictionary[source_position++ & (SB_DICSIZ - 1u)];
                g_mem[destination + output++] = value;
                state.dictionary[state.dictionary_position++ & (SB_DICSIZ - 1u)] = value;
            }
        }
    }
    return unpacked;
}
