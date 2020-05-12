#include <stdio.h>
#include <stdlib.h>
#include "interval.h"

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#define ASSERT_OR_ABORT(x, mex)                                                \
    if (unlikely(!(x))) {                                                      \
        fprintf(stderr, "[interval ABORT] " mex "\n");                         \
        abort();                                                               \
    }
#define _min(a, b) (a) < (b) ? (a) : (b)
#define _max(a, b) (a) > (b) ? (a) : (b)

__attribute__((unused)) static void dump_128bit_var(__int128_t v)
{
    printf("0x%016lx:%016lx\n", (uint64_t)(v >> 64), (uint64_t)v);
}

interval_t init_interval(unsigned size)
// for 8 bit is -> [-256, 255]. Fixed at the first update
{
    ASSERT_OR_ABORT(size <= 64, "INTERVAL_H init_interval() - invalid size");

    __int128_t mask = 2;
    mask            = (mask << (size - 1)) - 1;
    interval_t res  = {.min = -(mask + 1), .max = mask, .size = size};
    return res;
}

interval_t init_signed_interval(unsigned size)
{
    ASSERT_OR_ABORT(size <= 64, "INTERVAL_H init_interval() - invalid size");

    __int128_t mask = 2;
    mask            = (mask << (size - 2)) - 1;
    interval_t res  = {.min = -(mask + 1), .max = mask, .size = size};
    return res;
}

interval_t init_unsigned_interval(unsigned size)
{
    ASSERT_OR_ABORT(size <= 64, "INTERVAL_H init_interval() - invalid size");

    __int128_t mask = 2;
    mask            = (mask << (size - 1)) - 1;
    interval_t res  = {.min = 0, .max = mask, .size = size};
    return res;
}

static int is_signed_op(optype op)
{
    return !(op == OP_ULT || op == OP_ULE || op == OP_UGT || op == OP_UGE);
}

int update_interval(interval_t* src, __int128_t c, optype op)
{
    if (!is_signed_op(op) && c < 0) {
        __uint128_t mask = (2UL << (src->size - 1)) - 1;
        c                = c & mask;
    }
    switch (op) {
        case OP_ULT:
            c--;
        case OP_ULE: {
            __int128_t tmp_min = _max(src->min, 0);
            __int128_t tmp_max = _min(src->max, c);
            if (tmp_max < tmp_min)
                return 0;
            src->min = tmp_min;
            src->max = tmp_max;
            break;
        }
        case OP_SLT:
            c--;
        case OP_SLE: {
            __int128_t max_signed = 2;
            max_signed            = (max_signed << (src->size - 2)) - 1;
            __int128_t min_signed = -(max_signed + 1);

            __int128_t tmp_min = _max(src->min, min_signed);
            __int128_t tmp_max = _min(src->max, max_signed);
            tmp_max            = _min(src->max, c);
            if (tmp_max < tmp_min)
                return 0;
            src->min = tmp_min;
            src->max = tmp_max;
            break;
        }
        case OP_UGT:
            c++;
        case OP_UGE: {
            __int128_t tmp_min = _max(src->min, c);
            if (src->max < tmp_min)
                return 0;
            src->min = tmp_min;
            break;
        }
        case OP_SGT:
            c++;
        case OP_SGE: {
            __int128_t max_signed = 2;
            max_signed            = (max_signed << (src->size - 2)) - 1;
            __int128_t min_signed = -(max_signed + 1);
            __int128_t tmp_min    = _max(src->min, min_signed);
            tmp_min               = _max(src->min, c);
            __int128_t tmp_max    = _min(src->max, max_signed);
            if (tmp_max < tmp_min)
                return 0;
            src->min = tmp_min;
            src->max = tmp_max;
            break;
        }

        default:
            ASSERT_OR_ABORT(0, "INTERVAL_H update_interval() - invalid op");
            break;
    }
    return 1;
}

int is_element_in_interval(interval_t* interval, uint64_t value)
{
    __int128_t value_ext;
    if (is_signed(interval)) {
        uint64_t mask = 1UL << (interval->size - 1UL);
        if (value & mask) {
            value |= ((2 << (64 - interval->size - 1)) - 1) << interval->size;
            __int128_t high_mask = 0xffffffffffffffffUL;
            value_ext            = (high_mask << 64) | value;
        } else
            value_ext = (__int128_t)value;
    } else
        value_ext = (__uint128_t)value;
    return value_ext >= interval->min && value_ext <= interval->max;
}

int is_signed(interval_t* interval)
{
    uint64_t high = interval->min >> 64;
    return high & 1;
}

int64_t get_signed_min(interval_t* interval)
{
    int64_t min = (int64_t)interval->min;
    return min;
}

int64_t get_signed_max(interval_t* interval)
{
    int64_t max = (int64_t)interval->max;
    return max;
}

uint64_t get_unsigned_min(interval_t* interval)
{
    uint64_t min = (uint64_t)interval->min;
    return min;
}

uint64_t get_unsigned_max(interval_t* interval)
{
    uint64_t max = (uint64_t)interval->max;
    return max;
}

uint64_t get_range(interval_t* interval)
{
    __int128_t range = interval->max - interval->min;
    return (uint64_t)range;
}

const char* op_to_string(optype op)
{
    switch (op) {
        case OP_UGT:
            return "OP_UGT";
        case OP_UGE:
            return "OP_UGE";
        case OP_SGT:
            return "OP_SGT";
        case OP_SGE:
            return "OP_SGE";
        case OP_ULT:
            return "OP_ULT";
        case OP_ULE:
            return "OP_ULE";
        case OP_SLT:
            return "OP_SLT";
        case OP_SLE:
            return "OP_SLE";

        default:
            ASSERT_OR_ABORT(0, "op_to_string() - unexpected optype");
    }
}

void print_interval(interval_t* interval)
{
    uint64_t high = interval->min >> 64;
    if (interval->size == 64 && !(high & 1)) {
        // unsigned
        uint64_t low = (uint64_t)interval->min;
        uint64_t hig = (uint64_t)interval->max;
        printf("[ %lu, %lu ] (%u)\n", low, hig, interval->size);
    } else {
        // signed
        int64_t low = (int64_t)interval->min;
        int64_t hig = (int64_t)interval->max;
        printf("[ %ld, %ld ] (%u)\n", low, hig, interval->size);
    }
}
