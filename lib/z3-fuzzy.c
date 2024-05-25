#define FUZZY_SOURCE

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include "gradient_descend.h"
#include "wrapped_interval.h"
#include "timer.h"
#include "z3-fuzzy.h"

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#define ASSERT_OR_ABORT(x, mex)                                                \
    if (unlikely(!(x))) {                                                      \
        fprintf(stderr, "[z3fuzz ABORT] " mex "\n");                           \
        abort();                                                               \
    }
#define ABORT(mex)                                                             \
    do {                                                                       \
        fprintf(stderr, "[z3fuzz ABORT] " mex "\n");                           \
        abort();                                                               \
    } while (0);
#define Z3FUZZ_LOG(x...) fprintf(stderr, "[z3fuzz] " x)

#define FLIP_BIT(_var, _idx) ((_var) ^ (1 << (_idx)));
#define rightmost_set_bit(x) ((x) != 0 ? __builtin_ctzl(x) : -1)
#define leftmost_set_bit(x) ((x) != 0 ? (63 - __builtin_clzl(x)) : -1)

#define HAVOC_STACK_POW2 7
#define HAVOC_C 20
#define RANGE_MAX_WIDTH_BRUTE_FORCE 2048
#define Z3_UNIQUE Z3_get_ast_hash // Z3_get_ast_id

// #define PRINT_SAT
// #define DEBUG_RANGE
// #define DEBUG_CHECK_LIGHT
// #define DEBUG_DETECT_GROUP

// #define SKIP_IS_VALID_EVAL
// #define USE_MD5_HASH

// #define USE_HAVOC_ON_WHOLE_PI
// #define USE_HAVOC_MOD
#define USE_AFL_DET_GROUPS
#define ENABLE_AGGRESSIVE_OPTIMISTIC
#define AVOID_GD_FALLBACK 0

static int log_query_stats = 0;
static int skip_notify     = 0;

static int skip_reuse                   = 1;
static int skip_input_to_state          = 0;
static int skip_simple_math             = 0;
static int skip_input_to_state_extended = 0;
static int skip_brute_force             = 0;
static int skip_range_brute_force       = 0;
static int skip_range_brute_force_opt   = 0;
static int skip_gradient_descend        = 0;

static int skip_afl_deterministic          = 0;
static int skip_afl_det_single_walking_bit = 0;
static int skip_afl_det_two_walking_bit    = 0;
static int skip_afl_det_four_walking_bit   = 0;
static int skip_afl_det_byte_flip          = 0;
static int skip_afl_det_arith8             = 0;
static int skip_afl_det_int8               = 0;
static int skip_afl_det_flip_short         = 0;
static int skip_afl_det_arith16            = 0;
static int skip_afl_det_int16              = 0;
static int skip_afl_det_flip_int           = 0;
static int skip_afl_det_arith32            = 0;
static int skip_afl_det_int32              = 0;
static int skip_afl_det_flip_long          = 0;
static int skip_afl_det_arith64            = 0;
static int skip_afl_det_int64              = 0;

static int skip_freeze_neighbours = 1;
static int skip_afl_havoc         = 0;
static int use_greedy_mamin       = 0;
static int check_unnecessary_eval = 1;

static int max_ast_info_cache_size = 14000;

static int performing_aggressive_optimistic = 0;

#ifdef USE_MD5_HASH
#include "md5.h"
#else
#include "xxhash/xxh3.h"
#endif

// generate parametric data structures
#include "z3-fuzzy-datastructures-gen.h"

uint64_t Z3_API Z3_custom_eval_depth(Z3_context c, Z3_ast e, uint64_t* data,
                                     uint8_t* data_sizes, size_t data_size,
                                     uint32_t* depth);

typedef struct ast_info_t {
    unsigned      linear_arithmetic_operations;
    unsigned      nonlinear_arithmetic_operations;
    unsigned      input_extract_ops;
    unsigned      approximated_groups;
    unsigned long query_size;

    index_groups_t    index_groups;
    indexes_t         indexes;
    da__index_group_t index_groups_ud;
    da__ulong         indexes_ud;
    da__ite_its_t     inp_to_state_ite;
} ast_info_t;

typedef struct ast_data_t {
    // structure used to pass information during a single fuzzy sat execution
    unsigned n_useless_eval;

    unsigned      is_input_to_state;
    unsigned      op_input_to_state;
    index_group_t input_to_state_group;
    unsigned long input_to_state_const;

    processed_set_t processed_set;
    values_t        values;
    ast_info_t*     inputs;
} ast_data_t;

typedef ast_info_t* ast_info_ptr;
#define DICT_DATA_T ast_info_ptr
#include "dict.h"

static unsigned long* tmp_input           = NULL;
static unsigned long* tmp_opt_input       = NULL;
static unsigned char* tmp_proof           = NULL;
static unsigned char* tmp_opt_proof       = NULL;
static int            opt_found           = 0;
static unsigned       opt_num_sat         = 0;
static ast_data_t     ast_data            = {0};
static char           notify_count        = 0;
static unsigned long  g_prev_num_evaluate = 0;

static char* query_log_filename = "/tmp/fuzzy-log-info.csv";
FILE*        query_log;

static char interesting8[] = {
    -128, // 0x80
    -1,   // 0xff
    0,    // 0x0
    1,    // 0x1
    2,    // 0x2
    16,   // 0x10
    32,   // 0x20
    64,   // 0x40
    100,  // 0x64
    127,  // 0x7f
};

static short interesting16[] = {
    -32768, // 0x8000
    -129,   // 0xff7f
    128,    // 0x80
    255,    // 0xff
    256,    // 0x100
    512,    // 0x200
    1000,   // 0x3e8
    1024,   // 0x400
    4096,   // 0x1000
    32767,  // 0x7fff
    -20561, // 0xafaf
    -128,   // 0xff80
    -1,     // 0xffff
    0,      // 0x0
    1,      // 0x1
    2,      // 0x2
    16,     // 0x10
    32,     // 0x20
    64,     // 0x40
    100,    // 0x64
    127,    // 0x7f
};

static int interesting32[] = {
    -2147483648, // 0x80000000
    -100663046,  // 0xfa0000fa
    -32769,      // 0xffff7fff
    32768,       // 0x8000
    65535,       // 0xffff
    65536,       // 0x10000
    16777215,    // 0xffffff
    2147483647,  // 0x7fffffff
    -32768,      // 0xffff8000
    8421504,     // 0x808080
    -129,        // 0xffffff7f
    16711935,    // 0xff00ff
    128,         // 0x80
    255,         // 0xff
    256,         // 0x100
    512,         // 0x200
    1000,        // 0x3e8
    1024,        // 0x400
    1280,        // 0x500
    4096,        // 0x1000
    32767,       // 0x7fff
    344064,      // 0x54000
    -128,        // 0xffffff80
    -1,          // 0xffffffff
    0,           // 0x0
    1,           // 0x1
    2,           // 0x2
    16,          // 0x10
    32,          // 0x20
    64,          // 0x40
    100,         // 0x64
    127          // 0x7f
};

static long interesting64[] = {
    -9223372036854775807, // 0x8000000000000001
    9223372036854775807,  // 0x7fffffffffffffff
    -2147483648,          // 0xffffffff80000000
    -100663046,           // 0xfffffffffa0000fa
    -32769,               // 0xffffffffffff7fff
    32768,                // 0x8000
    65535,                // 0xffff
    65536,                // 0x10000
    16777215,             // 0xffffff
    2147483647,           // 0x7fffffff
    -32768,               // 0xffffffffffff8000
    -129,                 // 0xffffffffffffff7f
    72057589759737855,    // 0xffffff00ffffff
    128,                  // 0x80
    255,                  // 0xff
    256,                  // 0x100
    512,                  // 0x200
    1000,                 // 0x3e8
    1024,                 // 0x400
    4096,                 // 0x1000
    32767,                // 0x7fff
    -128,                 // 0xffffffffffffff80
    -1,                   // 0xffffffffffffffff
    0,                    // 0x0
    1,                    // 0x1
    2,                    // 0x2
    16,                   // 0x10
    32,                   // 0x20
    64,                   // 0x40
    100,                  // 0x64
    127,                  // 0x7f
};

#include "z3-fuzzy-debug-utils.h"

#define TIMEOUT_V 0xffff

static inline int timer_check_wrapper(fuzzy_ctx_t* ctx)
{
    if (ctx->timer == NULL)
        return 0;
    static int i = 0;
    if (unlikely(++i & 16)) {
        i                          = 0;
        int           res          = check_timer(ctx->timer);
        unsigned long elapsed_time = get_elapsed_time(ctx->timer);
        ctx->stats.avg_time_for_eval =
            (double)(ctx->stats.num_evaluate - g_prev_num_evaluate) == 0
                ? ctx->stats.avg_time_for_eval
                : (double)elapsed_time /
                      ((double)(ctx->stats.num_evaluate - g_prev_num_evaluate));
        return res;
    }
    return 0;
}

static inline void timer_init_wrapper(fuzzy_ctx_t* ctx, unsigned time_max_msec)
{
    init_timer(ctx->timer, time_max_msec);
}

static inline void timer_start_wrapper(fuzzy_ctx_t* ctx)
{
    if (ctx->timer == NULL)
        return;
    start_timer(ctx->timer);
}

#define RESEED_RNG 10000
static int             dev_urandom_fd = -1;
static unsigned        rand_cnt       = 1;
static inline unsigned UR(unsigned limit)
{
    if (unlikely(!rand_cnt--)) {
        unsigned seed[2];
        size_t   res = read(dev_urandom_fd, &seed, sizeof(seed));
        ASSERT_OR_ABORT(res == sizeof(seed), "read failed");
        srandom(seed[0]);
        rand_cnt = (RESEED_RNG / 2) + (seed[1] % RESEED_RNG);
    }
    return random() % limit;
}

static inline int check_sum_wrap(uint64_t v1, uint64_t v2, unsigned size)
{
    __uint128_t v1_ext   = (__uint128_t)v1;
    __uint128_t v2_ext   = (__uint128_t)v2;
    __uint128_t max_size = 2;
    max_size             = (max_size << (size - 1)) - 1;
    return v1_ext + v2_ext > max_size;
}

static inline void ast_info_init(ast_info_ptr ptr)
{
    set_init__index_group_t(&ptr->index_groups, &index_group_hash,
                            &index_group_equals);
    set_init__ulong(&ptr->indexes, &index_hash, &index_equals);
    da_init__ulong(&ptr->indexes_ud);
    da_init__index_group_t(&ptr->index_groups_ud);
    da_init__ite_its_t(&ptr->inp_to_state_ite);
    ptr->linear_arithmetic_operations    = 0;
    ptr->nonlinear_arithmetic_operations = 0;
    ptr->input_extract_ops               = 0;
    ptr->query_size                      = 0;
    ptr->approximated_groups             = 0;
}

static inline void ast_info_reset(ast_info_ptr ptr)
{
    set_remove_all__index_group_t(&ptr->index_groups, NULL);
    set_remove_all__ulong(&ptr->indexes, NULL);
    da_remove_all__ulong(&ptr->indexes_ud, NULL);
    da_remove_all__index_group_t(&ptr->index_groups_ud, NULL);
    da_remove_all__ite_its_t(&ptr->inp_to_state_ite, NULL);
    ptr->linear_arithmetic_operations    = 0;
    ptr->nonlinear_arithmetic_operations = 0;
    ptr->input_extract_ops               = 0;
    ptr->query_size                      = 0;
    ptr->approximated_groups             = 0;
}

static inline void ast_info_ptr_free(ast_info_ptr* ptr)
{
    set_free__index_group_t(&(*ptr)->index_groups, NULL);
    set_free__ulong(&(*ptr)->indexes, NULL);
    da_free__ulong(&(*ptr)->indexes_ud, NULL);
    da_free__index_group_t(&(*ptr)->index_groups_ud, NULL);
    da_free__ite_its_t(&(*ptr)->inp_to_state_ite, NULL);
    free(*ptr);
}

static inline void ast_data_init(ast_data_t* ast_data)
{
    set_init__digest_t(&ast_data->processed_set, &digest_64bit_hash,
                       &digest_equals);
    da_init__ulong(&ast_data->values);
}

static inline void ast_data_free(ast_data_t* ast_data)
{
    set_free__digest_t(&ast_data->processed_set, NULL);
    da_free__ulong(&ast_data->values, NULL);
}

// ********* gradient stuff *********
static void __reset_ast_data();
static void detect_involved_inputs_wrapper(fuzzy_ctx_t* ctx, Z3_ast v,
                                           ast_info_ptr* data);

typedef struct mapping_subel_t {
    unsigned      idx;
    unsigned      shift;
    unsigned long mask;
} mapping_subel_t;

typedef struct mapping_el_t {
    mapping_subel_t subels[8];
    unsigned        n;
} mapping_el_t;

typedef struct eval_wapper_ctx_t {
    char           check_pi_eval;
    unsigned long* input;
    mapping_el_t*  mapping;
    unsigned       mapping_size;
    unsigned       ast_sort_size;
    Z3_ast         pi;
    Z3_ast         ast;
    fuzzy_ctx_t*   fctx;
} eval_wapper_ctx_t;

static eval_wapper_ctx_t* eval_ctx;

void eval_set_ctx(eval_wapper_ctx_t* c) { eval_ctx = c; }

static void __gd_fix_tmp_input(unsigned long* x)
{
    unsigned i, j;
    for (i = 0; i < eval_ctx->mapping_size; ++i) {
        mapping_el_t* mel = &eval_ctx->mapping[i];
        for (j = 0; j < mel->n; ++j) {
            mapping_subel_t* sel   = &mel->subels[j];
            unsigned long    value = (x[i] & sel->mask) >> sel->shift;
            tmp_input[sel->idx]    = value & 0xff;
        }
    }
}

static void __gd_restore_tmp_input(testcase_t* t)
{
    unsigned i, j;
    for (i = 0; i < eval_ctx->mapping_size; ++i) {
        mapping_el_t* mel = &eval_ctx->mapping[i];
        for (j = 0; j < mel->n; ++j) {
            mapping_subel_t* sel = &mel->subels[j];
            tmp_input[sel->idx]  = t->values[sel->idx];
        }
    }
}

static unsigned long __gd_eval(unsigned long* x, int* should_exit)
{
    *should_exit = 0;
    if (timer_check_wrapper(eval_ctx->fctx)) {
        eval_ctx->fctx->stats.num_timeouts++;
        *should_exit = 1;
        return 0;
    }

    testcase_t* seed_testcase = &eval_ctx->fctx->testcases.data[0];
    __gd_fix_tmp_input(x);

    if (eval_ctx->check_pi_eval) {
        unsigned long pi_eval = eval_ctx->fctx->model_eval(
            eval_ctx->fctx->z3_ctx, eval_ctx->pi, tmp_input,
            seed_testcase->value_sizes, seed_testcase->values_len, NULL);

        if (!pi_eval)
            return 0x7fffffffffffffff;
    }

    unsigned long res = eval_ctx->fctx->model_eval(
        eval_ctx->fctx->z3_ctx, eval_ctx->ast, tmp_input,
        seed_testcase->value_sizes, seed_testcase->values_len, NULL);
    eval_ctx->fctx->stats.num_evaluate++;
    return res;
}

static int __check_overlapping_groups()
{
    int        res = 0;
    set__ulong s;
    set_init__ulong(&s, &index_hash, &index_equals);

    index_group_t* g;
    set_reset_iter__index_group_t(&ast_data.inputs->index_groups, 0);
    while (
        set_iter_next__index_group_t(&ast_data.inputs->index_groups, 0, &g)) {
        int i;
        for (i = 0; i < g->n; ++i) {
            if (set_check__ulong(&s, g->indexes[i])) {
                res = 1;
                break;
            }
            set_add__ulong(&s, g->indexes[i]);
        }
        if (res)
            break;
    }

    set_free__ulong(&s, NULL);
    return res;
}

static int __gd_init_eval(fuzzy_ctx_t* ctx, Z3_ast pi, Z3_ast expr,
                          char check_pi_eval, char must_initialize_ast,
                          eval_wapper_ctx_t* out_ctx)
{
    out_ctx->fctx          = ctx;
    out_ctx->pi            = pi;
    out_ctx->ast           = expr;
    out_ctx->check_pi_eval = check_pi_eval;
    out_ctx->mapping       = NULL;
    out_ctx->input         = NULL;

    Z3_inc_ref(ctx->z3_ctx, out_ctx->ast);
    Z3_inc_ref(ctx->z3_ctx, out_ctx->pi);

    Z3_sort bv_sort = Z3_get_sort(ctx->z3_ctx, expr);
    ASSERT_OR_ABORT(Z3_get_sort_kind(ctx->z3_ctx, bv_sort) == Z3_BV_SORT,
                    "gd works with bitvectors");
    out_ctx->ast_sort_size = Z3_get_bv_sort_size(ctx->z3_ctx, bv_sort);

    if (must_initialize_ast) {
        __reset_ast_data();
        detect_involved_inputs_wrapper(ctx, expr, &ast_data.inputs);

        if (ast_data.inputs->indexes.size == 0)
            return 0; // no index!
    }

    unsigned idx = 0;
    if (AVOID_GD_FALLBACK || !__check_overlapping_groups()) {
        out_ctx->mapping_size = ast_data.inputs->index_groups.size;
        out_ctx->mapping =
            (mapping_el_t*)malloc(sizeof(mapping_el_t) * out_ctx->mapping_size);
        out_ctx->input = (unsigned long*)calloc(sizeof(unsigned long),
                                                out_ctx->mapping_size);

        index_group_t* g;
        set_reset_iter__index_group_t(&ast_data.inputs->index_groups, 0);
        while (set_iter_next__index_group_t(&ast_data.inputs->index_groups, 0,
                                            &g)) {
            int i;
            out_ctx->mapping[idx].n = g->n;
            for (i = 0; i < g->n; ++i) {
                unsigned fixed_i                            = g->n - i - 1;
                out_ctx->mapping[idx].subels[fixed_i].idx   = g->indexes[i];
                out_ctx->mapping[idx].subels[fixed_i].shift = fixed_i * 8;
                out_ctx->mapping[idx].subels[fixed_i].mask  = 0xff
                                                             << (fixed_i * 8);

                out_ctx->input[idx] |= tmp_input[g->indexes[i]]
                                       << (fixed_i * 8);
            }
            idx++;
        }
    } else {
        // overlapping groups... Fallback to byte-by-byte gd
        out_ctx->mapping_size = ast_data.inputs->indexes.size;
        out_ctx->mapping =
            (mapping_el_t*)malloc(sizeof(mapping_el_t) * out_ctx->mapping_size);
        out_ctx->input = (unsigned long*)calloc(sizeof(unsigned long),
                                                out_ctx->mapping_size);

        ulong* i;
        set_reset_iter__ulong(&ast_data.inputs->indexes, 0);
        while (set_iter_next__ulong(&ast_data.inputs->indexes, 0, &i)) {
            out_ctx->mapping[idx].n               = 1;
            out_ctx->mapping[idx].subels[0].idx   = *i;
            out_ctx->mapping[idx].subels[0].shift = 0;
            out_ctx->mapping[idx].subels[0].mask  = 0xff;
            out_ctx->input[idx]                   = tmp_input[*i];
            idx++;
        }
    }
    return 1;
}

static void __gd_free_eval(eval_wapper_ctx_t* eval_ctx)
{
    Z3_dec_ref(eval_ctx->fctx->z3_ctx, eval_ctx->pi);
    Z3_dec_ref(eval_ctx->fctx->z3_ctx, eval_ctx->ast);

    free(eval_ctx->mapping);
    free(eval_ctx->input);
}

static inline Z3_ast __gd_create_sub(Z3_context ctx, Z3_ast lhs, Z3_ast rhs,
                                     unsigned sort_size, int is_signed)
{
    // I'm assuming that lhs and rhs has a ref_counter > 0
    ASSERT_OR_ABORT(sort_size >= 2,
                    "__gd_create_sub(): sort_size cannot be lower than 2");
    if (!is_signed) {
        Z3_ast zero =
            Z3_mk_unsigned_int64(ctx, 0, Z3_mk_bv_sort(ctx, sort_size));
        Z3_inc_ref(ctx, zero);
        Z3_ast max_signed = Z3_mk_unsigned_int64(
            ctx, (2 << (sort_size - 2)) - 1, Z3_mk_bv_sort(ctx, sort_size));
        Z3_inc_ref(ctx, max_signed);
        Z3_ast cond1 = Z3_mk_bvslt(ctx, lhs, zero);
        Z3_inc_ref(ctx, cond1);
        Z3_ast cond2 = Z3_mk_bvslt(ctx, rhs, zero);
        Z3_inc_ref(ctx, cond2);
        lhs = Z3_mk_ite(ctx, cond1, max_signed, lhs);
        Z3_inc_ref(ctx, lhs);
        rhs = Z3_mk_ite(ctx, cond2, zero, rhs);
        Z3_inc_ref(ctx, rhs);

        Z3_dec_ref(ctx, zero);
        Z3_dec_ref(ctx, max_signed);
        Z3_dec_ref(ctx, cond1);
        Z3_dec_ref(ctx, cond2);
    }
    Z3_ast res = Z3_mk_bvsub(ctx, lhs, rhs);
    Z3_inc_ref(ctx, res);
    if (!is_signed) {
        Z3_dec_ref(ctx, lhs);
        Z3_dec_ref(ctx, rhs);
    }
    return res;
}

static int __gradient_transf_init(fuzzy_ctx_t* ctx, Z3_ast expr,
                                  Z3_ast* out_exp)
{
    ASSERT_OR_ABORT(Z3_get_ast_kind(ctx->z3_ctx, expr) == Z3_APP_AST,
                    "__gradient_transf_init expects an APP argument");

    Z3_app       app       = Z3_to_app(ctx->z3_ctx, expr);
    Z3_func_decl decl      = Z3_get_app_decl(ctx->z3_ctx, app);
    Z3_decl_kind decl_kind = Z3_get_decl_kind(ctx->z3_ctx, decl);

    int    is_not = 0;
    Z3_ast arg    = expr;
    while (decl_kind == Z3_OP_NOT) {
        arg       = Z3_get_app_arg(ctx->z3_ctx, app, 0);
        app       = Z3_to_app(ctx->z3_ctx, arg);
        decl      = Z3_get_app_decl(ctx->z3_ctx, app);
        decl_kind = Z3_get_decl_kind(ctx->z3_ctx, decl);
        is_not    = !is_not;
    }
    Z3_inc_ref(ctx->z3_ctx, arg);

    if ((decl_kind == Z3_OP_OR && !is_not) ||
        (decl_kind == Z3_OP_AND && is_not)) {
        // detect whether all the conditions but one are valid
        Z3_ast valid_arg = NULL;

        unsigned i;
        unsigned nargs = Z3_get_app_num_args(ctx->z3_ctx, app);
        for (i = 0; i < nargs; ++i) {
            Z3_ast child = Z3_get_app_arg(ctx->z3_ctx, app, i);
            if (is_not)
                child = Z3_mk_not(ctx->z3_ctx, child);
            Z3_inc_ref(ctx->z3_ctx, child);

            ast_info_ptr ast_info;
            detect_involved_inputs_wrapper(ctx, child, &ast_info);

            ulong* p;
            int    has_inputs = 0;
            set_reset_iter__ulong(&ast_info->indexes, 1);
            while (set_iter_next__ulong(&ast_info->indexes, 1, &p)) {
                if (set_check__ulong(&ast_data.inputs->indexes, *p)) {
                    has_inputs = 1;
                    break;
                }
            }

            if (!has_inputs) {
                Z3_dec_ref(ctx->z3_ctx, child);
                continue;
            }
            if (valid_arg != NULL) {
                Z3_dec_ref(ctx->z3_ctx, valid_arg);
                Z3_dec_ref(ctx->z3_ctx, child);
                return 0;
            }
            valid_arg = child;
        }
        Z3_dec_ref(ctx->z3_ctx, arg);
        arg       = valid_arg;
        app       = Z3_to_app(ctx->z3_ctx, arg);
        decl      = Z3_get_app_decl(ctx->z3_ctx, app);
        decl_kind = Z3_get_decl_kind(ctx->z3_ctx, decl);

        while (decl_kind == Z3_OP_NOT) {
            Z3_ast prev_arg = arg;
            arg             = Z3_get_app_arg(ctx->z3_ctx, app, 0);
            Z3_inc_ref(ctx->z3_ctx, arg);

            app       = Z3_to_app(ctx->z3_ctx, arg);
            decl      = Z3_get_app_decl(ctx->z3_ctx, app);
            decl_kind = Z3_get_decl_kind(ctx->z3_ctx, decl);
            is_not    = !is_not;
            Z3_dec_ref(ctx->z3_ctx, prev_arg);
        }
    }
    if (decl_kind == Z3_OP_OR || decl_kind == Z3_OP_AND)
        return 0;

    int is_unsigned = 0;
    if (decl_kind == Z3_OP_UGT || decl_kind == Z3_OP_UGEQ ||
        decl_kind == Z3_OP_ULT || decl_kind == Z3_OP_ULEQ)
        is_unsigned = 1;

    ASSERT_OR_ABORT(Z3_get_app_num_args(ctx->z3_ctx, app) == 2,
                    "__gradient_transf_init requires a binary APP");

    Z3_ast args[2] = {0};
    Z3_ast arg1    = Z3_get_app_arg(ctx->z3_ctx, app, 0);
    Z3_inc_ref(ctx->z3_ctx, arg1);
    Z3_ast arg2 = Z3_get_app_arg(ctx->z3_ctx, app, 1);
    Z3_inc_ref(ctx->z3_ctx, arg2);
    Z3_dec_ref(ctx->z3_ctx, arg);

    Z3_sort arg_sort = Z3_get_sort(ctx->z3_ctx, arg1);
    if (Z3_get_sort_kind(ctx->z3_ctx, arg_sort) != Z3_BV_SORT)
        return 0;
    unsigned sort_size = Z3_get_bv_sort_size(ctx->z3_ctx, arg_sort);
    if (sort_size < 2) {
        // 1 bit bv
        Z3_dec_ref(ctx->z3_ctx, arg1);
        Z3_dec_ref(ctx->z3_ctx, arg2);
        return 0;
    }

    if (sort_size < 64) {
        if (is_unsigned) {
            Z3_ast tmp1 = arg1;
            Z3_ast tmp2 = arg2;
            arg1        = Z3_mk_zero_ext(ctx->z3_ctx, 64 - sort_size, tmp1);
            Z3_inc_ref(ctx->z3_ctx, arg1);
            arg2 = Z3_mk_zero_ext(ctx->z3_ctx, 64 - sort_size, tmp2);
            Z3_inc_ref(ctx->z3_ctx, arg2);
            Z3_dec_ref(ctx->z3_ctx, tmp1);
            Z3_dec_ref(ctx->z3_ctx, tmp2);
            sort_size = 64;
        } else {
            Z3_ast tmp1 = arg1;
            Z3_ast tmp2 = arg2;
            arg1        = Z3_mk_sign_ext(ctx->z3_ctx, 64 - sort_size, tmp1);
            Z3_inc_ref(ctx->z3_ctx, arg1);
            arg2 = Z3_mk_sign_ext(ctx->z3_ctx, 64 - sort_size, tmp2);
            Z3_inc_ref(ctx->z3_ctx, arg2);
            Z3_dec_ref(ctx->z3_ctx, tmp1);
            Z3_dec_ref(ctx->z3_ctx, tmp2);
            sort_size = 64;
        }
    }

    Z3_ast res;

PRE_SWITCH:
    switch (decl_kind) {
        case Z3_OP_SGT:
        case Z3_OP_SGEQ:
        case Z3_OP_UGT:
        case Z3_OP_UGEQ: { // arg1 > arg2 => arg2 - arg1
            if (is_not) {
                is_not    = 0;
                decl_kind = Z3_OP_SLT;
                goto PRE_SWITCH;
            }
            args[0] = arg2;
            args[1] = arg1;
            res     = __gd_create_sub(ctx->z3_ctx, args[0], args[1], sort_size,
                                  !is_unsigned);
            break;
        }
        case Z3_OP_SLT:
        case Z3_OP_SLEQ:
        case Z3_OP_ULT:
        case Z3_OP_ULEQ: { // arg1 < arg2 => arg1 - arg2
            if (is_not) {
                is_not    = 0;
                decl_kind = Z3_OP_SGT;
                goto PRE_SWITCH;
            }
            args[0] = arg1;
            args[1] = arg2;
            res     = __gd_create_sub(ctx->z3_ctx, args[0], args[1], sort_size,
                                  !is_unsigned);
            break;
        }
        case Z3_OP_EQ: { // arg1 == arg2 =>   abs(arg1 - arg2)
                         // arg1 != arg2 => - abs(arg1 - arg2)
            args[0] = arg1;
            args[1] = arg2;

            Z3_ast cond = Z3_mk_bvsgt(ctx->z3_ctx, args[0], args[1]);
            Z3_inc_ref(ctx->z3_ctx, cond);
            Z3_ast ift = Z3_mk_bvsub(ctx->z3_ctx, args[0], args[1]);
            Z3_inc_ref(ctx->z3_ctx, ift);
            Z3_ast iff = Z3_mk_bvsub(ctx->z3_ctx, args[1], args[0]);
            Z3_inc_ref(ctx->z3_ctx, iff);

            ASSERT_OR_ABORT(
                Z3_get_sort_kind(ctx->z3_ctx, Z3_get_sort(ctx->z3_ctx, cond)) ==
                    Z3_BOOL_SORT,
                "not bool sort");
            Z3_ast ite = Z3_mk_ite(ctx->z3_ctx, cond, ift, iff);
            Z3_inc_ref(ctx->z3_ctx, ite);
            Z3_dec_ref(ctx->z3_ctx, cond);
            Z3_dec_ref(ctx->z3_ctx, ift);
            Z3_dec_ref(ctx->z3_ctx, iff);

            if (is_not) {
                Z3_ast zero = Z3_mk_unsigned_int64(
                    ctx->z3_ctx, 0, Z3_mk_bv_sort(ctx->z3_ctx, sort_size));
                Z3_inc_ref(ctx->z3_ctx, zero);
                res = Z3_mk_bvsub(ctx->z3_ctx, zero, ite);
                Z3_inc_ref(ctx->z3_ctx, res);
                Z3_dec_ref(ctx->z3_ctx, ite);
                Z3_dec_ref(ctx->z3_ctx, zero);
            } else
                res = ite;
            break;
        }

        default:
            ASSERT_OR_ABORT(0, "__gradient_transf_init unknown decl kind");
    }

    Z3_dec_ref(ctx->z3_ctx, arg1);
    Z3_dec_ref(ctx->z3_ctx, arg2);
    *out_exp = res;
    return 1;
}
// **********************************

static inline void __symbol_init(fuzzy_ctx_t* ctx, unsigned long n_values)
{
    if (ctx->n_symbols >= n_values)
        return;

    unsigned int  i;
    Z3_sort       bsort         = Z3_mk_bv_sort(ctx->z3_ctx, 8);
    unsigned long old_n_symbols = ctx->n_symbols;

    if (ctx->symbols == NULL) {
        ctx->symbols   = (Z3_ast*)malloc(n_values * sizeof(Z3_ast));
        ctx->n_symbols = n_values;
    } else if (ctx->n_symbols < n_values) {
        ctx->symbols =
            (Z3_ast*)realloc(ctx->symbols, n_values * sizeof(Z3_ast));
        ctx->n_symbols = n_values;
    }

    for (i = old_n_symbols; i < ctx->n_symbols; ++i) {
        Z3_symbol s    = Z3_mk_int_symbol(ctx->z3_ctx, i);
        Z3_ast    s_bv = Z3_mk_const(ctx->z3_ctx, s, bsort);
        Z3_inc_ref(ctx->z3_ctx, s_bv);
        ctx->symbols[i] = s_bv;
    }
}

static void env_get_or_die(int* env_var, char* value)
{
    if (value == NULL)
        return;

    if (value[0] == '0')
        *env_var = 0;
    else if (value[0] == '1')
        *env_var = 1;
    else
        ASSERT_OR_ABORT(0, "environment config value must be '0' or '1'");
}

static void init_config_params()
{
    env_get_or_die(&log_query_stats, getenv("Z3FUZZ_LOG_QUERY_STATS"));
    env_get_or_die(&skip_notify, getenv("Z3FUZZ_SKIP_NOTIFY"));
    env_get_or_die(&skip_reuse, getenv("Z3FUZZ_SKIP_REUSE"));
    env_get_or_die(&skip_input_to_state, getenv("Z3FUZZ_SKIP_INPUT_TO_STATE"));
    env_get_or_die(&skip_simple_math, getenv("Z3FUZZ_SKIP_SIMPLE_MATH"));
    env_get_or_die(&skip_input_to_state_extended,
                   getenv("Z3FUZZ_SKIP_INPUT_TO_STATE_EXTENDED"));
    env_get_or_die(&skip_brute_force, getenv("Z3FUZZ_SKIP_BRUTE_FORCE"));
    env_get_or_die(&skip_range_brute_force,
                   getenv("Z3FUZZ_SKIP_RANGE_BRUTE_FORCE"));
    env_get_or_die(&skip_range_brute_force_opt,
                   getenv("Z3FUZZ_SKIP_RANGE_BRUTE_FORCE_OPT"));
    env_get_or_die(&skip_afl_deterministic,
                   getenv("Z3FUZZ_SKIP_DETERMINISTIC"));
    env_get_or_die(&skip_afl_det_single_walking_bit,
                   getenv("Z3FUZZ_SKIP_SINGLE_WALKING_BIT"));
    env_get_or_die(&skip_afl_det_two_walking_bit,
                   getenv("Z3FUZZ_SKIP_TWO_WALKING_BIT"));
    env_get_or_die(&skip_afl_det_four_walking_bit,
                   getenv("Z3FUZZ_SKIP_FOUR_WALKING_BIT"));
    env_get_or_die(&skip_afl_det_byte_flip, getenv("Z3FUZZ_SKIP_BYTE_FLIP"));
    env_get_or_die(&skip_afl_det_arith8, getenv("Z3FUZZ_SKIP_ARITH8"));
    env_get_or_die(&skip_afl_det_int8, getenv("Z3FUZZ_SKIP_INT8"));
    env_get_or_die(&skip_afl_det_flip_short, getenv("Z3FUZZ_SKIP_FLIP_SHORT"));
    env_get_or_die(&skip_afl_det_arith16, getenv("Z3FUZZ_SKIP_ARITH16"));
    env_get_or_die(&skip_afl_det_int16, getenv("Z3FUZZ_SKIP_INT16"));
    env_get_or_die(&skip_afl_det_flip_int, getenv("Z3FUZZ_SKIP_FLIP_INT"));
    env_get_or_die(&skip_afl_det_arith32, getenv("Z3FUZZ_SKIP_ARITH32"));
    env_get_or_die(&skip_afl_det_int32, getenv("Z3FUZZ_SKIP_INT32"));
    env_get_or_die(&skip_afl_det_flip_long, getenv("Z3FUZZ_SKIP_FLIP_LONG"));
    env_get_or_die(&skip_afl_det_arith64, getenv("Z3FUZZ_SKIP_ARITH64"));
    env_get_or_die(&skip_afl_det_int64, getenv("Z3FUZZ_SKIP_INT64"));
    env_get_or_die(&skip_afl_havoc, getenv("Z3FUZZ_SKIP_HAVOC"));
    env_get_or_die(&skip_gradient_descend,
                   getenv("Z3FUZZ_SKIP_GRADIENT_DESCEND"));
    env_get_or_die(&use_greedy_mamin, getenv("Z3FUZZ_USE_GREEDY_MAMIN"));
    env_get_or_die(&check_unnecessary_eval,
                   getenv("Z3FUZZ_CHECK_UNNECESSARY_EVAL"));
}

static int  g_global_ctx_initialized = 0;
static void init_global_context(size_t input_size)
{
    static size_t current_input_size = 0;
    if (g_global_ctx_initialized) {
        if (current_input_size < input_size) {
            // The handling of tmp_input is awful... I probably should be moved
            // in the context. Anyway, we are increasing the size of tmp_input
            // (and the other variants) to make room to a new context
            tmp_input = (unsigned long*)realloc(
                tmp_input, sizeof(unsigned long) * input_size);
            ASSERT_OR_ABORT(tmp_input, "init_global_context(): realloc failed");
            tmp_opt_input = (unsigned long*)realloc(
                tmp_opt_input, sizeof(unsigned long) * input_size);
            ASSERT_OR_ABORT(tmp_opt_input,
                            "init_global_context(): realloc failed");
            tmp_proof = (unsigned char*)realloc(
                tmp_proof, sizeof(unsigned char) * input_size);
            ASSERT_OR_ABORT(tmp_proof, "init_global_context(): realloc failed");
            tmp_opt_proof = (unsigned char*)realloc(
                tmp_opt_proof, sizeof(unsigned char) * input_size);
            ASSERT_OR_ABORT(tmp_opt_proof,
                            "init_global_context(): realloc failed");
        }
        return;
    }

    current_input_size = input_size;
    tmp_input = (unsigned long*)malloc(sizeof(unsigned long) * input_size);
    ASSERT_OR_ABORT(tmp_input, "init_global_context(): malloc failed");
    tmp_opt_input = (unsigned long*)malloc(sizeof(unsigned long) * input_size);
    ASSERT_OR_ABORT(tmp_opt_input, "init_global_context(): malloc failed");
    tmp_proof = (unsigned char*)malloc(sizeof(unsigned char) * input_size);
    ASSERT_OR_ABORT(tmp_proof, "init_global_context(): malloc failed");
    tmp_opt_proof = (unsigned char*)malloc(sizeof(unsigned char) * input_size);
    ASSERT_OR_ABORT(tmp_opt_proof, "init_global_context(): malloc failed");

    init_config_params();
    dev_urandom_fd = open("/dev/urandom", O_RDONLY);
    if (dev_urandom_fd < 0)
        ASSERT_OR_ABORT(0, "Unable to open /dev/urandom");

    if (log_query_stats) {
        query_log = fopen(query_log_filename, "w");
        fprintf(query_log, "ctx id;query size;index size;index group size;is "
                           "input to state;linear "
                           "arith ops;non linear arith ops");
    }

    ast_data_init(&ast_data);
    gd_init();

    g_global_ctx_initialized = 1;
}

void z3fuzz_init(fuzzy_ctx_t* fctx, Z3_context ctx, char* seed_filename,
                 char* testcase_path,
                 uint64_t (*model_eval)(Z3_context, Z3_ast, uint64_t*, uint8_t*,
                                        size_t, uint32_t*),
                 unsigned timeout)
{
    memset((void*)&fctx->stats, 0, sizeof(fuzzy_stats_t));

    if (timeout != 0) {
        fctx->timer = (void*)malloc(sizeof(simple_timer_t));
        timer_init_wrapper(fctx, timeout);
    } else
        fctx->timer = NULL;

    Z3_set_ast_print_mode(ctx, Z3_PRINT_SMTLIB2_COMPLIANT);

    fctx->model_eval = model_eval != NULL ? model_eval : Z3_custom_eval_depth;
    fctx->z3_ctx     = ctx;
    fctx->testcase_path = testcase_path;
    init_testcase_list(&fctx->testcases);
    load_testcase(&fctx->testcases, seed_filename, ctx);
    if (testcase_path != NULL)
        load_testcase_folder(&fctx->testcases, testcase_path, ctx);
    ASSERT_OR_ABORT(fctx->testcases.size > 0, "no testcase");

    fctx->assignments      = (Z3_ast*)calloc(10, sizeof(Z3_ast));
    fctx->size_assignments = 10;

    fctx->n_symbols = 0;
    fctx->symbols   = NULL;
    __symbol_init(fctx, fctx->testcases.data[0].values_len);

    testcase_t* current_testcase = &fctx->testcases.data[0];
    init_global_context(current_testcase->values_len);

    fctx->univocally_defined_inputs = (void*)malloc(sizeof(set__ulong));
    set__ulong* univocally_defined_inputs =
        (set__ulong*)fctx->univocally_defined_inputs;
    set_init__ulong(univocally_defined_inputs, &index_hash, &index_equals);

    fctx->group_intervals = (void*)malloc(sizeof(set__interval_group_ptr));
    set__interval_group_ptr* group_intervals =
        (set__interval_group_ptr*)fctx->group_intervals;
    set_init__interval_group_ptr(group_intervals, &interval_group_ptr_hash,
                                 &interval_group_ptr_equals);

    fctx->index_to_group_intervals =
        malloc(sizeof(dict__da__interval_group_ptr));
    dict__da__interval_group_ptr* index_to_group_intervals =
        (dict__da__interval_group_ptr*)fctx->index_to_group_intervals;
    dict_init__da__interval_group_ptr(index_to_group_intervals,
                                      &index_to_group_intervals_el_free);

    fctx->ast_info_cache = malloc(sizeof(dict__ast_info_ptr));
    dict__ast_info_ptr* ast_info_cache =
        (dict__ast_info_ptr*)fctx->ast_info_cache;
    dict_init__ast_info_ptr(ast_info_cache, ast_info_ptr_free);

    fctx->conflicting_asts =
        (dict__conflicting_ptr*)malloc(sizeof(dict__conflicting_ptr));
    dict__conflicting_ptr* conflicting_asts =
        (dict__conflicting_ptr*)fctx->conflicting_asts;
    dict_init__conflicting_ptr(conflicting_asts, conflicting_ptr_free);

    fctx->processed_constraints = (set__ulong*)malloc(sizeof(set__ulong));
    set__ulong* processed_constraints =
        (set__ulong*)fctx->processed_constraints;
    set_init__ulong(processed_constraints, index_hash, index_equals);
}

fuzzy_ctx_t* z3fuzz_create(Z3_context ctx, char* seed_filename,
                           unsigned timeout)
{
    fuzzy_ctx_t* res = (fuzzy_ctx_t*)malloc(sizeof(fuzzy_ctx_t));
    ASSERT_OR_ABORT(res, "z3fuzz_create(): failed malloc");

    z3fuzz_init(res, ctx, seed_filename, NULL, NULL, timeout);
    return res;
}

__attribute__((destructor)) static void release_global_context()
{
    if (!g_global_ctx_initialized)
        return;
    g_global_ctx_initialized = 0;

    close(dev_urandom_fd);

    if (log_query_stats)
        fclose(query_log);

    free(tmp_input);
    tmp_input = NULL;
    free(tmp_opt_input);
    tmp_opt_input = NULL;
    free(tmp_proof);
    tmp_proof = NULL;
    free(tmp_opt_proof);
    tmp_opt_proof = NULL;

    ast_data_free(&ast_data);
    gd_free();
}

void z3fuzz_free(fuzzy_ctx_t* ctx)
{
    free(ctx->timer);
    free_testcase_list(ctx->z3_ctx, &ctx->testcases);

    unsigned int i;
    for (i = 0; i < ctx->n_symbols; ++i)
        Z3_dec_ref(ctx->z3_ctx, ctx->symbols[i]);
    free(ctx->symbols);
    ctx->symbols   = NULL;
    ctx->n_symbols = 0;
    for (i = 0; i < ctx->size_assignments; ++i)
        if (ctx->assignments[i] != NULL)
            Z3_dec_ref(ctx->z3_ctx, ctx->assignments[i]);
    free(ctx->assignments);
    ctx->assignments      = NULL;
    ctx->size_assignments = 0;

    dict__ast_info_ptr* ast_info_cache =
        (dict__ast_info_ptr*)ctx->ast_info_cache;
    dict_free__ast_info_ptr(ast_info_cache);
    free(ctx->ast_info_cache);

    dict__conflicting_ptr* conflicting_asts =
        (dict__conflicting_ptr*)ctx->conflicting_asts;
    dict_free__conflicting_ptr(conflicting_asts);
    free(conflicting_asts);

    set__ulong* processed_constraints = (set__ulong*)ctx->processed_constraints;
    set_free__ulong(processed_constraints, NULL);
    free(ctx->processed_constraints);

    set__ulong* univocally_defined_inputs =
        (set__ulong*)ctx->univocally_defined_inputs;
    set_free__ulong(univocally_defined_inputs, NULL);
    free(ctx->univocally_defined_inputs);

    set__interval_group_ptr* group_intervals =
        (set__interval_group_ptr*)ctx->group_intervals;
    set_free__interval_group_ptr(group_intervals, interval_group_set_el_free);
    free(ctx->group_intervals);

    dict__da__interval_group_ptr* index_to_group_intervals =
        (dict__da__interval_group_ptr*)ctx->index_to_group_intervals;
    dict_free__da__interval_group_ptr(index_to_group_intervals);
    free(ctx->index_to_group_intervals);
}

void z3fuzz_print_expr(fuzzy_ctx_t* ctx, Z3_ast e)
{
    Z3FUZZ_LOG("expr:\n%s\n[end expr]\n", Z3_ast_to_string(ctx->z3_ctx, e));
}

static inline void __vals_char_to_long(unsigned char* in_vals,
                                       unsigned long* out_vals,
                                       unsigned long  n_vals)
{
    unsigned long i;
    for (i = 0; i < n_vals; ++i) {
        out_vals[i] = (unsigned long)(in_vals[i]);
    }
}

static inline void __vals_long_to_char(unsigned long* in_vals,
                                       unsigned char* out_vals,
                                       unsigned long  n_vals)
{
    unsigned long i;
    for (i = 0; i < n_vals; ++i)
        out_vals[i] = (unsigned char)in_vals[i];
}

static int __check_or_add_digest(set__digest_t* set, unsigned char* values,
                                 unsigned n)
{
    digest_t d;
#ifdef USE_MD5_HASH
    md5((unsigned char*)values, n, d.digest);
#else
    XXH128_hash_t xxd = XXH3_128bits((unsigned char*)values, n);
    d.digest[0]       = xxd.high64 & 0xff;
    d.digest[1]       = (xxd.high64 >> 8) & 0xff;
    d.digest[2]       = (xxd.high64 >> 16) & 0xff;
    d.digest[3]       = (xxd.high64 >> 24) & 0xff;
    d.digest[4]       = (xxd.high64 >> 32) & 0xff;
    d.digest[5]       = (xxd.high64 >> 40) & 0xff;
    d.digest[6]       = (xxd.high64 >> 48) & 0xff;
    d.digest[7]       = (xxd.high64 >> 56) & 0xff;
    d.digest[8]       = xxd.low64 & 0xff;
    d.digest[9]       = (xxd.low64 >> 8) & 0xff;
    d.digest[10]      = (xxd.low64 >> 16) & 0xff;
    d.digest[11]      = (xxd.low64 >> 24) & 0xff;
    d.digest[12]      = (xxd.low64 >> 32) & 0xff;
    d.digest[13]      = (xxd.low64 >> 40) & 0xff;
    d.digest[14]      = (xxd.low64 >> 48) & 0xff;
    d.digest[15]      = (xxd.low64 >> 56) & 0xff;
#endif

    if (set_check__digest_t(set, d))
        return 1;
    set_add__digest_t(set, d);
    return 0;
}

static __always_inline unsigned long index_group_to_value(index_group_t* ig,
                                                          unsigned long* values)
{
    unsigned long res = 0;
    int           i;
    for (i = 0; i < ig->n; ++i)
        res |= (values[ig->indexes[ig->n - i - 1]] << (8UL * i));
    return res;
}

static int check_is_valid = 1;

static __always_inline int is_valid_eval_index(fuzzy_ctx_t*   ctx,
                                               unsigned long  index,
                                               unsigned long* values,
                                               unsigned char* value_sizes,
                                               unsigned long  n_values)
{
#ifdef SKIP_IS_VALID_EVAL
    return 1;
#else
    if (unlikely(!check_is_valid))
        return 1;
    // check validity of index eval
    dict__da__interval_group_ptr* index_to_group_intervals =
        (dict__da__interval_group_ptr*)ctx->index_to_group_intervals;

    da__interval_group_ptr* list;
    list =
        dict_get_ref__da__interval_group_ptr(index_to_group_intervals, index);
    if (list == NULL)
        return 1;

    unsigned           i;
    interval_group_ptr el;
    for (i = 0; i < list->size; ++i) {
        el = list->data[i];

        unsigned long group_value = index_group_to_value(&el->group, values);
        if (!wi_contains_element(&el->interval, group_value))
            return 0;
    }
    return 1;
#endif
}

static __always_inline int
is_valid_eval_group(fuzzy_ctx_t* ctx, index_group_t* ig, unsigned long* values,
                    unsigned char* value_sizes, unsigned long n_values)
{
#ifdef SKIP_IS_VALID_EVAL
    return 1;
#else
    if (unlikely(!check_is_valid))
        return 1;
    // for every element in ig, check interval validity
    unsigned i;
    for (i = 0; i < ig->n; ++i)
        if (!is_valid_eval_index(ctx, ig->indexes[i], values, value_sizes,
                                 n_values))
            return 0;
    return 1;
#endif
}

static inline int __evaluate_branch_query(fuzzy_ctx_t* ctx, Z3_ast query,
                                          Z3_ast         branch_condition,
                                          unsigned long* values,
                                          unsigned char* value_sizes,
                                          unsigned long  n_values)
{
    if (timer_check_wrapper(ctx)) {
        ctx->stats.num_timeouts++;
        return TIMEOUT_V;
    }

    ctx->stats.num_evaluate++;

    if (check_unnecessary_eval)
        if (__check_or_add_digest(&ast_data.processed_set,
                                  (unsigned char*)values,
                                  ctx->n_symbols * sizeof(unsigned long))) {
            return 0;
        }

    int      res;
    uint32_t depth;
    res = (int)ctx->model_eval(ctx->z3_ctx, branch_condition, values,
                               value_sizes, n_values, NULL);
    if (res) {
#if 0
        unsigned num_sat;
        res = evaluate_pi(ctx, query, values, value_sizes, n_values, &num_sat);
        if (!opt_found || num_sat > opt_num_sat) {
            testcase_t* t = &ctx->testcases.data[0];
            opt_found     = 1;
            opt_num_sat   = num_sat;
            memcpy(tmp_opt_input, values,
                   t->values_len * sizeof(unsigned long));
            __vals_long_to_char(values, tmp_opt_proof, t->testcase_len);
        }
#else
        res = (int)ctx->model_eval(ctx->z3_ctx, query, values, value_sizes,
                                   n_values, &depth);
        if (!opt_found || depth > opt_num_sat) {
            testcase_t* t = &ctx->testcases.data[0];
            opt_found     = 1;
            opt_num_sat   = depth;
            memcpy(tmp_opt_input, values,
                   t->values_len * sizeof(unsigned long));
            __vals_long_to_char(values, tmp_opt_proof, t->testcase_len);
        }
#endif
    }
    res = res != 0 ? 1 : 0;
    return res;
}

// *************************************************
// **** HEURISTICS - POPULATE ast_data_t struct ****
// *************************************************

static inline int is_bor_constraint(fuzzy_ctx_t* ctx, Z3_ast branch_condition)
{
    Z3_ast_kind node_kind = Z3_get_ast_kind(ctx->z3_ctx, branch_condition);
    if (node_kind != Z3_APP_AST)
        return 0;

    Z3_app       node_app       = Z3_to_app(ctx->z3_ctx, branch_condition);
    Z3_func_decl node_decl      = Z3_get_app_decl(ctx->z3_ctx, node_app);
    Z3_decl_kind node_decl_kind = Z3_get_decl_kind(ctx->z3_ctx, node_decl);

    if (node_decl_kind == Z3_OP_BOR)
        return 1;
    return 0;
}

static inline void flatten_bor_args(fuzzy_ctx_t* ctx, Z3_ast node,
                                    da__Z3_ast* args)
{
    Z3_app   app        = Z3_to_app(ctx->z3_ctx, node);
    unsigned num_fields = Z3_get_app_num_args(ctx->z3_ctx, app);

    unsigned i;
    for (i = 0; i < num_fields; ++i) {
        Z3_ast child = Z3_get_app_arg(ctx->z3_ctx, app, i);
        if (is_bor_constraint(ctx, child))
            flatten_bor_args(ctx, child, args);
        else {
            Z3_inc_ref(ctx->z3_ctx, child);
            da_add_item__Z3_ast(args, child);
        }
    }
}

static int __detect_single_byte(fuzzy_ctx_t* ctx, Z3_ast node, int* idx)
{
    if (Z3_get_ast_kind(ctx->z3_ctx, node) != Z3_APP_AST)
        return 0;

    int          res        = 0;
    Z3_app       app        = Z3_to_app(ctx->z3_ctx, node);
    unsigned     num_fields = Z3_get_app_num_args(ctx->z3_ctx, app);
    Z3_func_decl decl       = Z3_get_app_decl(ctx->z3_ctx, app);
    Z3_decl_kind decl_kind  = Z3_get_decl_kind(ctx->z3_ctx, decl);
    if (num_fields > 1) {
        return 0;
    }

    switch (decl_kind) {
        case Z3_OP_EXTRACT:
        case Z3_OP_ZERO_EXT:
        case Z3_OP_SIGN_EXT: {
            Z3_ast child = Z3_get_app_arg(ctx->z3_ctx, app, 0);
            res          = __detect_single_byte(ctx, child, idx);
            break;
        }
        case Z3_OP_UNINTERPRETED: {
            Z3_symbol s            = Z3_get_decl_name(ctx->z3_ctx, decl);
            int       symbol_index = Z3_get_symbol_int(ctx->z3_ctx, s);
            if (symbol_index >= ctx->testcases.data[0].testcase_len)
                // it is an assignment
                res = 0;
            else {
                *idx = symbol_index;
                res  = 1;
            }
            break;
        }
        default:
            break;
    }
    return res;
}

static int __detect_concat_shift(fuzzy_ctx_t* ctx, Z3_ast node, int* s_idx,
                                 int* pos)
{
    if (Z3_get_ast_kind(ctx->z3_ctx, node) != Z3_APP_AST)
        return 0;

    Z3_app       app        = Z3_to_app(ctx->z3_ctx, node);
    unsigned     num_fields = Z3_get_app_num_args(ctx->z3_ctx, app);
    Z3_func_decl decl       = Z3_get_app_decl(ctx->z3_ctx, app);
    Z3_decl_kind decl_kind  = Z3_get_decl_kind(ctx->z3_ctx, decl);

    if (decl_kind != Z3_OP_CONCAT) {
        int idx;
        if (!__detect_single_byte(ctx, node, &idx))
            return 0;

        *pos   = 0;
        *s_idx = idx;
        return 1;
    }

    if (num_fields != 2)
        return 0;

    *s_idx = -1;
    *pos   = -1;

    Z3_ast child_sx = Z3_get_app_arg(ctx->z3_ctx, app, 0);
    Z3_inc_ref(ctx->z3_ctx, child_sx);
    int idx;
    if (__detect_single_byte(ctx, child_sx, &idx)) {
        *s_idx = idx;
    } else {
        Z3_dec_ref(ctx->z3_ctx, child_sx);
        return 0;
    }
    Z3_dec_ref(ctx->z3_ctx, child_sx);

    Z3_ast child_dx = Z3_get_app_arg(ctx->z3_ctx, app, 1);
    Z3_inc_ref(ctx->z3_ctx, child_dx);
    if (Z3_get_ast_kind(ctx->z3_ctx, child_dx) == Z3_NUMERAL_AST) {
        *pos = Z3_get_bv_sort_size(ctx->z3_ctx,
                                   Z3_get_sort(ctx->z3_ctx, child_dx));
        if (*pos % 8 != 0) {
            Z3_dec_ref(ctx->z3_ctx, child_dx);
            return 0;
        }
        *pos /= 8;
    } else {
        Z3_dec_ref(ctx->z3_ctx, child_dx);
        return 0;
    }
    Z3_dec_ref(ctx->z3_ctx, child_dx);

    return 1;
}

static int __detect_input_group(fuzzy_ctx_t* ctx, Z3_ast node,
                                index_group_t* ig, char* approx)
{
    int res;
    switch (Z3_get_ast_kind(ctx->z3_ctx, node)) {
        case Z3_APP_AST: {
            Z3_app       app        = Z3_to_app(ctx->z3_ctx, node);
            unsigned     num_fields = Z3_get_app_num_args(ctx->z3_ctx, app);
            Z3_func_decl decl       = Z3_get_app_decl(ctx->z3_ctx, app);
            Z3_decl_kind decl_kind  = Z3_get_decl_kind(ctx->z3_ctx, decl);
            unsigned     i;
            switch (decl_kind) {
                case Z3_OP_EXTRACT: {
#ifdef DEBUG_DETECT_GROUP
                    Z3FUZZ_LOG("DETECT_GROUP [Z3_OP_EXTRACT]\n");
#endif

                    int prev_n = ig->n;

                    unsigned long hig =
                        Z3_get_decl_int_parameter(ctx->z3_ctx, decl, 0);
                    unsigned long low =
                        Z3_get_decl_int_parameter(ctx->z3_ctx, decl, 1);
#ifdef DEBUG_DETECT_GROUP
                    Z3FUZZ_LOG("hig: %lu, low: %lu, prev_n: %d\n", hig, low,
                               prev_n);
#endif
                    if (hig + 1 % 8 != 0 || low % 8 != 0)
                        *approx = 1;

                    // recursive call
                    Z3_ast child = Z3_get_app_arg(ctx->z3_ctx, app, 0);
                    Z3_inc_ref(ctx->z3_ctx, child);
                    res = __detect_input_group(ctx, child, ig, approx);
                    Z3_dec_ref(ctx->z3_ctx, child);
                    if (res == 0)
                        break;

                    int next_n = ig->n;
#ifdef DEBUG_DETECT_GROUP
                    Z3FUZZ_LOG("next_n: %d\n", next_n);
                    for (i = 0; i < ig->n; ++i)
                        Z3FUZZ_LOG(" @ ig->indexes[%u] = 0x%lx\n", i,
                                   ig->indexes[i]);
#endif

                    unsigned bv_width = next_n - prev_n;
                    if (bv_width < hig / 8 + 1) {
                        res = 0;
                        break;
                    }

                    // spill in tmp (little endian)
                    unsigned long tmp[bv_width];
                    for (i = 0; i < bv_width; ++i)
                        tmp[i] = ig->indexes[next_n - i - 1];

                    // move tmp to: ig->indexes + prev_n
                    for (i = low / 8; i <= hig / 8; ++i) {
                        ASSERT_OR_ABORT(i < bv_width, "extract overflow");
                        ig->indexes[prev_n++] = tmp[i];
                    }
                    ig->n = prev_n;

#ifdef DEBUG_DETECT_GROUP
                    for (i = 0; i < ig->n; ++i)
                        Z3FUZZ_LOG(" > ig->indexes[%u] = 0x%lx\n", i,
                                   ig->indexes[i]);
#endif
                    break;
                }
                case Z3_OP_BAND: {
#ifdef DEBUG_DETECT_GROUP
                    Z3FUZZ_LOG("DETECT_GROUP [Z3_OP_BAND]\n");
#endif
                    // check if one of the two is a constant
                    // recursive call as before
                    if (num_fields != 2) {
                        res = 0;
                        break;
                    }

                    Z3_ast child_1 = Z3_get_app_arg(ctx->z3_ctx, app, 0);
                    Z3_inc_ref(ctx->z3_ctx, child_1);

                    Z3_ast child_2 = Z3_get_app_arg(ctx->z3_ctx, app, 1);
                    Z3_inc_ref(ctx->z3_ctx, child_2);

                    Z3_ast        subexpr = NULL;
                    unsigned long mask;
                    if (Z3_get_ast_kind(ctx->z3_ctx, child_1) ==
                        Z3_NUMERAL_AST) {
                        Z3_bool successGet = Z3_get_numeral_uint64(
                            ctx->z3_ctx, child_1, (uint64_t*)&mask);
                        if (successGet == Z3_FALSE) {
                            res = 0;
                            goto BVAND_EXIT;
                        }
                        subexpr = child_2;
                    } else if (Z3_get_ast_kind(ctx->z3_ctx, child_2) ==
                               Z3_NUMERAL_AST) {
                        Z3_bool successGet = Z3_get_numeral_uint64(
                            ctx->z3_ctx, child_2, (uint64_t*)&mask);
                        if (successGet == Z3_FALSE) {
                            res = 0; // constant is too big
                            goto BVAND_EXIT;
                        }
                        subexpr = child_1;
                    } else {
                        res = 0;
                        goto BVAND_EXIT;
                    }
                    if (mask == 0) {
                        res = 0; // and with 0 -> no group, it is always 0
                        goto BVAND_EXIT;
                    }

                    int prev_n = ig->n;
#ifdef DEBUG_DETECT_GROUP
                    Z3FUZZ_LOG("prev_n: %d\n", prev_n);
#endif

                    // recursive call
                    res = __detect_input_group(ctx, subexpr, ig, approx);
                    if (res == 0)
                        goto BVAND_EXIT;

                    // find rightmost and leftmost set-bit of mask
                    unsigned long low = rightmost_set_bit(mask);
                    unsigned long hig = leftmost_set_bit(mask);
                    if (hig + 1 % 8 != 0 || low % 8 != 0)
                        *approx = 1;

                    int next_n = ig->n;
#ifdef DEBUG_DETECT_GROUP
                    Z3FUZZ_LOG("low: %lu, hig: %lu\n", low, hig);
                    Z3FUZZ_LOG("next_n: %d\n", next_n);
                    for (i = 0; i < ig->n; ++i)
                        Z3FUZZ_LOG(" @ ig->indexes[%u] = 0x%lx\n", i,
                                   ig->indexes[i]);
#endif

                    unsigned bv_width = next_n - prev_n;
                    if (bv_width < hig / 8 + 1) {
                        res = 0;
                        goto BVAND_EXIT;
                    }

                    // spill in tmp (little endian)
                    unsigned long* tmp = (unsigned long*)malloc(
                        sizeof(unsigned long) * bv_width);
                    for (i = 0; i < bv_width; ++i)
                        tmp[i] = ig->indexes[next_n - i - 1];

                    // move tmp to: ig->indexes + prev_n
                    for (i = low / 8; i <= hig / 8; ++i) {
                        ASSERT_OR_ABORT(i < bv_width, "extract overflow");
                        ig->indexes[prev_n++] = tmp[i];
                    }
                    ig->n = prev_n;

#ifdef DEBUG_DETECT_GROUP
                    for (i = 0; i < ig->n; ++i)
                        Z3FUZZ_LOG(" > ig->indexes[%u] = 0x%lx\n", i,
                                   ig->indexes[i]);
#endif
                    free(tmp);

                BVAND_EXIT:
                    Z3_dec_ref(ctx->z3_ctx, child_1);
                    Z3_dec_ref(ctx->z3_ctx, child_2);
                    break;
                }
                case Z3_OP_BADD:
                case Z3_OP_BOR: {
                    // detect if is an OR/ADD of BSHL
#ifdef DEBUG_DETECT_GROUP
                    Z3FUZZ_LOG("DETECT_GROUP [Z3_OP_BADD/Z3_OP_BOR]\n");
#endif

                    if (decl_kind == Z3_OP_BOR) {

                        da__Z3_ast args;
                        da_init__Z3_ast(&args);
                        flatten_bor_args(ctx, node, &args);

                        int found_concat_shifts = 1;
                        int n_tmp_inps          = 0;
                        int tmp_inps[8]         = {-1, -1, -1, -1, -1, -1, -1};
                        for (i = 0; i < args.size; ++i) {
                            if (i >= 8) {
                                found_concat_shifts = 0;
                                break;
                            }
                            Z3_ast child = args.data[i];

                            int symb_idx, pos;
                            if (!__detect_concat_shift(ctx, child, &symb_idx,
                                                       &pos)) {
                                found_concat_shifts = 0;
                                break;
                            }

                            if (tmp_inps[pos] != -1) {
                                found_concat_shifts = 0;
                                break;
                            }

                            tmp_inps[pos] = symb_idx;
                            n_tmp_inps++;
                        }
                        for (i = 0; i < args.size; ++i)
                            Z3_dec_ref(ctx->z3_ctx, args.data[i]);
                        da_free__Z3_ast(&args, NULL);

                        if (found_concat_shifts) {
                            for (i = 0; i < 8; ++i) {
                                if (ig->n >= MAX_GROUP_SIZE) {
                                    res = 0;
                                    break;
                                }
                                if (tmp_inps[i] == -1)
                                    break;
                                ig->indexes[ig->n++] = tmp_inps[i];
                            }
                            if (i != n_tmp_inps) {
                                res = 0;
                                break;
                            }
                            // invert group!
                            for (i = 0; i < n_tmp_inps; ++i)
                                ig->indexes[i] = tmp_inps[n_tmp_inps - i - 1];
#ifdef DEBUG_DETECT_GROUP
                            Z3FUZZ_LOG("Detected with detect concat shift the "
                                       "group:\n");
                            print_index_group(ig);
#endif
                            res = 1;
                            break;
                        }
                    }

                    res                            = 0;
                    unsigned long shift_mask       = 0;
                    int           op_without_shift = 0;
                    for (i = 0; i < num_fields; ++i) {
                        Z3_ast child = Z3_get_app_arg(ctx->z3_ctx, app, i);
                        if (Z3_get_ast_kind(ctx->z3_ctx, child) == Z3_APP_AST) {
                            Z3_app child_app = Z3_to_app(ctx->z3_ctx, child);
                            Z3_func_decl child_decl =
                                Z3_get_app_decl(ctx->z3_ctx, child_app);
                            if (Z3_get_decl_kind(ctx->z3_ctx, child_decl) ==
                                Z3_OP_BSHL) {
#ifdef DEBUG_DETECT_GROUP
                                Z3FUZZ_LOG("> shift\n");
#endif
                                unsigned long shift_val = 0;

                                Z3_ast child_1 =
                                    Z3_get_app_arg(ctx->z3_ctx, child_app, 0);
                                Z3_inc_ref(ctx->z3_ctx, child_1);

                                Z3_ast child_2 =
                                    Z3_get_app_arg(ctx->z3_ctx, child_app, 1);
                                Z3_inc_ref(ctx->z3_ctx, child_2);

                                Z3_ast subexpr = NULL;
                                if (Z3_get_ast_kind(ctx->z3_ctx, child_2) ==
                                    Z3_NUMERAL_AST) {
                                    Z3_bool successGet = Z3_get_numeral_uint64(
                                        ctx->z3_ctx, child_2,
                                        (uint64_t*)&shift_val);
                                    if (!successGet)
                                        res = 0; // constant is too big
                                    else {
                                        subexpr = child_1;
                                        res     = 1;
                                    }
                                } else
                                    res = 0;

                                if (!res) {
                                    Z3_dec_ref(ctx->z3_ctx, child_1);
                                    Z3_dec_ref(ctx->z3_ctx, child_2);
                                    break;
                                }

#ifdef DEBUG_DETECT_GROUP
                                Z3FUZZ_LOG("> shift val: 0x%016lx\n",
                                           shift_val);
#endif

                                unsigned char prev_n = ig->n;
                                res = __detect_input_group(ctx, subexpr, ig,
                                                           approx);
                                if (ig->n == prev_n) {
                                    res = 0;
                                    Z3_dec_ref(ctx->z3_ctx, child_1);
                                    Z3_dec_ref(ctx->z3_ctx, child_2);
                                    break;
                                }
                                // TODO: fix this. `Concat(k!0, 0x00) << 8` <-
                                // mispredicted
                                unsigned long curr_mask =
                                    (2UL << ((ig->n - prev_n) * 8 - 1)) - 1;
                                if (((curr_mask << shift_val) & shift_mask) !=
                                    0) {
                                    res = 0;
                                    Z3_dec_ref(ctx->z3_ctx, child_1);
                                    Z3_dec_ref(ctx->z3_ctx, child_2);
                                    break;
                                }
                                shift_mask |= curr_mask << shift_val;

                                Z3_dec_ref(ctx->z3_ctx, child_1);
                                Z3_dec_ref(ctx->z3_ctx, child_2);

                                if (!res)
                                    break;
#ifdef DEBUG_DETECT_GROUP
                                Z3FUZZ_LOG("> shift OK\n");
#endif
                                continue;
                            }
                        }
                        if (!op_without_shift)
                            op_without_shift = 1;
                        else {
                            res = 0;
                            break;
                        }
#ifdef DEBUG_DETECT_GROUP
                        Z3FUZZ_LOG("> op != shift\n");
#endif
                        res = __detect_input_group(ctx, child, ig, approx);
                        if (!res)
                            break;
#ifdef DEBUG_DETECT_GROUP
                        Z3FUZZ_LOG("> op != shift OK\n");
#endif
                    }

                    break;
                }
                case Z3_OP_CONCAT: {
                    // recursive call
                    int found_inp = 0;
                    res           = 0;
                    for (i = 0; i < num_fields; ++i) {
                        Z3_ast child = Z3_get_app_arg(ctx->z3_ctx, app, i);
                        Z3_inc_ref(ctx->z3_ctx, child);
                        Z3_ast_kind child_kind =
                            Z3_get_ast_kind(ctx->z3_ctx, child);
                        if (child_kind != Z3_NUMERAL_AST)
                            found_inp = 1;
                        else if (child_kind == Z3_NUMERAL_AST && found_inp)
                            // the group is shifted...
                            *approx = 1;

                        if (child_kind == Z3_NUMERAL_AST) {
                            Z3_dec_ref(ctx->z3_ctx, child);
                            continue;
                        }

                        res = __detect_input_group(ctx, child, ig, approx);
                        Z3_dec_ref(ctx->z3_ctx, child);
                        if (res == 0)
                            break;
                    }
                    break;
                }
                case Z3_OP_UNINTERPRETED: {
                    Z3_symbol s = Z3_get_decl_name(ctx->z3_ctx, decl);
                    if (Z3_get_symbol_kind(ctx->z3_ctx, s) != Z3_INT_SYMBOL) {
                        fprintf(stderr, "bad symbol %s\n", Z3_get_symbol_string(ctx->z3_ctx, s));
                        exit(1);
                    }
                    unsigned  symbol_index =
                        (unsigned)Z3_get_symbol_int(ctx->z3_ctx, s);

                    if (symbol_index >= ctx->testcases.data[0].testcase_len)
                        // it is an assignment
                        return 0;

                    if (ig->n >= MAX_GROUP_SIZE) {
                        res = 0;
                        break;
                    }

                    int      already_in = 0;
                    unsigned i;
                    for (i = 0; i < ig->n; ++i) {
                        if (ig->indexes[i] == symbol_index)
                            already_in = 1;
                    }

                    if (!already_in)
                        ig->indexes[ig->n++] = symbol_index;

                    res = 1;
                    break;
                }
                default: {
                    res = 0;
                    break;
                }
            }
            break;
        }
        case Z3_NUMERAL_AST: {
            // uint64_t v;
            // Z3_bool  successGet =
            //     Z3_get_numeral_uint64(ctx->z3_ctx, node, (uint64_t*)&v);
            // if (!successGet || v != 0)
            //     *approx = 1;

            res = 1;
            break;
        }
        default: {
            res = 0;
            break;
        }
    }
    return res;
}

static void __detect_input_to_state_query(fuzzy_ctx_t* ctx, Z3_ast node,
                                          ast_data_t* data, int constant_strict)
{
    Z3_ast_kind node_kind = Z3_get_ast_kind(ctx->z3_ctx, node);
    unsigned    is_app    = node_kind == Z3_APP_AST;

    Z3_app       node_app = is_app ? Z3_to_app(ctx->z3_ctx, node) : (Z3_app)0;
    Z3_func_decl node_decl =
        is_app ? Z3_get_app_decl(ctx->z3_ctx, node_app) : (Z3_func_decl)0;
    Z3_decl_kind node_decl_kind =
        is_app ? Z3_get_decl_kind(ctx->z3_ctx, node_decl) : (Z3_decl_kind)0;

    // condition 1 - root is a comparison (should always be the case)
    // also, save the comparison type
    if (!is_app) {
        data->is_input_to_state = 0;
        return;
    }

    unsigned is_neg;
    unsigned op_type;

    if (node_decl_kind == Z3_OP_EQ || node_decl_kind == Z3_OP_UGEQ ||
        node_decl_kind == Z3_OP_SGEQ || node_decl_kind == Z3_OP_ULEQ ||
        node_decl_kind == Z3_OP_SLEQ || node_decl_kind == Z3_OP_UGT ||
        node_decl_kind == Z3_OP_SGT || node_decl_kind == Z3_OP_ULT ||
        node_decl_kind == Z3_OP_SLT) {

        op_type = node_decl_kind;
        is_neg  = 0;
    } else if (node_decl_kind == Z3_OP_NOT) {
        Z3_ast      child        = Z3_get_app_arg(ctx->z3_ctx, node_app, 0);
        Z3_ast_kind child_kind   = Z3_get_ast_kind(ctx->z3_ctx, child);
        unsigned    child_is_app = child_kind == Z3_APP_AST;
        if (!child_is_app) {
            data->is_input_to_state = 0;
            return;
        }

        Z3_app       child_app  = Z3_to_app(ctx->z3_ctx, child);
        Z3_func_decl child_decl = Z3_get_app_decl(ctx->z3_ctx, child_app);
        Z3_decl_kind child_decl_kind =
            Z3_get_decl_kind(ctx->z3_ctx, child_decl);

        node_app       = child_app;
        node_decl      = child_decl;
        node_decl_kind = child_decl_kind;
        is_app         = child_is_app;

        if (child_decl_kind == Z3_OP_EQ || child_decl_kind == Z3_OP_UGEQ ||
            child_decl_kind == Z3_OP_SGEQ || child_decl_kind == Z3_OP_ULEQ ||
            child_decl_kind == Z3_OP_SLEQ || child_decl_kind == Z3_OP_UGT ||
            child_decl_kind == Z3_OP_SGT || child_decl_kind == Z3_OP_ULT ||
            child_decl_kind == Z3_OP_SLT) {

            op_type = child_decl_kind;
            is_neg  = 1;
        } else {
            data->is_input_to_state = 0;
            return;
        }
    } else {
        data->is_input_to_state = 0;
        return;
    }

    // condition 2 - one child is input-to-state
    int      condition_ok = 0;
    unsigned its_operand  = 0;
    unsigned num_fields   = Z3_get_app_num_args(ctx->z3_ctx, node_app);
    unsigned i;
    for (i = 0; i < num_fields; ++i) {
        Z3_ast child = Z3_get_app_arg(ctx->z3_ctx, node_app, i);
        Z3_inc_ref(ctx->z3_ctx, child);
        char approx;
        condition_ok = __detect_input_group(
                           ctx, child, &data->input_to_state_group, &approx) &&
                       data->input_to_state_group.n > 0;
        Z3_dec_ref(ctx->z3_ctx, child);
        if (condition_ok) {
            its_operand = i;
            break;
        }
    }

    if (!condition_ok) {
        data->is_input_to_state = 0;
        return;
    }

    // The other child is a constant?
    condition_ok           = 0;
    unsigned const_operand = its_operand == 1 ? 0 : 1;
    Z3_ast   other_child = Z3_get_app_arg(ctx->z3_ctx, node_app, const_operand);
    Z3_inc_ref(ctx->z3_ctx, other_child);
    if (Z3_get_ast_kind(ctx->z3_ctx, other_child) == Z3_NUMERAL_AST) {
        Z3_bool successGet = Z3_get_numeral_uint64(
            ctx->z3_ctx, other_child, (uint64_t*)&data->input_to_state_const);
        if (successGet == Z3_FALSE) {
            Z3_dec_ref(ctx->z3_ctx, other_child);
            data->is_input_to_state = 0;
            return; // constant is too big
        }
        condition_ok = 1;
    }

    if (!condition_ok) {
        if (constant_strict) {
            data->is_input_to_state = 0;
            Z3_dec_ref(ctx->z3_ctx, other_child);
            return;
        }
        // Not a constant, lets evaluate the AST in the current testcase...
        testcase_t* current_testcase = &ctx->testcases.data[0];

        data->input_to_state_const = ctx->model_eval(
            ctx->z3_ctx, other_child, current_testcase->values,
            current_testcase->value_sizes, current_testcase->values_len, NULL);
    }

    Z3_dec_ref(ctx->z3_ctx, other_child);

    if (is_neg && (op_type == Z3_OP_EQ || op_type == Z3_OP_UGEQ ||
                   op_type == Z3_OP_SGEQ)) {
        data->input_to_state_const += (const_operand == 0 ? 1 : -1);
    } else if (is_neg && (op_type == Z3_OP_ULEQ || op_type == Z3_OP_SLEQ)) {
        data->input_to_state_const += (const_operand == 0 ? -1 : 1);
    } else if (!is_neg && (op_type == Z3_OP_UGT || op_type == Z3_OP_SGT)) {
        data->input_to_state_const += (const_operand == 0 ? -1 : 1);
    } else if (!is_neg && (op_type == Z3_OP_ULT || op_type == Z3_OP_SLT)) {
        data->input_to_state_const += (const_operand == 1 ? -1 : 1);
    }

    data->op_input_to_state = op_type;
    data->is_input_to_state = 1;
    return;
}

static void __union_ast_info(ast_info_ptr dst, ast_info_ptr src)
{
    index_group_t* group;
    set_reset_iter__index_group_t(&src->index_groups, 0);
    while (set_iter_next__index_group_t(&src->index_groups, 0, &group)) {
        set_add__index_group_t(&dst->index_groups, *group);
    }

    ulong* p;
    set_reset_iter__ulong(&src->indexes, 0);
    while (set_iter_next__ulong(&src->indexes, 0, &p)) {
        set_add__ulong(&dst->indexes, *p);
    }

    unsigned long i;
    for (i = 0; i < src->indexes_ud.size; ++i)
        if (!da_check_el__ulong(&dst->indexes_ud, src->indexes_ud.data[i]))
            da_add_item__ulong(&dst->indexes_ud, src->indexes_ud.data[i]);
    for (i = 0; i < src->index_groups_ud.size; ++i)
        if (!da_check_el__index_group_t(&dst->index_groups_ud,
                                        &src->index_groups_ud.data[i]))
            da_add_item__index_group_t(&dst->index_groups_ud,
                                       src->index_groups_ud.data[i]);
    for (i = 0; i < src->inp_to_state_ite.size; ++i)
        if (!da_check_el__ite_its_t(&dst->inp_to_state_ite,
                                    &src->inp_to_state_ite.data[i]))
            da_add_item__ite_its_t(&dst->inp_to_state_ite,
                                   src->inp_to_state_ite.data[i]);

    dst->input_extract_ops += src->input_extract_ops;
    dst->linear_arithmetic_operations += src->linear_arithmetic_operations;
    dst->nonlinear_arithmetic_operations +=
        src->nonlinear_arithmetic_operations;
    dst->query_size += src->query_size;
    dst->approximated_groups += src->approximated_groups;
}

static void ast_info_populate_with_blacklist(ast_info_ptr dst, ast_info_ptr src,
                                             set__ulong* blacklist)
{
    ulong* p;
    set_reset_iter__ulong(&src->indexes, 0);
    while (set_iter_next__ulong(&src->indexes, 0, &p))
        if (!set_check__ulong(blacklist, *p))
            set_add__ulong(&dst->indexes, *p);

    index_group_t* group;
    set_reset_iter__index_group_t(&src->index_groups, 0);
    while (set_iter_next__index_group_t(&src->index_groups, 0, &group)) {
        char     is_ok = 1;
        unsigned i;
        for (i = 0; i < group->n; ++i)
            if (set_check__ulong(blacklist, group->indexes[i])) {
                is_ok = 0;
                break;
            }

        if (is_ok)
            set_add__index_group_t(&dst->index_groups, *group);
        else {
            // add indexes as individual groups
            index_group_t g;
            for (i = 0; i < group->n; ++i)
                if (!set_check__ulong(blacklist, group->indexes[i])) {
                    g.n          = 1;
                    g.indexes[0] = group->indexes[i];
                    set_add__index_group_t(&dst->index_groups, g);
                }
        }
    }
}

static inline int is_and_constraint(fuzzy_ctx_t* ctx, Z3_ast branch_condition,
                                    int* with_not)
{
    Z3_ast_kind node_kind = Z3_get_ast_kind(ctx->z3_ctx, branch_condition);
    if (node_kind != Z3_APP_AST)
        return 0;

    Z3_app       node_app       = Z3_to_app(ctx->z3_ctx, branch_condition);
    Z3_func_decl node_decl      = Z3_get_app_decl(ctx->z3_ctx, node_app);
    Z3_decl_kind node_decl_kind = Z3_get_decl_kind(ctx->z3_ctx, node_decl);

    if (node_decl_kind == Z3_OP_AND) {
        *with_not = 0;
        return 1;
    }

    int is_not = 0;
    while (node_decl_kind == Z3_OP_NOT) {
        branch_condition = Z3_get_app_arg(ctx->z3_ctx, node_app, 0);
        node_app         = Z3_to_app(ctx->z3_ctx, branch_condition);
        node_decl        = Z3_get_app_decl(ctx->z3_ctx, node_app);
        node_decl_kind   = Z3_get_decl_kind(ctx->z3_ctx, node_decl);
        is_not           = !is_not;
    }
    if (is_not && node_decl_kind == Z3_OP_OR) {
        *with_not = 1;
        return 1;
    }

    return 0;
}

static inline void flatten_and_args(fuzzy_ctx_t* ctx, Z3_ast node,
                                    da__Z3_ast* args)
{
    Z3_app       app        = Z3_to_app(ctx->z3_ctx, node);
    unsigned     num_fields = Z3_get_app_num_args(ctx->z3_ctx, app);
    Z3_func_decl decl       = Z3_get_app_decl(ctx->z3_ctx, app);
    Z3_decl_kind decl_kind  = Z3_get_decl_kind(ctx->z3_ctx, decl);

    int negated = 0;
    if (decl_kind != Z3_OP_AND) {
        int is_not = 0;
        while (decl_kind == Z3_OP_NOT) {
            node      = Z3_get_app_arg(ctx->z3_ctx, app, 0);
            app       = Z3_to_app(ctx->z3_ctx, node);
            decl      = Z3_get_app_decl(ctx->z3_ctx, app);
            decl_kind = Z3_get_decl_kind(ctx->z3_ctx, decl);
            is_not    = !is_not;
        }
        negated = is_not;
        ASSERT_OR_ABORT(decl_kind == Z3_OP_OR,
                        "flatten_and_args: not an and constraint");
        num_fields = Z3_get_app_num_args(ctx->z3_ctx, app);
    }

    int      with_not;
    unsigned i;
    for (i = 0; i < num_fields; ++i) {
        Z3_ast child = Z3_get_app_arg(ctx->z3_ctx, app, i);
        if (is_and_constraint(ctx, child, &with_not))
            flatten_and_args(ctx, child, args);
        else {
            if (negated)
                child = Z3_mk_not(ctx->z3_ctx, child);
            Z3_inc_ref(ctx->z3_ctx, child);
            da_add_item__Z3_ast(args, child);
        }
    }
}

static inline void __detect_involved_inputs(fuzzy_ctx_t* ctx, Z3_ast v,
                                            ast_info_ptr* data)
{
    // visit the AST and collect some data
    // 1. Find "groups" of inputs involved in the AST and store them in
    // 'index_queue'
    // 2. Populate global 'indexes' with encountered indexes

    unsigned long       ast_hash = Z3_UNIQUE(ctx->z3_ctx, v);
    dict__ast_info_ptr* ast_info_cache =
        (dict__ast_info_ptr*)ctx->ast_info_cache;
    ast_info_ptr* cached_el;
    if ((cached_el = dict_get_ref__ast_info_ptr(ast_info_cache, ast_hash)) !=
        NULL) {
        ctx->stats.ast_info_cache_hits++;
        *data = *cached_el;
        return;
    }
    ast_info_ptr new_el = (ast_info_ptr)malloc(sizeof(ast_info_t));
    ast_info_init(new_el);

    switch (Z3_get_ast_kind(ctx->z3_ctx, v)) {
        case Z3_NUMERAL_AST: {
            new_el->query_size++;
            break;
        }
        case Z3_APP_AST: {
            new_el->query_size++;
            unsigned     i;
            Z3_app       app        = Z3_to_app(ctx->z3_ctx, v);
            unsigned     num_fields = Z3_get_app_num_args(ctx->z3_ctx, app);
            Z3_func_decl decl       = Z3_get_app_decl(ctx->z3_ctx, app);
            Z3_decl_kind decl_kind  = Z3_get_decl_kind(ctx->z3_ctx, decl);

            switch (decl_kind) {
                case Z3_OP_EXTRACT:
                case Z3_OP_BAND:
                case Z3_OP_BADD:
                case Z3_OP_BOR:
                case Z3_OP_CONCAT: {
                    index_group_t group  = {0};
                    char          approx = 0;
                    if (__detect_input_group(ctx, v, &group, &approx) &&
                        group.n > 0) {
                        new_el->approximated_groups += approx;
                        // concat chain. commit
                        unsigned i, at_least_one = 0;
                        for (i = 0; i < group.n; ++i)
                            if (!set_check__ulong(
                                    (set__ulong*)ctx->univocally_defined_inputs,
                                    group.indexes[i])) {
                                set_add__ulong(&new_el->indexes,
                                               group.indexes[i]);
                                at_least_one = 1;
                            } else if (!da_check_el__ulong(
                                           &new_el->indexes_ud,
                                           group.indexes[i])) { // linear check,
                                                                // but it
                                                                // should
                                                                // be small...
                                da_add_item__ulong(&new_el->indexes_ud,
                                                   group.indexes[i]);
                            }

                        if (at_least_one)
                            set_add__index_group_t(&new_el->index_groups,
                                                   group);
                        else if (!da_check_el__index_group_t(
                                     &new_el->index_groups_ud,
                                     &group)) // linear check, but it should be
                                              // small...
                            da_add_item__index_group_t(&new_el->index_groups_ud,
                                                       group);

                        goto FUN_END;
                    } else {
                        if (decl_kind == Z3_OP_EXTRACT ||
                            decl_kind == Z3_OP_BAND)
                            // extract op not within a group. Take note
                            new_el->input_extract_ops++;
                    }
                    break;
                }
                case Z3_OP_UNINTERPRETED: {
                    index_group_t group = {0};
                    Z3_symbol     s     = Z3_get_decl_name(ctx->z3_ctx, decl);
                    int symbol_index    = Z3_get_symbol_int(ctx->z3_ctx, s);

                    if (symbol_index >= ctx->testcases.data[0].testcase_len) {
                        // the symbol is indeed an assignment. Resolve the
                        // assignment
                        ast_info_ptr tmp;
                        __detect_involved_inputs(
                            ctx, ctx->assignments[symbol_index], &tmp);
                        __union_ast_info(new_el, tmp);
                        break;
                    }

                    group.indexes[group.n++] = symbol_index;

                    if (!set_check__ulong(
                            (set__ulong*)ctx->univocally_defined_inputs,
                            symbol_index)) {
                        set_add__index_group_t(&new_el->index_groups, group);
                        set_add__ulong(&new_el->indexes, symbol_index);
                    } else if (!da_check_el__ulong(
                                   &new_el->indexes_ud,
                                   symbol_index)) { // linear check, but it
                                                    // should
                                                    // be small...
                        da_add_item__ulong(&new_el->indexes_ud, symbol_index);
                        da_add_item__index_group_t(&new_el->index_groups_ud,
                                                   group);
                    }
                    goto FUN_END;
                }
                case Z3_OP_BLSHR:
                case Z3_OP_BASHR:
                case Z3_OP_BUDIV0:
                case Z3_OP_BUDIV:
                case Z3_OP_BUDIV_I:
                case Z3_OP_BSDIV0:
                case Z3_OP_BSDIV:
                case Z3_OP_BSDIV_I:
                case Z3_OP_BUREM:
                case Z3_OP_BUREM_I:
                case Z3_OP_BSREM:
                case Z3_OP_BSREM_I:
                case Z3_OP_BSMOD: {
                    new_el->input_extract_ops++;
                    break;
                }
                case Z3_OP_ITE: {
                    // look in ite condition
                    Z3_ast cond = Z3_get_app_arg(ctx->z3_ctx, app, 0);
                    Z3_inc_ref(ctx->z3_ctx, cond);
                    da__Z3_ast and_vals;
                    da_init__Z3_ast(&and_vals);
                    int with_not;
                    if (is_and_constraint(ctx, cond, &with_not))
                        flatten_and_args(ctx, cond, &and_vals);
                    else {
                        Z3_inc_ref(ctx->z3_ctx, cond);
                        da_add_item__Z3_ast(&and_vals, cond);
                    }

                    unsigned i;
                    for (i = 0; i < and_vals.size; ++i) {
                        ast_data_t tmp = {0};
                        __detect_input_to_state_query(ctx, and_vals.data[i],
                                                      &tmp, 1);
                        if (tmp.is_input_to_state) {
                            ite_its_t its_el = {.ig = tmp.input_to_state_group,
                                                .val =
                                                    tmp.input_to_state_const};
                            da_add_item__ite_its_t(&new_el->inp_to_state_ite,
                                                   its_el);
                        }
                        Z3_dec_ref(ctx->z3_ctx, and_vals.data[i]);
                    }
                    da_free__Z3_ast(&and_vals, NULL);
                    Z3_dec_ref(ctx->z3_ctx, cond);
                    break;
                }
                default: {
                    break;
                }
            }
            if (num_fields > 0) {
                for (i = 0; i < num_fields; i++) {
                    Z3_ast child = Z3_get_app_arg(ctx->z3_ctx, app, i);
                    Z3_inc_ref(ctx->z3_ctx, child);
                    ast_info_ptr tmp;
                    __detect_involved_inputs(ctx, child, &tmp);
                    __union_ast_info(new_el, tmp);
                    Z3_dec_ref(ctx->z3_ctx, child);
                }
            }
            break;
        }
        case Z3_QUANTIFIER_AST: {
            ASSERT_OR_ABORT(0, "__main_ast_visit() found quantifier\n");
        }
        default:
            ASSERT_OR_ABORT(0, "__main_ast_visit() unknown ast kind\n");
    }

FUN_END:
    dict_set__ast_info_ptr(ast_info_cache, ast_hash, new_el);
    *data = new_el;
}

static void detect_involved_inputs_wrapper(fuzzy_ctx_t* ctx, Z3_ast v,
                                           ast_info_ptr* data)
{
    __detect_involved_inputs(ctx, v, data);
}

static void __detect_early_constants(fuzzy_ctx_t* ctx, Z3_ast v,
                                     ast_data_t* data)
{
    // look for constants in early SUB/AND and in early EQ/GE/GT/LE/LT/SLE/SLT
    unsigned long tmp_const;
    switch (Z3_get_ast_kind(ctx->z3_ctx, v)) {
        case Z3_APP_AST: {
            Z3_bool      successGet;
            Z3_ast       child1, child2;
            Z3_app       app       = Z3_to_app(ctx->z3_ctx, v);
            Z3_func_decl decl      = Z3_get_app_decl(ctx->z3_ctx, app);
            Z3_decl_kind decl_kind = Z3_get_decl_kind(ctx->z3_ctx, decl);

            switch (decl_kind) {
                case Z3_OP_EXTRACT:
                case Z3_OP_NOT: {
                    // unary forward
                    child1 = Z3_get_app_arg(ctx->z3_ctx, app, 0);
                    __detect_early_constants(ctx, child1, data);
                    break;
                }
                case Z3_OP_CONCAT: {
                    child1 = Z3_get_app_arg(ctx->z3_ctx, app, 0);
                    Z3_inc_ref(ctx->z3_ctx, child1);
                    child2 = Z3_get_app_arg(ctx->z3_ctx, app, 1);
                    Z3_inc_ref(ctx->z3_ctx, child2);

                    __detect_early_constants(ctx, child1, data);
                    Z3_dec_ref(ctx->z3_ctx, child1);
                    __detect_early_constants(ctx, child2, data);
                    Z3_dec_ref(ctx->z3_ctx, child2);
                    break;
                }
                case Z3_OP_OR:
                case Z3_OP_AND: {
                    unsigned num_fields = Z3_get_app_num_args(ctx->z3_ctx, app);
                    unsigned i          = 0;
                    for (i = 0; i < num_fields; ++i) {
                        Z3_ast child = Z3_get_app_arg(ctx->z3_ctx, app, i);
                        Z3_inc_ref(ctx->z3_ctx, child);
                        __detect_early_constants(ctx, child, data);
                        Z3_dec_ref(ctx->z3_ctx, child);
                    }
                    break;
                }
                case Z3_OP_EQ:
                case Z3_OP_UGEQ:
                case Z3_OP_SGEQ:
                case Z3_OP_UGT:
                case Z3_OP_SGT:
                case Z3_OP_ULEQ:
                case Z3_OP_ULT:
                case Z3_OP_SLT:
                case Z3_OP_SLEQ: {
                    child1 = Z3_get_app_arg(ctx->z3_ctx, app, 0);
                    Z3_inc_ref(ctx->z3_ctx, child1);
                    child2 = Z3_get_app_arg(ctx->z3_ctx, app, 1);
                    Z3_inc_ref(ctx->z3_ctx, child2);

                    if (Z3_get_ast_kind(ctx->z3_ctx, child1) ==
                        Z3_NUMERAL_AST) {
                        successGet = Z3_get_numeral_uint64(
                            ctx->z3_ctx, child1, (uint64_t*)&tmp_const);
                        if (successGet == Z3_FALSE)
                            break; // constant bigger than 64
                        da_add_item__ulong(&data->values, tmp_const);
                        da_add_item__ulong(&data->values, tmp_const + 1);
                        da_add_item__ulong(&data->values, tmp_const - 1);
                    } else if (Z3_get_ast_kind(ctx->z3_ctx, child2) ==
                               Z3_NUMERAL_AST) {
                        successGet = Z3_get_numeral_uint64(
                            ctx->z3_ctx, child2, (uint64_t*)&tmp_const);
                        if (successGet == Z3_FALSE)
                            break; // constant bigger than 64
                        da_add_item__ulong(&data->values, tmp_const);
                        da_add_item__ulong(&data->values, tmp_const + 1);
                        da_add_item__ulong(&data->values, tmp_const - 1);
                    }

                    // binary forward
                    __detect_early_constants(ctx, child1, data);
                    Z3_dec_ref(ctx->z3_ctx, child1);
                    __detect_early_constants(ctx, child2, data);
                    Z3_dec_ref(ctx->z3_ctx, child2);
                    break;
                }
                case Z3_OP_BSUB:
                case Z3_OP_BADD:
                case Z3_OP_BAND: {
                    // look for constant
                    child1 = Z3_get_app_arg(ctx->z3_ctx, app, 0);
                    Z3_inc_ref(ctx->z3_ctx, child1);
                    child2 = Z3_get_app_arg(ctx->z3_ctx, app, 1);
                    Z3_inc_ref(ctx->z3_ctx, child2);

                    if (Z3_get_ast_kind(ctx->z3_ctx, child1) ==
                        Z3_NUMERAL_AST) {
                        successGet = Z3_get_numeral_uint64(
                            ctx->z3_ctx, child1, (uint64_t*)&tmp_const);
                        ASSERT_OR_ABORT(successGet == Z3_TRUE,
                                        "failed to get constant");
                        da_add_item__ulong(&data->values, tmp_const);
                        da_add_item__ulong(&data->values, tmp_const + 1);
                        da_add_item__ulong(&data->values, tmp_const - 1);
                    } else if (Z3_get_ast_kind(ctx->z3_ctx, child2) ==
                               Z3_NUMERAL_AST) {
                        successGet = Z3_get_numeral_uint64(
                            ctx->z3_ctx, child2, (uint64_t*)&tmp_const);
                        ASSERT_OR_ABORT(successGet == Z3_TRUE,
                                        "failed to get constant");
                        da_add_item__ulong(&data->values, tmp_const);
                        da_add_item__ulong(&data->values, tmp_const + 1);
                        da_add_item__ulong(&data->values, tmp_const - 1);
                    }
                    Z3_dec_ref(ctx->z3_ctx, child1);
                    Z3_dec_ref(ctx->z3_ctx, child2);
                    break;
                }
                case Z3_OP_ITE: {
                    // look in ite condition
                    Z3_ast cond = Z3_get_app_arg(ctx->z3_ctx, app, 0);
                    Z3_inc_ref(ctx->z3_ctx, cond);
                    __detect_early_constants(ctx, cond, data);
                    Z3_dec_ref(ctx->z3_ctx, cond);
                    break;
                }
                default: {
                    break;
                }
            }
            break;
        }
        default: {
            break;
        }
    }
    return;
}

static inline unsigned long get_group_value_in_tmp_input(index_group_t* group)
{
    unsigned long res = 0;
    unsigned char k;
    for (k = 0; k < group->n; ++k) {
        unsigned long index = group->indexes[group->n - k - 1];
        res |= tmp_input[index] << (k * 8);
    }
    return res;
}

static inline unsigned long
get_group_value_in_tmp_input_inv(index_group_t* group)
{
    unsigned long res = 0;
    unsigned char k;
    for (k = 0; k < group->n; ++k) {
        unsigned long index = group->indexes[k];
        res |= tmp_input[index] << (k * 8);
    }
    return res;
}

static inline unsigned char __extract_from_long(long value, unsigned int i)
{
    return (value >> i * 8) & 0xff;
}

static inline void set_tmp_input_group_to_value(index_group_t* group,
                                                uint64_t       v)
{
    unsigned char k;
    for (k = 0; k < group->n; ++k) {
        unsigned long index = group->indexes[group->n - k - 1];
        unsigned char b     = __extract_from_long(v, k);
        tmp_input[index]    = b;
    }
}

static inline void set_tmp_input_group_to_value_inv(index_group_t* group,
                                                    uint64_t       v)
{
    unsigned char k;
    for (k = 0; k < group->n; ++k) {
        unsigned long index = group->indexes[k];
        unsigned char b     = __extract_from_long(v, k);
        tmp_input[index]    = b;
    }
}

static inline void restore_tmp_input_group(index_group_t* group,
                                           unsigned long* vals)
{
    unsigned char k;
    for (k = 0; k < group->n; ++k) {
        unsigned long index = group->indexes[group->n - k - 1];
        tmp_input[index]    = vals[index];
    }
}

static void __put_solutions_of_current_groups_to_early_constants(
    fuzzy_ctx_t* ctx, ast_data_t* data, ast_info_ptr curr_groups)
{
    index_group_t* ig;
    set_reset_iter__index_group_t(&curr_groups->index_groups, 0);
    while (set_iter_next__index_group_t(&curr_groups->index_groups, 0, &ig)) {
        unsigned long val = get_group_value_in_tmp_input(ig);
        da_add_item__ulong(&data->values, val);
    }
}

__attribute__((unused)) static void
__detect_all_constants(fuzzy_ctx_t* ctx, Z3_ast v, ast_data_t* data)
{
    switch (Z3_get_ast_kind(ctx->z3_ctx, v)) {
        case Z3_APP_AST: {
            Z3_app   app        = Z3_to_app(ctx->z3_ctx, v);
            unsigned num_fields = Z3_get_app_num_args(ctx->z3_ctx, app);
            unsigned i;
            for (i = 0; i < num_fields; ++i) {
                Z3_ast child = Z3_get_app_arg(ctx->z3_ctx, app, i);
                __detect_all_constants(ctx, child, data);
            }
            break;
        }
        case Z3_NUMERAL_AST: {
            unsigned long tmp_const;
            Z3_bool       successGet =
                Z3_get_numeral_uint64(ctx->z3_ctx, v, (uint64_t*)&tmp_const);
            ASSERT_OR_ABORT(successGet == Z3_TRUE, "failed to get constant");
            da_add_item__ulong(&data->values, tmp_const);
            da_add_item__ulong(&data->values, tmp_const + 1);
            da_add_item__ulong(&data->values, tmp_const - 1);
        }
        default: {
            break;
        }
    }
    return;
}

static inline int __check_conflicting_constraint(fuzzy_ctx_t* ctx, Z3_ast expr)
{
    Z3_ast_kind kind = Z3_get_ast_kind(ctx->z3_ctx, expr);
    if (kind != Z3_APP_AST)
        return 0;

    int res = 0;
    Z3_inc_ref(ctx->z3_ctx, expr);
    Z3_app       app       = Z3_to_app(ctx->z3_ctx, expr);
    Z3_func_decl decl      = Z3_get_app_decl(ctx->z3_ctx, app);
    Z3_decl_kind decl_kind = Z3_get_decl_kind(ctx->z3_ctx, decl);
    Z3_ast       old_expr  = expr;
    Z3_inc_ref(ctx->z3_ctx, old_expr);

    // exclude initial NOT
    int is_not = 0;
    while (decl_kind == Z3_OP_NOT) {
        Z3_ast tmp_expr = Z3_get_app_arg(ctx->z3_ctx, app, 0);
        Z3_inc_ref(ctx->z3_ctx, tmp_expr);
        Z3_dec_ref(ctx->z3_ctx, expr);
        expr      = tmp_expr;
        app       = Z3_to_app(ctx->z3_ctx, expr);
        decl      = Z3_get_app_decl(ctx->z3_ctx, app);
        decl_kind = Z3_get_decl_kind(ctx->z3_ctx, decl);
        is_not    = !is_not;
    }

    if (decl_kind != Z3_OP_EQ && decl_kind != Z3_OP_SLEQ &&
        decl_kind != Z3_OP_ULEQ && decl_kind != Z3_OP_SLT &&
        decl_kind != Z3_OP_ULT && decl_kind != Z3_OP_SGEQ &&
        decl_kind != Z3_OP_UGEQ && decl_kind != Z3_OP_SGT &&
        decl_kind != Z3_OP_UGT && decl_kind != Z3_OP_OR) {
        res = 0;
        goto OUT;
    }

    if (is_not) {
        Z3_dec_ref(ctx->z3_ctx, expr);
        expr = old_expr;
        Z3_inc_ref(ctx->z3_ctx, expr);
    }

    ast_info_ptr inputs;
    detect_involved_inputs_wrapper(ctx, expr, &inputs);
#if 1
    if (inputs->query_size > 1020) {
        res = 0;
        goto OUT;
    }
#endif

    // Take note of groups
    dict__conflicting_ptr* conflicting_asts =
        (dict__conflicting_ptr*)ctx->conflicting_asts;

    index_group_t* ig = NULL;
    set_reset_iter__index_group_t(&inputs->index_groups, 0);
    while (set_iter_next__index_group_t(&inputs->index_groups, 0, &ig)) {
        unsigned i;
        for (i = 0; i < ig->n; ++i)
            add_item_to_conflicting(conflicting_asts, expr, ig->indexes[i],
                                    ctx->z3_ctx);
    }
    res = 1;
OUT:
    Z3_dec_ref(ctx->z3_ctx, expr);
    Z3_dec_ref(ctx->z3_ctx, old_expr);
    return res;
}

static inline optype __find_optype(Z3_decl_kind dk, int is_const_at_right,
                                   int has_zext)
{
    switch (dk) {
        case Z3_OP_SLEQ:
            if (is_const_at_right)
                if (!has_zext)
                    return OP_SLE;
                else
                    return OP_ULE;
            else if (!has_zext)
                return OP_SGE;
            else
                return OP_UGE;
        case Z3_OP_ULEQ:
            if (is_const_at_right)
                return OP_ULE;
            else
                return OP_UGE;
        case Z3_OP_SLT:
            if (is_const_at_right)
                if (!has_zext)
                    return OP_SLT;
                else
                    return OP_ULT;
            else if (!has_zext)
                return OP_SGT;
            else
                return OP_UGT;
        case Z3_OP_ULT:
            if (is_const_at_right)
                return OP_ULT;
            else
                return OP_UGT;
        case Z3_OP_SGEQ:
            if (is_const_at_right)
                if (!has_zext)
                    return OP_SGE;
                else
                    return OP_UGE;
            else if (!has_zext)
                return OP_SLE;
            else
                return OP_ULE;
        case Z3_OP_UGEQ:
            if (is_const_at_right)
                return OP_UGE;
            else
                return OP_ULE;
        case Z3_OP_SGT:
            if (is_const_at_right)
                if (!has_zext)
                    return OP_SGT;
                else
                    return OP_UGT;
            else if (!has_zext)
                return OP_SLT;
            else
                return OP_ULT;
        case Z3_OP_UGT:
            if (is_const_at_right)
                return OP_UGT;
            else
                return OP_ULT;
        case Z3_OP_EQ:
            return OP_EQ;
        default:
            break;
    }
    ABORT("__find_optype() unexpected Z3_decl_kind");
}

static inline int __find_child_constant(Z3_context ctx, Z3_app app,
                                        uint64_t* constant,
                                        unsigned* const_operand,
                                        unsigned* const_size)
{
    int      condition_ok = 0;
    unsigned num_fields   = Z3_get_app_num_args(ctx, app);

    unsigned i;
    for (i = 0; i < num_fields; ++i) {
        Z3_ast child = Z3_get_app_arg(ctx, app, i);
        if (Z3_get_ast_kind(ctx, child) == Z3_NUMERAL_AST) {
            Z3_bool successGet =
                Z3_get_numeral_uint64(ctx, child, (uint64_t*)constant);
            if (successGet == Z3_FALSE)
                return 0; // constant is too big
            condition_ok   = 1;
            *const_operand = i;
            *const_size    = Z3_get_bv_sort_size(ctx, Z3_get_sort(ctx, child));
            break;
        }
    }
    return condition_ok;
}

static inline Z3_decl_kind get_opposite_decl_kind(Z3_decl_kind kind)
{
    switch (kind) {
        case Z3_OP_SLEQ:
            return Z3_OP_SGT;
        case Z3_OP_ULEQ:
            return Z3_OP_UGT;
        case Z3_OP_SLT:
            return Z3_OP_SGEQ;
        case Z3_OP_ULT:
            return Z3_OP_UGEQ;
        case Z3_OP_SGEQ:
            return Z3_OP_SLT;
        case Z3_OP_UGEQ:
            return Z3_OP_ULT;
        case Z3_OP_SGT:
            return Z3_OP_SLEQ;
        case Z3_OP_UGT:
            return Z3_OP_ULEQ;
        default:
            break;
    }
    ABORT("get_opposite_decl_kind() - unexpected decl kind");
}

static inline int is_signed_op(optype op)
{
    return op == OP_SGT || op == OP_SGE || op == OP_SLT || op == OP_SLE;
}

static inline unsigned size_normalized(unsigned size)
{
    switch (size) {
        case 1:
            return 1;
        case 2:
            return 2;
        case 3:
        case 4:
            return 4;
        case 5:
        case 6:
        case 7:
        case 8:
            return 8;
        default:
            break;
    }
    ABORT("size_normalized() - unexpected size");
}

static inline interval_group_ptr interval_group_set_add_or_modify(
    set__interval_group_ptr* set, index_group_t* ig, uint64_t c, optype op,
    uint64_t add_constant, uint64_t sub_constant, int should_invert,
    uint32_t add_sub_const_size, uint32_t const_size, int* created_new)
{
    ASSERT_OR_ABORT(op != -1,
                    "interval_group_set_add_or_modify() invalid optype");
    interval_group_t    igt     = {.group = *ig, .interval = {0}};
    interval_group_ptr  igt_p   = &igt;
    interval_group_ptr* igt_ptr = set_find_el__interval_group_ptr(set, &igt_p);

    wrapped_interval_t wi = wi_init(const_size);
    wi_update_cmp(&wi, c, op);
    if (add_constant > 0) {
        wi_modify_size(&wi, add_sub_const_size);
        wi_update_sub(&wi, add_constant);
    }
    if (sub_constant > 0) {
        wi_modify_size(&wi, add_sub_const_size);
        if (!should_invert)
            wi_update_add(&wi, sub_constant);
        else {
            wi_update_invert(&wi);
            wi_update_add(&wi, sub_constant);
        }
    }

    wi_modify_size(&wi, ig->n * 8);
    if (igt_ptr != NULL) {
        wi_intersect(&(*igt_ptr)->interval, &wi);
        return *igt_ptr;
    } else {
        *created_new = 1;
        interval_group_ptr new_el =
            (interval_group_ptr)malloc(sizeof(interval_group_t));
        unsigned size    = ig->n;
        size             = size_normalized(size);
        new_el->interval = wi;
        new_el->group    = *ig;
        set_add__interval_group_ptr(set, new_el);
        return new_el;
    }
}

static inline void
update_or_create_in_index_to_group_intervals(dict__da__interval_group_ptr* dict,
                                             unsigned long                 idx,
                                             interval_group_ptr            el)
{
    da__interval_group_ptr* el_list;
    el_list = dict_get_ref__da__interval_group_ptr(dict, idx);
    if (el_list == NULL) {
        da__interval_group_ptr new_list;
        da_init__interval_group_ptr(&new_list);
        da_add_item__interval_group_ptr(&new_list, el);
        dict_set__da__interval_group_ptr(dict, idx, new_list);
    } else {
        da_add_item__interval_group_ptr(el_list, el);
    }
}

static inline wrapped_interval_t*
interval_group_get_interval(set__interval_group_ptr* set, index_group_t* ig)
{
    interval_group_t    igt     = {.group = *ig, .interval = {0}};
    interval_group_ptr  igt_p   = &igt;
    interval_group_ptr* igt_ptr = set_find_el__interval_group_ptr(set, &igt_p);
    if (igt_ptr != NULL)
        return &(*igt_ptr)->interval;
    return NULL;
}

static inline int __check_if_range(fuzzy_ctx_t* ctx, Z3_ast expr,
                                   // output args
                                   index_group_t* ig, uint64_t* constant,
                                   optype* op, uint64_t* add_constant,
                                   uint64_t* sub_constant, int* should_invert,
                                   uint32_t* add_sub_const_size,
                                   unsigned* const_size)
{
    int         res  = 0;
    Z3_ast_kind kind = Z3_get_ast_kind(ctx->z3_ctx, expr);
    if (kind != Z3_APP_AST)
        return 0;

    Z3_inc_ref(ctx->z3_ctx, expr);
    Z3_app       app       = Z3_to_app(ctx->z3_ctx, expr);
    Z3_func_decl decl      = Z3_get_app_decl(ctx->z3_ctx, app);
    Z3_decl_kind decl_kind = Z3_get_decl_kind(ctx->z3_ctx, decl);

    Z3_ast original_expr = expr;
    Z3_inc_ref(ctx->z3_ctx, original_expr);

    // exclude initial NOT
    int is_not = 0;
    while (decl_kind == Z3_OP_NOT) {
        Z3_ast tmp_expr = Z3_get_app_arg(ctx->z3_ctx, app, 0);
        Z3_inc_ref(ctx->z3_ctx, tmp_expr);
        Z3_dec_ref(ctx->z3_ctx, expr);
        expr      = tmp_expr;
        app       = Z3_to_app(ctx->z3_ctx, expr);
        decl      = Z3_get_app_decl(ctx->z3_ctx, app);
        decl_kind = Z3_get_decl_kind(ctx->z3_ctx, decl);
        is_not    = !is_not;
    }

    // it is a range query
    if (decl_kind != Z3_OP_SLEQ && decl_kind != Z3_OP_ULEQ &&
        decl_kind != Z3_OP_SLT && decl_kind != Z3_OP_ULT &&
        decl_kind != Z3_OP_SGEQ && decl_kind != Z3_OP_UGEQ &&
        decl_kind != Z3_OP_SGT && decl_kind != Z3_OP_UGT &&
        decl_kind != Z3_OP_EQ)
        goto END_FUN_1;
    if (is_not && decl_kind == Z3_OP_EQ)
        // could be extended...
        goto END_FUN_1;
    if (is_not)
        decl_kind = get_opposite_decl_kind(decl_kind);

    // should be always the case
    if (Z3_get_app_num_args(ctx->z3_ctx, app) != 2)
        goto END_FUN_1;

    // one of the two child is a constant
    unsigned const_operand;
    if (!__find_child_constant(ctx->z3_ctx, app, constant, &const_operand,
                               const_size))
        goto END_FUN_1;

    // the other operand is a group (possibly with an add/sub with a constant)
    Z3_ast non_const_operand =
        Z3_get_app_arg(ctx->z3_ctx, app, const_operand ^ 1);
    if (Z3_get_ast_kind(ctx->z3_ctx, non_const_operand) != Z3_APP_AST)
        goto END_FUN_1;
    if (Z3_get_sort_kind(ctx->z3_ctx,
                         Z3_get_sort(ctx->z3_ctx, non_const_operand)) !=
        Z3_BV_SORT)
        goto END_FUN_1;

    Z3_inc_ref(ctx->z3_ctx, non_const_operand);

    Z3_app       nonconst_op_app = Z3_to_app(ctx->z3_ctx, non_const_operand);
    Z3_func_decl nonconst_op_decl =
        Z3_get_app_decl(ctx->z3_ctx, nonconst_op_app);
    Z3_decl_kind nonconst_op_decl_kind =
        Z3_get_decl_kind(ctx->z3_ctx, nonconst_op_decl);

    // remove concat with 0 (extend to any constant?)
    int has_zext = 0;
    if (nonconst_op_decl_kind == Z3_OP_CONCAT) {
        if (Z3_get_app_num_args(ctx->z3_ctx, nonconst_op_app) == 2) {
            Z3_ast tmp_expr = Z3_get_app_arg(ctx->z3_ctx, nonconst_op_app, 0);
            Z3_inc_ref(ctx->z3_ctx, tmp_expr);

            if (Z3_get_ast_kind(ctx->z3_ctx, tmp_expr) == Z3_NUMERAL_AST) {
                uint64_t v;
                Z3_bool  successGet =
                    Z3_get_numeral_uint64(ctx->z3_ctx, tmp_expr, &v);

                if (successGet != Z3_FALSE && v == 0) {
                    has_zext                     = 1;
                    Z3_ast old_non_const_operand = non_const_operand;
                    non_const_operand =
                        Z3_get_app_arg(ctx->z3_ctx, nonconst_op_app, 1);
                    Z3_inc_ref(ctx->z3_ctx, non_const_operand);
                    nonconst_op_app = Z3_to_app(ctx->z3_ctx, non_const_operand);
                    nonconst_op_decl =
                        Z3_get_app_decl(ctx->z3_ctx, nonconst_op_app);
                    nonconst_op_decl_kind =
                        Z3_get_decl_kind(ctx->z3_ctx, nonconst_op_decl);
                    Z3_dec_ref(ctx->z3_ctx, old_non_const_operand);
                }
            }
            Z3_dec_ref(ctx->z3_ctx, tmp_expr);
        }
    }

    *add_constant       = 0;
    *sub_constant       = 0;
    *add_sub_const_size = 0;
    *should_invert      = 0;

    if (nonconst_op_decl_kind == Z3_OP_BADD ||
        nonconst_op_decl_kind == Z3_OP_BSUB) {

        if (Z3_get_app_num_args(ctx->z3_ctx, nonconst_op_app) != 2)
            goto END_FUN_2;

        uint64_t constant_2;
        unsigned const_operand_2;
        if (!__find_child_constant(ctx->z3_ctx, nonconst_op_app, &constant_2,
                                   &const_operand_2, add_sub_const_size))
            goto END_FUN_2;

        if (nonconst_op_decl_kind == Z3_OP_BADD) {
            *add_constant = constant_2;
        } else {
            *sub_constant = constant_2;
            if (const_operand_2 == 0)
                *should_invert = 1;
        }

        Z3_ast non_const_operand2 =
            Z3_get_app_arg(ctx->z3_ctx, nonconst_op_app, const_operand_2 ^ 1);
        Z3_inc_ref(ctx->z3_ctx, non_const_operand2);

        char approx = 0;
        int  input_group_ok =
            __detect_input_group(ctx, non_const_operand2, ig, &approx);
        if (!input_group_ok || ig->n == 0 || approx) {
            // no input group or approximated group
            Z3_dec_ref(ctx->z3_ctx, non_const_operand2);
            goto END_FUN_2;
        }

        Z3_dec_ref(ctx->z3_ctx, non_const_operand);
        non_const_operand = non_const_operand2;
    } else {
        // only one group in the non_const_operand
        char approx = 0;
        int  input_group_ok =
            __detect_input_group(ctx, non_const_operand, ig, &approx);
        if (!input_group_ok || ig->n == 0 || approx)
            // no input group or approximated group
            goto END_FUN_2;
    }
    // it is a range query!
    has_zext = 0;
    *op      = __find_optype(decl_kind, const_operand, has_zext);

    res = 1;
END_FUN_2:
    Z3_dec_ref(ctx->z3_ctx, non_const_operand);
END_FUN_1:
    Z3_dec_ref(ctx->z3_ctx, expr);
    Z3_dec_ref(ctx->z3_ctx, original_expr);
    return res;
}

static inline int __check_range_constraint(fuzzy_ctx_t* ctx, Z3_ast expr)
{
    Z3_inc_ref(ctx->z3_ctx, expr);
    int res = 0;

    index_group_t ig = {0};
    uint64_t      constant, add_constant, sub_constant;
    optype        op = -1;
    uint32_t      add_sub_const_size;
    unsigned      const_size;
    int           should_invert;

    if (!__check_if_range(ctx, expr, &ig, &constant, &op, &add_constant,
                          &sub_constant, &should_invert, &add_sub_const_size,
                          &const_size)) {
        goto OUT;
    }
    if (const_size > 64)
        goto OUT;

    set__interval_group_ptr* group_intervals =
        (set__interval_group_ptr*)ctx->group_intervals;

    dict__da__interval_group_ptr* index_to_group_intervals =
        (dict__da__interval_group_ptr*)ctx->index_to_group_intervals;

    int                created_new;
    interval_group_ptr el = interval_group_set_add_or_modify(
        group_intervals, &ig, constant, op, add_constant, sub_constant,
        should_invert, add_sub_const_size, const_size, &created_new);

    if (created_new) {
        unsigned i;
        for (i = 0; i < ig.n; ++i)
            update_or_create_in_index_to_group_intervals(
                index_to_group_intervals, ig.indexes[i], el);
    }

#ifdef DEBUG_RANGE
    puts("+++++++++++++++++++++++++++++++++++++");
    z3fuzz_print_expr(ctx, expr);
    print_interval_groups(ctx);
    puts("+++++++++++++++++++++++++++++++++++++");
#endif

    res = 1;
OUT:
    Z3_dec_ref(ctx->z3_ctx, expr);
    return res;
}

static inline int get_range(fuzzy_ctx_t* ctx, Z3_ast expr, index_group_t* ig,
                            wrapped_interval_t* wi)
{
    Z3_inc_ref(ctx->z3_ctx, expr);
    int res = 0;

    uint64_t constant, add_constant, sub_constant;
    optype   op;
    uint32_t add_sub_const_size;
    unsigned const_size;
    int      should_invert;

    if (!__check_if_range(ctx, expr, ig, &constant, &op, &add_constant,
                          &sub_constant, &should_invert, &add_sub_const_size,
                          &const_size))
        goto OUT;

    set__interval_group_ptr* group_intervals =
        (set__interval_group_ptr*)ctx->group_intervals;

    *wi = wi_init(const_size);

    wi_update_cmp(wi, constant, op);
    if (add_constant > 0) {
        wi_modify_size(wi, add_sub_const_size);
        wi_update_sub(wi, add_constant);
    }
    if (sub_constant > 0) {
        wi_modify_size(wi, add_sub_const_size);
        if (!should_invert)
            wi_update_add(wi, sub_constant);
        else {
            wi_update_invert(wi);
            wi_update_add(wi, sub_constant);
        }
    }
    wi_modify_size(wi, ig->n * 8);

    const wrapped_interval_t* cached_wi =
        interval_group_get_interval(group_intervals, ig);
    if (!performing_aggressive_optimistic && cached_wi != NULL)
        wi_intersect(wi, cached_wi);

    res = 1;
OUT:
    Z3_dec_ref(ctx->z3_ctx, expr);
    return res;
}

static inline int __check_univocally_defined(fuzzy_ctx_t* ctx, Z3_ast expr)
{
    Z3_ast_kind kind = Z3_get_ast_kind(ctx->z3_ctx, expr);
    if (kind != Z3_APP_AST)
        return 0;

    Z3_app       app       = Z3_to_app(ctx->z3_ctx, expr);
    Z3_func_decl decl      = Z3_get_app_decl(ctx->z3_ctx, app);
    Z3_decl_kind decl_kind = Z3_get_decl_kind(ctx->z3_ctx, decl);

    if (decl_kind != Z3_OP_EQ)
        return 0;

    Z3_ast expr_sx = Z3_get_app_arg(ctx->z3_ctx, app, 0);
    Z3_inc_ref(ctx->z3_ctx, expr_sx);
    Z3_ast expr_dx = Z3_get_app_arg(ctx->z3_ctx, app, 1);
    Z3_inc_ref(ctx->z3_ctx, expr_dx);

    ast_info_ptr inputs_sx, inputs_dx, inputs;
    detect_involved_inputs_wrapper(ctx, expr, &inputs_sx);
    detect_involved_inputs_wrapper(ctx, expr, &inputs_dx);

    Z3_dec_ref(ctx->z3_ctx, expr_sx);
    Z3_dec_ref(ctx->z3_ctx, expr_dx);

    if (inputs_sx->input_extract_ops > 0 ||
        inputs_sx->approximated_groups > 0 ||
        inputs_dx->input_extract_ops > 0 || inputs_dx->approximated_groups > 0)
        return 0; // it is not safe to add to univocally defined

    if (inputs_sx->index_groups.size == 1 && inputs_sx->index_groups.size == 0)
        inputs = inputs_sx;
    else if (inputs_sx->index_groups.size == 0 &&
             inputs_sx->index_groups.size == 1)
        inputs = inputs_dx;
    else
        return 0;

    // we have (= something f(INPUT)) in the branch condition
    // from now on, INPUT is univocally defined (from seed!)
    // never add INPUT to indexes/index_groups again
    index_group_t* ig = NULL;
    set_reset_iter__index_group_t(&inputs->index_groups, 0);
    set_iter_next__index_group_t(&inputs->index_groups, 0, &ig);

    unsigned i;
    for (i = 0; i < ig->n; ++i) {
        set_add__ulong((set__ulong*)ctx->univocally_defined_inputs,
                       ig->indexes[i]);
    }
    return 1;
}

static inline int __detect_strcmp_pattern(fuzzy_ctx_t* ctx, Z3_ast ast,
                                          unsigned long* values)
{
    /*
        (... whatever
            (concat
                #x0..0
                (ite (= inp_0 const_0) #b1 #b0)
                (ite (= inp_1 const_1) #b1 #b0)
                ...
                (ite (= inp_i const_i) #b1 #b0)))
    */
    Z3_bool     successGet;
    unsigned    i;
    Z3_ast_kind kind = Z3_get_ast_kind(ctx->z3_ctx, ast);
    if (kind != Z3_APP_AST)
        return 0;

    int          res        = 0;
    Z3_app       app        = Z3_to_app(ctx->z3_ctx, ast);
    unsigned     num_fields = Z3_get_app_num_args(ctx->z3_ctx, app);
    Z3_func_decl decl       = Z3_get_app_decl(ctx->z3_ctx, app);
    Z3_decl_kind decl_kind  = Z3_get_decl_kind(ctx->z3_ctx, decl);

    if (decl_kind == Z3_OP_CONCAT) {
        res = 1;
        for (i = 0; i < num_fields; ++i) {
            Z3_ast      child      = Z3_get_app_arg(ctx->z3_ctx, app, i);
            Z3_ast_kind child_kind = Z3_get_ast_kind(ctx->z3_ctx, child);
            if (child_kind == Z3_NUMERAL_AST)
                continue;
            if (child_kind == Z3_APP_AST) {
                Z3_app       child_app = Z3_to_app(ctx->z3_ctx, child);
                Z3_func_decl child_decl =
                    Z3_get_app_decl(ctx->z3_ctx, child_app);
                Z3_decl_kind child_decl_kind =
                    Z3_get_decl_kind(ctx->z3_ctx, child_decl);
                if (child_decl_kind != Z3_OP_ITE) {
                    res = 0;
                    break;
                }
                Z3_ast cond    = Z3_get_app_arg(ctx->z3_ctx, child_app, 0);
                Z3_ast iftrue  = Z3_get_app_arg(ctx->z3_ctx, child_app, 1);
                Z3_ast iffalse = Z3_get_app_arg(ctx->z3_ctx, child_app, 2);

                // iftrue must be #b0 or #b1
                if (Z3_get_ast_kind(ctx->z3_ctx, iftrue) != Z3_NUMERAL_AST) {
                    res = 0;
                    break;
                }
                uint64_t iftrue_v;
                successGet = Z3_get_numeral_uint64(ctx->z3_ctx, iftrue,
                                                   (uint64_t*)&iftrue_v);
                if (!successGet || (iftrue_v != 0 && iftrue_v != 1)) {
                    res = 0;
                    break;
                }
                if (Z3_get_ast_kind(ctx->z3_ctx, iffalse) != Z3_NUMERAL_AST) {
                    res = 0;
                    break;
                }

                // iffalse must be #b0 or #b1
                uint64_t iffalse_v;
                successGet = Z3_get_numeral_uint64(ctx->z3_ctx, iftrue,
                                                   (uint64_t*)&iffalse_v);
                if (!successGet || (iffalse_v != 0 && iffalse_v != 1)) {
                    res = 0;
                    break;
                }

                // cond must be (= inp_i const_i)
                if (Z3_get_ast_kind(ctx->z3_ctx, cond) != Z3_APP_AST) {
                    res = 0;
                    break;
                }
                Z3_app       cond_app  = Z3_to_app(ctx->z3_ctx, cond);
                Z3_func_decl cond_decl = Z3_get_app_decl(ctx->z3_ctx, cond_app);
                Z3_decl_kind cond_decl_kind =
                    Z3_get_decl_kind(ctx->z3_ctx, cond_decl);
                if (cond_decl_kind != Z3_OP_EQ) {
                    res = 0;
                    break;
                }
                Z3_ast inp_i   = Z3_get_app_arg(ctx->z3_ctx, cond_app, 0);
                Z3_ast const_i = Z3_get_app_arg(ctx->z3_ctx, cond_app, 1);
                if (Z3_get_ast_kind(ctx->z3_ctx, inp_i) != Z3_APP_AST) {
                    res = 0;
                    break;
                }
                if (Z3_get_ast_kind(ctx->z3_ctx, const_i) != Z3_NUMERAL_AST) {
                    res = 0;
                    break;
                }
                Z3_app       inp_i_app = Z3_to_app(ctx->z3_ctx, inp_i);
                Z3_func_decl inp_i_decl =
                    Z3_get_app_decl(ctx->z3_ctx, inp_i_app);
                Z3_decl_kind inp_i_decl_kind =
                    Z3_get_decl_kind(ctx->z3_ctx, inp_i_decl);
                if (inp_i_decl_kind != Z3_OP_UNINTERPRETED) {
                    res = 0;
                    break;
                }
                int inp_i_idx = Z3_get_symbol_int(
                    ctx->z3_ctx, Z3_get_decl_name(ctx->z3_ctx, inp_i_decl));
                uint64_t const_i_v;
                successGet = Z3_get_numeral_uint64(ctx->z3_ctx, const_i,
                                                   (uint64_t*)&const_i_v);
                if (!successGet) {
                    res = 0;
                    break;
                }

                // finally. Set value
                values[inp_i_idx] = const_i_v;
                res               = 1;
            } else {
                res = 0;
                break;
            }
        }
        if (res)
            return res;
    }

    for (i = 0; i < num_fields; ++i) {
        res |= __detect_strcmp_pattern(ctx, Z3_get_app_arg(ctx->z3_ctx, app, i),
                                       values);
    }

    return res;
}

// *************************************************
// **************** HEURISTICS - END ***************
// *************************************************

static inline void __reset_ast_data()
{
    set_remove_all__digest_t(&ast_data.processed_set, NULL);
    da_remove_all__ulong(&ast_data.values, NULL);

    ast_data.is_input_to_state      = 0;
    ast_data.inputs                 = NULL;
    ast_data.input_to_state_group.n = 0;
    ast_data.n_useless_eval         = 0;
}

static inline void __init_global_data(fuzzy_ctx_t* ctx, Z3_ast query,
                                      Z3_ast branch_condition)
{

    opt_found = 0;

    __reset_ast_data();

    __detect_input_to_state_query(ctx, branch_condition, &ast_data, 0);
    detect_involved_inputs_wrapper(ctx, branch_condition, &ast_data.inputs);
    __detect_early_constants(ctx, branch_condition, &ast_data);

    testcase_t* current_testcase = &ctx->testcases.data[0];
    memcpy(tmp_input, current_testcase->values,
           current_testcase->values_len * sizeof(unsigned long));
}

static __always_inline int PHASE_reuse(fuzzy_ctx_t* ctx, Z3_ast query,
                                       Z3_ast                branch_condition,
                                       unsigned char const** proof,
                                       unsigned long*        proof_size)
{
    if (skip_reuse)
        return 0;

    ASSERT_OR_ABORT(ctx->testcases.size > 1,
                    "PHASE_reuse not enough testcases");
#ifdef DEBUG_CHECK_LIGHT
    Z3FUZZ_LOG("Trying REUSE PHASE\n");
#endif
    unsigned i;
    for (i = 1; i < ctx->testcases.size; ++i) {
        testcase_t* testcase = &ctx->testcases.data[i];

        int eval_v = __evaluate_branch_query(
            ctx, query, branch_condition, testcase->values,
            testcase->value_sizes, testcase->values_len);
        if (eval_v == 1) {
#ifdef PRINT_SAT
            Z3FUZZ_LOG("[check light - reuse] Query is SAT\n");
#endif
            __vals_long_to_char(testcase->values, tmp_proof,
                                testcase->testcase_len);
            ctx->stats.reuse++;
            *proof      = tmp_proof;
            *proof_size = testcase->testcase_len;
            return 1;
        } else if (unlikely(eval_v == TIMEOUT_V))
            return TIMEOUT_V;
    }
    return 0;
}

static __always_inline int PHASE_input_to_state(fuzzy_ctx_t* ctx, Z3_ast query,
                                                Z3_ast branch_condition,
                                                unsigned char const** proof,
                                                unsigned long* proof_size)
{
    if (unlikely(skip_input_to_state))
        return 0;

    ASSERT_OR_ABORT(ast_data.is_input_to_state,
                    "PHASE_input_to_state not an input to state query");
#ifdef DEBUG_CHECK_LIGHT
    Z3FUZZ_LOG("Trying Input to State\n");
#endif
    testcase_t*    current_testcase = &ctx->testcases.data[0];
    index_group_t* group;
    unsigned int   index;
    unsigned char  b;
    unsigned       k;
    group = &ast_data.input_to_state_group;
    for (k = 0; k < group->n; ++k) {
        index = group->indexes[group->n - k - 1];
        b     = __extract_from_long(ast_data.input_to_state_const, k);

        if (current_testcase->values[index] == (unsigned long)b)
            continue;

#ifdef DEBUG_CHECK_LIGHT
        Z3FUZZ_LOG("L1 - inj byte: 0x%x @ %d\n", b, index);
#endif
        tmp_input[index] = b;
    }
    int valid_eval = is_valid_eval_group(ctx, group, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len);
    if (valid_eval) {
        int eval_v = __evaluate_branch_query(
            ctx, query, branch_condition, tmp_input,
            current_testcase->value_sizes, current_testcase->values_len);
        if (eval_v == 1) {
#ifdef PRINT_SAT
            Z3FUZZ_LOG("[check light - input to state] Query is SAT\n");
#endif
            ctx->stats.input_to_state++;
            ctx->stats.num_sat++;
            __vals_long_to_char(tmp_input, tmp_proof,
                                current_testcase->testcase_len);
            *proof      = tmp_proof;
            *proof_size = current_testcase->testcase_len;
            return 1;
        } else if (unlikely(eval_v == TIMEOUT_V))
            return TIMEOUT_V;
    }

    // if (ast_data.op_input_to_state == Z3_OP_EQ)
    //     // query is UNSAT
    //     return 2;

    // restore tmp_input
    for (k = 0; k < group->n; ++k) {
        index            = group->indexes[group->n - k - 1];
        tmp_input[index] = (unsigned long)current_testcase->values[index];
    }

    return 0;
}

static __always_inline int PHASE_simple_math(fuzzy_ctx_t* ctx, Z3_ast query,
                                             Z3_ast branch_condition,
                                             unsigned char const** proof,
                                             unsigned long*        proof_size)
{
    if (unlikely(skip_simple_math))
        return 0;

    index_group_t      ig = {0};
    wrapped_interval_t wi;
    if (!get_range(ctx, branch_condition, &ig, &wi))
        return 0;

#ifdef DEBUG_CHECK_LIGHT
    Z3FUZZ_LOG("Trying Simple Math\n");
#endif
    testcase_t*   current_testcase = &ctx->testcases.data[0];
    unsigned long c;
    int           i, j, k;

    if (wi_get_range(&wi) > RANGE_MAX_WIDTH_BRUTE_FORCE)
        goto TRY_MIN_MAX; // range too wide

    wrapped_interval_iter_t it = wi_init_iter_values(&wi);
    uint64_t                val;
    while (wi_iter_get_next(&it, &val)) {
        set_tmp_input_group_to_value(&ig, val);
        int eval_v = __evaluate_branch_query(
            ctx, query, branch_condition, tmp_input,
            current_testcase->value_sizes, current_testcase->values_len);
        if (eval_v == 1) {
#ifdef PRINT_SAT
            Z3FUZZ_LOG("[check light - simple math] Query is SAT\n");
#endif
            ctx->stats.simple_math++;
            ctx->stats.num_sat++;
            __vals_long_to_char(tmp_input, tmp_proof,
                                current_testcase->testcase_len);
            *proof      = tmp_proof;
            *proof_size = current_testcase->testcase_len;
            return 1;
        } else if (unlikely(eval_v == TIMEOUT_V))
            return TIMEOUT_V;
    }
    return 2;

TRY_MIN_MAX:
    for (j = 0; j < 2; ++j) {
        if (j == 0)
            c = wi.min;
        else
            c = wi.max;

        for (k = 0; k < ig.n; ++k) {
            unsigned int  index = ig.indexes[ig.n - k - 1];
            unsigned char b     = __extract_from_long(c, k);

#ifdef DEBUG_CHECK_LIGHT
            Z3FUZZ_LOG("SM - inj byte: 0x%x @ %d\n", b, index);
#endif
            if (current_testcase->values[index] == (unsigned long)b)
                continue;

            tmp_input[index] = b;
        }
        int valid_eval = is_valid_eval_group(ctx, &ig, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - simple math] Query "
                           "is SAT\n");
#endif
                ctx->stats.simple_math++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->values_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
    }
    for (k = 0; k < ig.n; ++k) {
        i            = ig.indexes[ig.n - k - 1];
        tmp_input[i] = current_testcase->values[i];
    }
    return 0;
}

static __always_inline int PHASE_input_to_state_extended(
    fuzzy_ctx_t* ctx, Z3_ast query, Z3_ast branch_condition,
    unsigned char const** proof, unsigned long* proof_size)
{
    if (unlikely(skip_input_to_state_extended))
        return 0;

    ASSERT_OR_ABORT(ast_data.values.size > 0 ||
                        ast_data.inputs->inp_to_state_ite.size > 0,
                    "PHASE_input_to_state_extended  no early constants");

#ifdef DEBUG_CHECK_LIGHT
    Z3FUZZ_LOG("Trying Input to State Extended\n");
#endif
    testcase_t* current_testcase = &ctx->testcases.data[0];

    index_group_t* group;
    unsigned int   index;
    unsigned       i;
    unsigned       k;

    for (i = 0; i < ast_data.values.size; ++i) {
        set_reset_iter__index_group_t(&ast_data.inputs->index_groups, 0);
        while (set_iter_next__index_group_t(&ast_data.inputs->index_groups, 0,
                                            &group)) {
            // little endian
            for (k = 0; k < group->n; ++k) {
                unsigned int  index = group->indexes[group->n - k - 1];
                unsigned char b =
                    __extract_from_long(ast_data.values.data[i], k);

#ifdef DEBUG_CHECK_LIGHT
                Z3FUZZ_LOG("L2 - inj byte: 0x%x @ %d\n", b, index);
#endif
                if (tmp_input[index] == (unsigned long)b)
                    continue;

                tmp_input[index] = b;
            }
            int valid_eval = is_valid_eval_group(ctx, group, tmp_input,
                                                 current_testcase->value_sizes,
                                                 current_testcase->values_len);
            if (valid_eval) {
                int eval_v = __evaluate_branch_query(
                    ctx, query, branch_condition, tmp_input,
                    current_testcase->value_sizes,
                    current_testcase->values_len);
                if (eval_v == 1) {
#ifdef PRINT_SAT
                    Z3FUZZ_LOG("[check light - input to state extended] Query "
                               "is SAT\n");
#endif
                    ctx->stats.input_to_state_ext++;
                    ctx->stats.num_sat++;
                    __vals_long_to_char(tmp_input, tmp_proof,
                                        current_testcase->testcase_len);
                    *proof      = tmp_proof;
                    *proof_size = current_testcase->values_len;
                    return 1;
                } else if (unlikely(eval_v == TIMEOUT_V))
                    return TIMEOUT_V;
            }
            // big endian
            for (k = 0; k < group->n; ++k) {
                unsigned int  index = group->indexes[k];
                unsigned char b =
                    __extract_from_long(ast_data.values.data[i], k);

#ifdef DEBUG_CHECK_LIGHT
                Z3FUZZ_LOG("L2 - inj byte: 0x%x @ %d\n", b, index);
#endif
                if (tmp_input[index] == (unsigned long)b)
                    continue;

                tmp_input[index] = b;
            }
            valid_eval = is_valid_eval_group(ctx, group, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len);
            if (valid_eval) {
                int eval_v = __evaluate_branch_query(
                    ctx, query, branch_condition, tmp_input,
                    current_testcase->value_sizes,
                    current_testcase->values_len);
                if (eval_v == 1) {
#ifdef PRINT_SAT
                    Z3FUZZ_LOG("[check light - input to state extended] Query "
                               "is SAT\n");
#endif
                    ctx->stats.input_to_state_ext++;
                    ctx->stats.num_sat++;
                    __vals_long_to_char(tmp_input, tmp_proof,
                                        current_testcase->testcase_len);
                    *proof      = tmp_proof;
                    *proof_size = current_testcase->values_len;
                    return 1;
                } else if (unlikely(eval_v == TIMEOUT_V))
                    return TIMEOUT_V;
            }
            // restore tmp_input
            for (k = 0; k < group->n; ++k) {
                index            = group->indexes[k];
                tmp_input[index] = current_testcase->values[index];
            }
        }
    }

    // ite constants
    for (i = 0; i < ast_data.inputs->inp_to_state_ite.size; ++i) {
        ite_its_t* its_el = &ast_data.inputs->inp_to_state_ite.data[i];
        set_tmp_input_group_to_value(&its_el->ig, its_el->val);
    }
    int eval_v = __evaluate_branch_query(
        ctx, query, branch_condition, tmp_input, current_testcase->value_sizes,
        current_testcase->values_len);
    if (eval_v == 1) {
#ifdef PRINT_SAT
        Z3FUZZ_LOG("[check light - input to state extended] Query "
                   "is SAT\n");
#endif
        ctx->stats.input_to_state_ext++;
        ctx->stats.num_sat++;
        __vals_long_to_char(tmp_input, tmp_proof,
                            current_testcase->testcase_len);
        *proof      = tmp_proof;
        *proof_size = current_testcase->values_len;
        return 1;
    } else if (unlikely(eval_v == TIMEOUT_V))
        return TIMEOUT_V;

    // restore tmp_input
    for (i = 0; i < ast_data.inputs->inp_to_state_ite.size; ++i) {
        ite_its_t* its_el = &ast_data.inputs->inp_to_state_ite.data[i];
        for (k = 0; k < its_el->ig.n; ++k) {
            index            = its_el->ig.indexes[k];
            tmp_input[index] = current_testcase->values[index];
        }
    }
    return 0;
}

static __always_inline int PHASE_brute_force(fuzzy_ctx_t* ctx, Z3_ast query,
                                             Z3_ast branch_condition,
                                             unsigned char const** proof,
                                             unsigned long*        proof_size)
{
    if (unlikely(skip_brute_force))
        return 2;

    testcase_t*    current_testcase = &ctx->testcases.data[0];
    unsigned       i;
    unsigned long* uniq_index;

#ifdef DEBUG_CHECK_LIGHT
    Z3FUZZ_LOG("Trying Brute Force\n");
#endif

    uniq_index = NULL;
    set_reset_iter__ulong(&ast_data.inputs->indexes, 0);
    set_iter_next__ulong(&ast_data.inputs->indexes, 0, &uniq_index);

    for (i = 0; i < 256; ++i) {
        tmp_input[*uniq_index] = i;
        int eval_v             = __evaluate_branch_query(
            ctx, query, branch_condition, tmp_input,
            current_testcase->value_sizes, current_testcase->values_len);
        if (eval_v == 1) {
#ifdef PRINT_SAT
            Z3FUZZ_LOG("[check light - brute force] "
                       "Query is SAT\n");
#endif
            ctx->stats.brute_force++;
            ctx->stats.num_sat++;
            __vals_long_to_char(tmp_input, tmp_proof,
                                current_testcase->testcase_len);
            *proof      = tmp_proof;
            *proof_size = current_testcase->testcase_len;
            return 1;
        } else if (unlikely(eval_v == TIMEOUT_V))
            return TIMEOUT_V;
    }
    // if we are here, the query is UNSAT
    return 0;
}

static __always_inline int
PHASE_gradient_descend(fuzzy_ctx_t* ctx, Z3_ast query, Z3_ast branch_condition,
                       unsigned char const** proof, unsigned long* proof_size)
{
    if (unlikely(skip_gradient_descend))
        return 0;

    testcase_t* current_testcase = &ctx->testcases.data[0];

#ifdef DEBUG_CHECK_LIGHT
    Z3FUZZ_LOG("Trying Gradient Descend\n");
#endif

    Z3_ast out_ast;
    int valid_for_gd = __gradient_transf_init(ctx, branch_condition, &out_ast);
    if (!valid_for_gd)
        return 0;

    int               res = 0;
    eval_wapper_ctx_t ew;

    int valid_eval = __gd_init_eval(ctx, query, out_ast, 0, 0, &ew);
    ASSERT_OR_ABORT(valid_eval == 1, "eval should be always valid here");

    eval_set_ctx(&ew);
    set__digest_t digest_set;
    set_init__digest_t(&digest_set, digest_64bit_hash, digest_equals);

    int      gd_ret;
    uint64_t val;
    while (
        ((gd_ret = gd_descend_transf(__gd_eval, ew.input, ew.input, &val,
                                     ew.mapping_size)) == 0) &&
        (__check_or_add_digest(&digest_set, (unsigned char*)ew.input,
                               ew.mapping_size * sizeof(unsigned long)) == 0)) {
        __gd_fix_tmp_input(ew.input);
        int eval_v = __evaluate_branch_query(
            ctx, query, branch_condition, tmp_input,
            current_testcase->value_sizes, current_testcase->values_len);
        if (eval_v == 1) {
#ifdef PRINT_SAT
            Z3FUZZ_LOG("[check light - gradient descend] "
                       "Query is SAT\n");
#endif
            ctx->stats.gradient_descend++;
            ctx->stats.num_sat++;
            __vals_long_to_char(tmp_input, tmp_proof,
                                current_testcase->testcase_len);
            *proof      = tmp_proof;
            *proof_size = current_testcase->testcase_len;
            res         = 1;
            goto OUT;
        } else if (unlikely(eval_v == TIMEOUT_V)) {
            res = TIMEOUT_V;
            goto OUT;
        }
    }
    if (unlikely(gd_ret == TIMEOUT_V)) {
        res = TIMEOUT_V;
        goto OUT;
    }

    __gd_restore_tmp_input(current_testcase);
OUT:
    Z3_dec_ref(ctx->z3_ctx, out_ast);
    set_free__digest_t(&digest_set, NULL);
    __gd_free_eval(&ew);
    return res;
}

static __always_inline int SUBPHASE_afl_det_single_waliking_bit(
    fuzzy_ctx_t* ctx, Z3_ast query, Z3_ast branch_condition,
    unsigned char const** proof, unsigned long* proof_size,
    unsigned long input_index)
{
    if (unlikely(skip_afl_det_single_walking_bit))
        return 0;

    testcase_t*   current_testcase = &ctx->testcases.data[0];
    unsigned char input_byte_0 =
        (unsigned char)current_testcase->values[input_index];
    unsigned char tmp_byte;
    unsigned      i;

    // single walking bit
    for (i = 0; i < 8; ++i) {
        tmp_byte               = FLIP_BIT(input_byte_0, i);
        tmp_input[input_index] = (unsigned long)tmp_byte;
        int valid_eval = is_valid_eval_index(ctx, input_index, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - flip1] "
                           "Query is SAT\n");
#endif
                ctx->stats.flip1++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
    }
    return 0;
}

static __always_inline int SUBPHASE_afl_det_two_waliking_bits(
    fuzzy_ctx_t* ctx, Z3_ast query, Z3_ast branch_condition,
    unsigned char const** proof, unsigned long* proof_size,
    unsigned long input_index)
{
    if (unlikely(skip_afl_det_two_walking_bit))
        return 0;

    testcase_t*   current_testcase = &ctx->testcases.data[0];
    unsigned char input_byte_0 =
        (unsigned char)current_testcase->values[input_index];
    unsigned char tmp_byte;
    unsigned      i;
    for (i = 0; i < 7; ++i) {
        tmp_byte               = FLIP_BIT(input_byte_0, i);
        tmp_byte               = FLIP_BIT(tmp_byte, i + 1);
        tmp_input[input_index] = (unsigned long)tmp_byte;
        int valid_eval = is_valid_eval_index(ctx, input_index, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len);
        if (valid_eval) {

            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - flip2] "
                           "Query is SAT\n");
#endif
                ctx->stats.flip2++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
    }
    return 0;
}

static __always_inline int SUBPHASE_afl_det_four_waliking_bits(
    fuzzy_ctx_t* ctx, Z3_ast query, Z3_ast branch_condition,
    unsigned char const** proof, unsigned long* proof_size,
    unsigned long input_index)
{
    if (unlikely(skip_afl_det_four_walking_bit))
        return 0;

    testcase_t*   current_testcase = &ctx->testcases.data[0];
    unsigned char input_byte_0 =
        (unsigned char)current_testcase->values[input_index];
    unsigned char tmp_byte;
    unsigned      i;

    for (i = 0; i < 5; ++i) {
        tmp_byte               = FLIP_BIT(input_byte_0, i);
        tmp_byte               = FLIP_BIT(tmp_byte, i + 1);
        tmp_byte               = FLIP_BIT(tmp_byte, i + 2);
        tmp_byte               = FLIP_BIT(tmp_byte, i + 3);
        tmp_input[input_index] = (unsigned long)tmp_byte;
        int valid_eval = is_valid_eval_index(ctx, input_index, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - flip4] "
                           "Query is SAT\n");
#endif
                ctx->stats.flip4++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
    }
    return 0;
}

static __always_inline int
SUBPHASE_afl_det_byte_flip(fuzzy_ctx_t* ctx, Z3_ast query,
                           Z3_ast branch_condition, unsigned char const** proof,
                           unsigned long* proof_size, unsigned long input_index)
{
    if (unlikely(skip_afl_det_byte_flip))
        return 0;

    testcase_t*   current_testcase = &ctx->testcases.data[0];
    unsigned char input_byte_0 =
        (unsigned char)current_testcase->values[input_index];

    tmp_input[input_index] = (unsigned long)input_byte_0 ^ 0xffUL;
    int valid_eval         = is_valid_eval_index(ctx, input_index, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len);
    if (valid_eval) {
        int eval_v = __evaluate_branch_query(
            ctx, query, branch_condition, tmp_input,
            current_testcase->value_sizes, current_testcase->values_len);
        if (eval_v == 1) {
#ifdef PRINT_SAT
            Z3FUZZ_LOG("[check light - flip8] "
                       "Query is SAT\n");
#endif
            ctx->stats.flip8++;
            ctx->stats.num_sat++;
            __vals_long_to_char(tmp_input, tmp_proof,
                                current_testcase->testcase_len);
            *proof      = tmp_proof;
            *proof_size = current_testcase->testcase_len;
            return 1;
        } else if (unlikely(eval_v == TIMEOUT_V))
            return TIMEOUT_V;
    }
    return 0;
}

static __always_inline int
SUBPHASE_afl_det_arith8(fuzzy_ctx_t* ctx, Z3_ast query, Z3_ast branch_condition,
                        unsigned char const** proof, unsigned long* proof_size,
                        unsigned long input_index)
{
    if (unlikely(skip_afl_det_arith8))
        return 0;

    testcase_t*   current_testcase = &ctx->testcases.data[0];
    unsigned char input_byte_0 =
        (unsigned char)current_testcase->values[input_index];
    unsigned i;

    for (i = 1; i < 35; ++i) {
        tmp_input[input_index] = (unsigned char)(input_byte_0 + i);
        int valid_eval = is_valid_eval_index(ctx, input_index, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - arith8-sum] "
                           "Query is SAT\n");
#endif
                ctx->stats.arith8_sum++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
        tmp_input[input_index] = (unsigned char)(input_byte_0 - i);
        valid_eval = is_valid_eval_index(ctx, input_index, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len);
        if (valid_eval) {

            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - arith8-sub] "
                           "Query is SAT\n");
#endif
                ctx->stats.arith8_sub++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
    }
    return 0;
}

static __always_inline int SUBPHASE_afl_det_int8(fuzzy_ctx_t* ctx, Z3_ast query,
                                                 Z3_ast branch_condition,
                                                 unsigned char const** proof,
                                                 unsigned long* proof_size,
                                                 unsigned long  input_index)
{
    if (unlikely(skip_afl_det_int8))
        return 0;

    testcase_t* current_testcase = &ctx->testcases.data[0];
    unsigned    i;

    for (i = 0; i < sizeof(interesting8); ++i) {
        tmp_input[input_index] = (unsigned char)(interesting8[i]);
        int valid_eval = is_valid_eval_index(ctx, input_index, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - int8] "
                           "Query is SAT\n");
#endif
                ctx->stats.int8++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
    }
    return 0;
}

static __always_inline int SUBPHASE_afl_det_flip_short(
    fuzzy_ctx_t* ctx, Z3_ast query, Z3_ast branch_condition,
    unsigned char const** proof, unsigned long* proof_size,
    unsigned long input_index_0, unsigned long input_index_1)
{
    if (unlikely(skip_afl_det_flip_short))
        return 0;

    testcase_t*   current_testcase = &ctx->testcases.data[0];
    unsigned char input_byte_0 =
        (unsigned char)current_testcase->values[input_index_0];
    unsigned char input_byte_1 =
        (unsigned char)current_testcase->values[input_index_1];

    // flip short
    tmp_input[input_index_0] = (unsigned long)input_byte_0 ^ 0xffUL;
    tmp_input[input_index_1] = (unsigned long)input_byte_1 ^ 0xffUL;
    int valid_eval = is_valid_eval_index(ctx, input_index_0, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_1, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len);
    if (valid_eval) {
        int eval_v = __evaluate_branch_query(
            ctx, query, branch_condition, tmp_input,
            current_testcase->value_sizes, current_testcase->values_len);
        if (eval_v == 1) {
#ifdef PRINT_SAT
            Z3FUZZ_LOG("[check light - flip16] "
                       "Query is SAT\n");
#endif
            ctx->stats.flip16++;
            ctx->stats.num_sat++;
            __vals_long_to_char(tmp_input, tmp_proof,
                                current_testcase->testcase_len);
            *proof      = tmp_proof;
            *proof_size = current_testcase->testcase_len;
            return 1;
        } else if (unlikely(eval_v == TIMEOUT_V))
            return TIMEOUT_V;
    }
    return 0;
}

static __always_inline int
SUBPHASE_afl_det_arith16(fuzzy_ctx_t* ctx, Z3_ast query,
                         Z3_ast branch_condition, unsigned char const** proof,
                         unsigned long* proof_size, unsigned long input_index_0,
                         unsigned long input_index_1)
{
    if (unlikely(skip_afl_det_arith16))
        return 0;

    testcase_t*   current_testcase = &ctx->testcases.data[0];
    unsigned char input_byte_0 =
        (unsigned char)current_testcase->values[input_index_0];
    unsigned char input_byte_1 =
        (unsigned char)current_testcase->values[input_index_1];
    unsigned short input_word_LE = (input_byte_1 << 8) | input_byte_0;
    unsigned short input_word_BE = (input_byte_0 << 8) | input_byte_1;

    unsigned short tmp;
    unsigned       i;
    for (i = 1; i < 35; ++i) {
        tmp                      = input_word_LE + i;
        tmp_input[input_index_0] = (unsigned long)(tmp & 0xffUL);
        tmp_input[input_index_1] = (unsigned long)((tmp >> 8) & 0xffUL);

        int valid_eval = is_valid_eval_index(ctx, input_index_0, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_1, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - arith16-sum-LE] "
                           "Query is SAT\n");
#endif
                ctx->stats.arith16_sum_LE++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
        tmp                      = input_word_LE - i;
        tmp_input[input_index_0] = (unsigned long)(tmp & 0xffUL);
        tmp_input[input_index_1] = (unsigned long)((tmp >> 8) & 0xffUL);
        valid_eval = is_valid_eval_index(ctx, input_index_0, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_1, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - arith16-sub-LE] "
                           "Query is SAT\n");
#endif
                ctx->stats.arith16_sub_LE++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
        tmp                      = input_word_BE + i;
        tmp_input[input_index_1] = (unsigned long)(tmp & 0xffUL);
        tmp_input[input_index_0] = (unsigned long)((tmp >> 8) & 0xffUL);
        valid_eval = is_valid_eval_index(ctx, input_index_0, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_1, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - arith16-sum-BE] "
                           "Query is SAT\n");
#endif
                ctx->stats.arith16_sum_BE++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }

        tmp                      = input_word_BE - i;
        tmp_input[input_index_1] = (unsigned long)(tmp & 0xffUL);
        tmp_input[input_index_0] = (unsigned long)((tmp >> 8) & 0xffUL);
        valid_eval = is_valid_eval_index(ctx, input_index_0, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_1, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - arith16-sub-BE] "
                           "Query is SAT\n");
#endif
                ctx->stats.arith32_sub_BE++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
    }
    return 0;
}

static __always_inline int
SUBPHASE_afl_det_int16(fuzzy_ctx_t* ctx, Z3_ast query, Z3_ast branch_condition,
                       unsigned char const** proof, unsigned long* proof_size,
                       unsigned long input_index_0, unsigned long input_index_1)
{
    if (unlikely(skip_afl_det_int16))
        return 0;
    testcase_t* current_testcase = &ctx->testcases.data[0];

    unsigned i;
    for (i = 0; i < sizeof(interesting16) / sizeof(short); ++i) {
        tmp_input[input_index_0] = (unsigned long)(interesting16[i]) & 0xffUL;
        tmp_input[input_index_1] =
            (unsigned long)(interesting16[i] >> 8) & 0xffUL;
        int valid_eval = is_valid_eval_index(ctx, input_index_0, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_1, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - int16] "
                           "Query is SAT\n");
#endif
                ctx->stats.int16++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
#if 0
        tmp_input[input_index_1] = (unsigned long)(interesting16[i]) & 0xffUL;
        tmp_input[input_index_0] =
            (unsigned long)(interesting16[i] >> 8) & 0xffUL;
        valid_eval = is_valid_eval_index(ctx, input_index_0, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_1, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - int16] "
                           "Query is SAT\n");
#endif
                ctx->stats.int16++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
#endif
    }
    return 0;
}

static __always_inline int SUBPHASE_afl_det_flip_int(
    fuzzy_ctx_t* ctx, Z3_ast query, Z3_ast branch_condition,
    unsigned char const** proof, unsigned long* proof_size,
    unsigned long input_index_0, unsigned long input_index_1,
    unsigned long input_index_2, unsigned long input_index_3)
{
    if (unlikely(skip_afl_det_flip_int))
        return 0;

    testcase_t* current_testcase = &ctx->testcases.data[0];

    unsigned char input_byte_0 =
        (unsigned char)current_testcase->values[input_index_0];
    unsigned char input_byte_1 =
        (unsigned char)current_testcase->values[input_index_1];
    unsigned char input_byte_2 =
        (unsigned char)current_testcase->values[input_index_2];
    unsigned char input_byte_3 =
        (unsigned char)current_testcase->values[input_index_3];

    tmp_input[input_index_0] = (unsigned long)input_byte_0 ^ 0xffUL;
    tmp_input[input_index_1] = (unsigned long)input_byte_1 ^ 0xffUL;
    tmp_input[input_index_2] = (unsigned long)input_byte_2 ^ 0xffUL;
    tmp_input[input_index_3] = (unsigned long)input_byte_3 ^ 0xffUL;
    int valid_eval = is_valid_eval_index(ctx, input_index_0, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_1, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_2, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_3, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len);
    if (valid_eval) {
        int eval_v = __evaluate_branch_query(
            ctx, query, branch_condition, tmp_input,
            current_testcase->value_sizes, current_testcase->values_len);
        if (eval_v == 1) {
#ifdef PRINT_SAT
            Z3FUZZ_LOG("[check light - flip32] "
                       "Query is SAT\n");
#endif
            ctx->stats.flip32++;
            ctx->stats.num_sat++;
            __vals_long_to_char(tmp_input, tmp_proof,
                                current_testcase->testcase_len);
            *proof      = tmp_proof;
            *proof_size = current_testcase->testcase_len;
            return 1;
        } else if (unlikely(eval_v == TIMEOUT_V))
            return TIMEOUT_V;
    }
    return 0;
}

static __always_inline int SUBPHASE_afl_det_arith32(
    fuzzy_ctx_t* ctx, Z3_ast query, Z3_ast branch_condition,
    unsigned char const** proof, unsigned long* proof_size,
    unsigned long input_index_0, unsigned long input_index_1,
    unsigned long input_index_2, unsigned long input_index_3)
{
    if (unlikely(skip_afl_det_arith32))
        return 0;

    testcase_t* current_testcase = &ctx->testcases.data[0];

    unsigned char input_byte_0 =
        (unsigned char)current_testcase->values[input_index_0];
    unsigned char input_byte_1 =
        (unsigned char)current_testcase->values[input_index_1];
    unsigned char input_byte_2 =
        (unsigned char)current_testcase->values[input_index_2];
    unsigned char input_byte_3 =
        (unsigned char)current_testcase->values[input_index_3];
    unsigned input_dword_LE = (input_byte_3 << 24) | (input_byte_2 << 16) |
                              (input_byte_1 << 8) | input_byte_0;
    unsigned input_dword_BE = (input_byte_0 << 24) | (input_byte_1 << 16) |
                              (input_byte_2 << 8) | input_byte_3;

    unsigned i, tmp;
    for (i = 1; i < 35; ++i) {
        tmp                      = input_dword_LE + i;
        tmp_input[input_index_0] = (unsigned long)(tmp & 0xffUL);
        tmp_input[input_index_1] = (unsigned long)((tmp >> 8) & 0xffUL);
        tmp_input[input_index_2] = (unsigned long)((tmp >> 16) & 0xffUL);
        tmp_input[input_index_3] = (unsigned long)((tmp >> 24) & 0xffUL);
        int valid_eval = is_valid_eval_index(ctx, input_index_0, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_1, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_2, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_3, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - arith32-sum-LE] "
                           "Query is SAT\n");
#endif
                ctx->stats.arith32_sum_LE++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }

        tmp                      = input_dword_LE - i;
        tmp_input[input_index_0] = (unsigned long)(tmp & 0xffUL);
        tmp_input[input_index_1] = (unsigned long)((tmp >> 8) & 0xffUL);
        tmp_input[input_index_2] = (unsigned long)((tmp >> 16) & 0xffUL);
        tmp_input[input_index_3] = (unsigned long)((tmp >> 24) & 0xffUL);
        valid_eval = is_valid_eval_index(ctx, input_index_0, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_1, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_2, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_3, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - arith32-sub-LE] "
                           "Query is SAT\n");
#endif
                ctx->stats.arith32_sub_LE++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }

        tmp                      = input_dword_BE + i;
        tmp_input[input_index_3] = (unsigned long)(tmp & 0xffUL);
        tmp_input[input_index_2] = (unsigned long)((tmp >> 8) & 0xffUL);
        tmp_input[input_index_1] = (unsigned long)((tmp >> 16) & 0xffUL);
        tmp_input[input_index_0] = (unsigned long)((tmp >> 24) & 0xffUL);
        valid_eval = is_valid_eval_index(ctx, input_index_0, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_1, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_2, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_3, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - arith32-sum-BE] "
                           "Query is SAT\n");
#endif
                ctx->stats.arith32_sum_BE++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
        tmp                      = input_dword_BE - i;
        tmp_input[input_index_3] = (unsigned long)(tmp & 0xffU);
        tmp_input[input_index_2] = (unsigned long)((tmp >> 8) & 0xffU);
        tmp_input[input_index_1] = (unsigned long)((tmp >> 16) & 0xffU);
        tmp_input[input_index_0] = (unsigned long)((tmp >> 24) & 0xffU);
        valid_eval = is_valid_eval_index(ctx, input_index_0, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_1, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_2, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_3, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - arith32-sub-BE] "
                           "Query is SAT\n");
#endif
                ctx->stats.arith32_sub_BE++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
    }
    return 0;
}

static __always_inline int
SUBPHASE_afl_det_int32(fuzzy_ctx_t* ctx, Z3_ast query, Z3_ast branch_condition,
                       unsigned char const** proof, unsigned long* proof_size,
                       unsigned long input_index_0, unsigned long input_index_1,
                       unsigned long input_index_2, unsigned long input_index_3)
{
    if (unlikely(skip_afl_det_int32))
        return 0;

    testcase_t* current_testcase = &ctx->testcases.data[0];

    unsigned i;
    for (i = 0; i < sizeof(interesting32) / sizeof(int); ++i) {
        tmp_input[input_index_0] = (unsigned long)(interesting32[i]) & 0xffU;
        tmp_input[input_index_1] =
            (unsigned long)(interesting32[i] >> 8) & 0xffU;
        tmp_input[input_index_2] =
            (unsigned long)(interesting32[i] >> 16) & 0xffU;
        tmp_input[input_index_3] =
            (unsigned long)(interesting32[i] >> 24) & 0xffU;
        int valid_eval = is_valid_eval_index(ctx, input_index_0, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_1, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_2, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_3, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - int32] "
                           "Query is SAT\n");
#endif
                ctx->stats.int32++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            }
        }

#if 1
        tmp_input[input_index_3] = (unsigned long)(interesting32[i]) & 0xffU;
        tmp_input[input_index_2] =
            (unsigned long)(interesting32[i] >> 8) & 0xffU;
        tmp_input[input_index_1] =
            (unsigned long)(interesting32[i] >> 16) & 0xffU;
        tmp_input[input_index_0] =
            (unsigned long)(interesting32[i] >> 24) & 0xffU;
        valid_eval = is_valid_eval_index(ctx, input_index_0, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_1, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_2, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_3, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - int32] "
                           "Query is SAT\n");
#endif
                ctx->stats.int32++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
#endif
    }
    return 0;
}

static __always_inline int SUBPHASE_afl_det_flip_long(
    fuzzy_ctx_t* ctx, Z3_ast query, Z3_ast branch_condition,
    unsigned char const** proof, unsigned long* proof_size,
    unsigned long input_index_0, unsigned long input_index_1,
    unsigned long input_index_2, unsigned long input_index_3,
    unsigned long input_index_4, unsigned long input_index_5,
    unsigned long input_index_6, unsigned long input_index_7)
{
    if (unlikely(skip_afl_det_flip_long))
        return 0;

    testcase_t* current_testcase = &ctx->testcases.data[0];

    unsigned char input_byte_0 =
        (unsigned char)current_testcase->values[input_index_0];
    unsigned char input_byte_1 =
        (unsigned char)current_testcase->values[input_index_1];
    unsigned char input_byte_2 =
        (unsigned char)current_testcase->values[input_index_2];
    unsigned char input_byte_3 =
        (unsigned char)current_testcase->values[input_index_3];
    unsigned char input_byte_4 =
        (unsigned char)current_testcase->values[input_index_4];
    unsigned char input_byte_5 =
        (unsigned char)current_testcase->values[input_index_5];
    unsigned char input_byte_6 =
        (unsigned char)current_testcase->values[input_index_6];
    unsigned char input_byte_7 =
        (unsigned char)current_testcase->values[input_index_7];

    tmp_input[input_index_0] = (unsigned long)input_byte_0 ^ 0xffUL;
    tmp_input[input_index_1] = (unsigned long)input_byte_1 ^ 0xffUL;
    tmp_input[input_index_2] = (unsigned long)input_byte_2 ^ 0xffUL;
    tmp_input[input_index_3] = (unsigned long)input_byte_3 ^ 0xffUL;
    tmp_input[input_index_4] = (unsigned long)input_byte_4 ^ 0xffUL;
    tmp_input[input_index_5] = (unsigned long)input_byte_5 ^ 0xffUL;
    tmp_input[input_index_6] = (unsigned long)input_byte_6 ^ 0xffUL;
    tmp_input[input_index_7] = (unsigned long)input_byte_7 ^ 0xffUL;
    int valid_eval = is_valid_eval_index(ctx, input_index_0, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_1, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_2, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_3, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_4, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_5, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_6, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_7, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len);
    if (valid_eval) {
        int eval_v = __evaluate_branch_query(
            ctx, query, branch_condition, tmp_input,
            current_testcase->value_sizes, current_testcase->values_len);
        if (eval_v == 1) {
#ifdef PRINT_SAT
            Z3FUZZ_LOG("[check light - flip64] "
                       "Query is SAT\n");
#endif
            ctx->stats.flip64++;
            ctx->stats.num_sat++;
            __vals_long_to_char(tmp_input, tmp_proof,
                                current_testcase->testcase_len);
            *proof      = tmp_proof;
            *proof_size = current_testcase->testcase_len;
            return 1;
        } else if (unlikely(eval_v == TIMEOUT_V))
            return TIMEOUT_V;
    }
    return 0;
}

static __always_inline int SUBPHASE_afl_det_arith64(
    fuzzy_ctx_t* ctx, Z3_ast query, Z3_ast branch_condition,
    unsigned char const** proof, unsigned long* proof_size,
    unsigned long input_index_0, unsigned long input_index_1,
    unsigned long input_index_2, unsigned long input_index_3,
    unsigned long input_index_4, unsigned long input_index_5,
    unsigned long input_index_6, unsigned long input_index_7)
{
    if (unlikely(skip_afl_det_arith64))
        return 0;

    testcase_t* current_testcase = &ctx->testcases.data[0];

    unsigned char input_byte_0 =
        (unsigned char)current_testcase->values[input_index_0];
    unsigned char input_byte_1 =
        (unsigned char)current_testcase->values[input_index_1];
    unsigned char input_byte_2 =
        (unsigned char)current_testcase->values[input_index_2];
    unsigned char input_byte_3 =
        (unsigned char)current_testcase->values[input_index_3];
    unsigned char input_byte_4 =
        (unsigned char)current_testcase->values[input_index_4];
    unsigned char input_byte_5 =
        (unsigned char)current_testcase->values[input_index_5];
    unsigned char input_byte_6 =
        (unsigned char)current_testcase->values[input_index_6];
    unsigned char input_byte_7 =
        (unsigned char)current_testcase->values[input_index_7];

    unsigned long input_qword_LE =
        ((ulong)input_byte_7 << 56) | ((ulong)input_byte_6 << 48) |
        ((ulong)input_byte_5 << 40) | ((ulong)input_byte_4 << 32) |
        ((ulong)input_byte_3 << 24) | ((ulong)input_byte_2 << 16) |
        ((ulong)input_byte_1 << 8) | (ulong)input_byte_0;
    unsigned long input_qword_BE =
        ((ulong)input_byte_0 << 56) | ((ulong)input_byte_1 << 48) |
        ((ulong)input_byte_2 << 40) | ((ulong)input_byte_3 << 32) |
        ((ulong)input_byte_4 << 24) | ((ulong)input_byte_5 << 16) |
        ((ulong)input_byte_6 << 8) | (ulong)input_byte_7;

    unsigned long tmp;
    unsigned      i;
    for (i = 1; i < 35; ++i) {
        tmp                      = input_qword_LE + i;
        tmp_input[input_index_0] = (unsigned long)(tmp & 0xffUL);
        tmp_input[input_index_1] = (unsigned long)((tmp >> 8) & 0xffUL);
        tmp_input[input_index_2] = (unsigned long)((tmp >> 16) & 0xffUL);
        tmp_input[input_index_3] = (unsigned long)((tmp >> 24) & 0xffUL);
        tmp_input[input_index_4] = (unsigned long)((tmp >> 32) & 0xffUL);
        tmp_input[input_index_5] = (unsigned long)((tmp >> 40) & 0xffUL);
        tmp_input[input_index_6] = (unsigned long)((tmp >> 48) & 0xffUL);
        tmp_input[input_index_7] = (unsigned long)((tmp >> 56) & 0xffUL);
        int valid_eval = is_valid_eval_index(ctx, input_index_0, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_1, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_2, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_3, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_4, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_5, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_6, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_7, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - arith64-sum-LE] "
                           "Query is SAT\n");
#endif
                ctx->stats.arith64_sum_LE++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
        tmp                      = input_qword_LE - i;
        tmp_input[input_index_0] = (unsigned long)(tmp & 0xffUL);
        tmp_input[input_index_1] = (unsigned long)((tmp >> 8) & 0xffUL);
        tmp_input[input_index_2] = (unsigned long)((tmp >> 16) & 0xffUL);
        tmp_input[input_index_3] = (unsigned long)((tmp >> 24) & 0xffUL);
        tmp_input[input_index_4] = (unsigned long)((tmp >> 32) & 0xffUL);
        tmp_input[input_index_5] = (unsigned long)((tmp >> 40) & 0xffUL);
        tmp_input[input_index_6] = (unsigned long)((tmp >> 48) & 0xffUL);
        tmp_input[input_index_7] = (unsigned long)((tmp >> 56) & 0xffUL);
        valid_eval = is_valid_eval_index(ctx, input_index_0, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_1, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_2, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_3, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_4, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_5, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_6, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_7, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - arith64-sub-LE] "
                           "Query is SAT\n");
#endif
                ctx->stats.arith64_sub_LE++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
        tmp                      = input_qword_BE + i;
        tmp_input[input_index_7] = (unsigned long)(tmp & 0xffUL);
        tmp_input[input_index_6] = (unsigned long)((tmp >> 8) & 0xffUL);
        tmp_input[input_index_5] = (unsigned long)((tmp >> 16) & 0xffUL);
        tmp_input[input_index_4] = (unsigned long)((tmp >> 24) & 0xffUL);
        tmp_input[input_index_3] = (unsigned long)((tmp >> 32) & 0xffUL);
        tmp_input[input_index_2] = (unsigned long)((tmp >> 40) & 0xffUL);
        tmp_input[input_index_1] = (unsigned long)((tmp >> 48) & 0xffUL);
        tmp_input[input_index_0] = (unsigned long)((tmp >> 56) & 0xffUL);
        valid_eval = is_valid_eval_index(ctx, input_index_0, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_1, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_2, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_3, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_4, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_5, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_6, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_7, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - arith64-sum-BE] "
                           "Query is SAT\n");
#endif
                ctx->stats.arith64_sum_BE++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
        tmp                      = input_qword_BE - i;
        tmp_input[input_index_7] = (unsigned long)(tmp & 0xffUL);
        tmp_input[input_index_6] = (unsigned long)((tmp >> 8) & 0xffUL);
        tmp_input[input_index_5] = (unsigned long)((tmp >> 16) & 0xffUL);
        tmp_input[input_index_4] = (unsigned long)((tmp >> 24) & 0xffUL);
        tmp_input[input_index_3] = (unsigned long)((tmp >> 32) & 0xffUL);
        tmp_input[input_index_2] = (unsigned long)((tmp >> 40) & 0xffUL);
        tmp_input[input_index_1] = (unsigned long)((tmp >> 48) & 0xffUL);
        tmp_input[input_index_0] = (unsigned long)((tmp >> 56) & 0xffUL);
        valid_eval = is_valid_eval_index(ctx, input_index_0, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_1, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_2, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_3, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_4, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_5, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_6, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len) &&
                     is_valid_eval_index(ctx, input_index_7, tmp_input,
                                         current_testcase->value_sizes,
                                         current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - arith64-sub-BE] "
                           "Query is SAT\n");
#endif
                ctx->stats.arith64_sub_BE++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
    }
    return 0;
}

static __always_inline int
SUBPHASE_afl_det_int64(fuzzy_ctx_t* ctx, Z3_ast query, Z3_ast branch_condition,
                       unsigned char const** proof, unsigned long* proof_size,
                       unsigned long input_index_0, unsigned long input_index_1,
                       unsigned long input_index_2, unsigned long input_index_3,
                       unsigned long input_index_4, unsigned long input_index_5,
                       unsigned long input_index_6, unsigned long input_index_7)
{
    if (unlikely(skip_afl_det_int32))
        return 0;

    testcase_t* current_testcase = &ctx->testcases.data[0];

    unsigned i;
    for (i = 0; i < sizeof(interesting64) / sizeof(long); ++i) {
        tmp_input[input_index_0] = (unsigned long)(interesting64[i]) & 0xffU;
        tmp_input[input_index_1] =
            (unsigned long)(interesting64[i] >> 8) & 0xffU;
        tmp_input[input_index_2] =
            (unsigned long)(interesting64[i] >> 16) & 0xffU;
        tmp_input[input_index_3] =
            (unsigned long)(interesting64[i] >> 24) & 0xffU;
        tmp_input[input_index_4] =
            (unsigned long)(interesting64[i] >> 24) & 0xffU;
        tmp_input[input_index_5] =
            (unsigned long)(interesting64[i] >> 24) & 0xffU;
        tmp_input[input_index_6] =
            (unsigned long)(interesting64[i] >> 24) & 0xffU;
        tmp_input[input_index_7] =
            (unsigned long)(interesting64[i] >> 24) & 0xffU;
        int valid_eval = is_valid_eval_index(ctx, input_index_0, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_1, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_2, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_3, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_4, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_5, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_6, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_7, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - int64] "
                           "Query is SAT\n");
#endif
                ctx->stats.int64++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }

#if 0
        tmp_input[input_index_7] = (unsigned long)(interesting64[i]) & 0xffU;
        tmp_input[input_index_6] =
            (unsigned long)(interesting64[i] >> 8) & 0xffU;
        tmp_input[input_index_5] =
            (unsigned long)(interesting64[i] >> 16) & 0xffU;
        tmp_input[input_index_4] =
            (unsigned long)(interesting64[i] >> 24) & 0xffU;
        tmp_input[input_index_3] =
            (unsigned long)(interesting64[i] >> 24) & 0xffU;
        tmp_input[input_index_2] =
            (unsigned long)(interesting64[i] >> 24) & 0xffU;
        tmp_input[input_index_1] =
            (unsigned long)(interesting64[i] >> 24) & 0xffU;
        tmp_input[input_index_0] =
            (unsigned long)(interesting64[i] >> 24) & 0xffU;
        valid_eval = is_valid_eval_index(ctx, input_index_0, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_1, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_2, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_3, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_4, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_5, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_6, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len) &&
                         is_valid_eval_index(ctx, input_index_7, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - int64] "
                           "Query is SAT\n");
#endif
                ctx->stats.int64++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
#endif
    }
    return 0;
}

static __always_inline int PHASE_afl_deterministic_groups(
    fuzzy_ctx_t* ctx, Z3_ast query, Z3_ast branch_condition,
    unsigned char const** proof, unsigned long* proof_size)
{
    if (unlikely(skip_afl_deterministic))
        return 0;

    int            ret;
    testcase_t*    current_testcase = &ctx->testcases.data[0];
    index_group_t* g;

#ifdef DEBUG_CHECK_LIGHT
    Z3FUZZ_LOG("Trying AFL Deterministic (groups)\n");
#endif

    set_reset_iter__index_group_t(&ast_data.inputs->index_groups, 1);
    while (
        set_iter_next__index_group_t(&ast_data.inputs->index_groups, 1, &g)) {
        unsigned i;
        // flip 1/2/4 int8 -> do for every group type
        for (i = 0; i < g->n; ++i) {
            unsigned long input_index = g->indexes[i];

            ret = SUBPHASE_afl_det_single_waliking_bit(
                ctx, query, branch_condition, proof, proof_size, input_index);
            if (unlikely(ret == TIMEOUT_V))
                return TIMEOUT_V;
            if (ret)
                return 1;

            ret = SUBPHASE_afl_det_two_waliking_bits(
                ctx, query, branch_condition, proof, proof_size, input_index);
            if (unlikely(ret == TIMEOUT_V))
                return TIMEOUT_V;
            if (ret)
                return 1;

            ret = SUBPHASE_afl_det_four_waliking_bits(
                ctx, query, branch_condition, proof, proof_size, input_index);
            if (unlikely(ret == TIMEOUT_V))
                return TIMEOUT_V;
            if (ret)
                return 1;

            ret = SUBPHASE_afl_det_int8(ctx, query, branch_condition, proof,
                                        proof_size, input_index);
            if (unlikely(ret == TIMEOUT_V))
                return TIMEOUT_V;
            if (ret)
                return 1;

            tmp_input[input_index] =
                (unsigned long)current_testcase->values[input_index];
        }

        switch (g->n) {
            case 2: {
                // flip short
                ret = SUBPHASE_afl_det_flip_short(ctx, query, branch_condition,
                                                  proof, proof_size,
                                                  g->indexes[0], g->indexes[1]);
                if (unlikely(ret == TIMEOUT_V))
                    return TIMEOUT_V;
                if (ret)
                    return 1;

                // 16-bit arithmetics
                ret = SUBPHASE_afl_det_arith16(ctx, query, branch_condition,
                                               proof, proof_size, g->indexes[0],
                                               g->indexes[1]);
                if (unlikely(ret == TIMEOUT_V))
                    return TIMEOUT_V;
                if (ret)
                    return 1;

                // interesting 16
                ret = SUBPHASE_afl_det_int16(ctx, query, branch_condition,
                                             proof, proof_size, g->indexes[0],
                                             g->indexes[1]);
                if (unlikely(ret == TIMEOUT_V))
                    return TIMEOUT_V;
                if (ret)
                    return 1;

                if (set_check__ulong(&ast_data.inputs->indexes,
                                     g->indexes[1] + 1) &&
                    set_check__ulong(&ast_data.inputs->indexes,
                                     g->indexes[1] + 2)) {
                    // interesting 32
                    ret = SUBPHASE_afl_det_int32(
                        ctx, query, branch_condition, proof, proof_size,
                        g->indexes[0], g->indexes[1], g->indexes[1] + 1,
                        g->indexes[1] + 2);
                    if (unlikely(ret == TIMEOUT_V))
                        return TIMEOUT_V;
                    if (ret)
                        return 1;

                    tmp_input[g->indexes[1] + 1] =
                        current_testcase->values[g->indexes[1] + 1];
                    tmp_input[g->indexes[1] + 2] =
                        current_testcase->values[g->indexes[1] + 2];
                }

                tmp_input[g->indexes[0]] =
                    current_testcase->values[g->indexes[0]];
                tmp_input[g->indexes[1]] =
                    current_testcase->values[g->indexes[1]];
                break;
            }
            case 4: {
                // flip int
                ret = SUBPHASE_afl_det_flip_int(
                    ctx, query, branch_condition, proof, proof_size,
                    g->indexes[0], g->indexes[1], g->indexes[2], g->indexes[3]);
                if (unlikely(ret == TIMEOUT_V))
                    return TIMEOUT_V;
                if (ret)
                    return 1;

                // 32-bit arithmetics
                ret = SUBPHASE_afl_det_arith32(
                    ctx, query, branch_condition, proof, proof_size,
                    g->indexes[0], g->indexes[1], g->indexes[2], g->indexes[3]);
                if (unlikely(ret == TIMEOUT_V))
                    return TIMEOUT_V;
                if (ret)
                    return 1;

                // interesting 32
                ret = SUBPHASE_afl_det_int32(
                    ctx, query, branch_condition, proof, proof_size,
                    g->indexes[0], g->indexes[1], g->indexes[2], g->indexes[3]);
                if (unlikely(ret == TIMEOUT_V))
                    return TIMEOUT_V;
                if (ret)
                    return 1;

                if (set_check__ulong(&ast_data.inputs->indexes,
                                     g->indexes[3] + 1) &&
                    set_check__ulong(&ast_data.inputs->indexes,
                                     g->indexes[3] + 2) &&
                    set_check__ulong(&ast_data.inputs->indexes,
                                     g->indexes[3] + 3) &&
                    set_check__ulong(&ast_data.inputs->indexes,
                                     g->indexes[3] + 4)) {
                    // interesting 64
                    ret = SUBPHASE_afl_det_int64(
                        ctx, query, branch_condition, proof, proof_size,
                        g->indexes[0], g->indexes[1], g->indexes[2],
                        g->indexes[3], g->indexes[3] + 1, g->indexes[3] + 2,
                        g->indexes[3] + 3, g->indexes[3] + 4);
                    if (unlikely(ret == TIMEOUT_V))
                        return TIMEOUT_V;
                    if (ret)
                        return 1;
                    tmp_input[g->indexes[3] + 1] =
                        current_testcase->values[g->indexes[3] + 1];
                    tmp_input[g->indexes[3] + 2] =
                        current_testcase->values[g->indexes[3] + 2];
                    tmp_input[g->indexes[3] + 3] =
                        current_testcase->values[g->indexes[3] + 3];
                    tmp_input[g->indexes[3] + 4] =
                        current_testcase->values[g->indexes[3] + 4];
                }

                tmp_input[g->indexes[0]] =
                    current_testcase->values[g->indexes[0]];
                tmp_input[g->indexes[1]] =
                    current_testcase->values[g->indexes[1]];
                tmp_input[g->indexes[2]] =
                    current_testcase->values[g->indexes[2]];
                tmp_input[g->indexes[3]] =
                    current_testcase->values[g->indexes[3]];

                break;
            }
            case 8: {
                // flip long
                ret = SUBPHASE_afl_det_flip_long(
                    ctx, query, branch_condition, proof, proof_size,
                    g->indexes[0], g->indexes[1], g->indexes[2], g->indexes[3],
                    g->indexes[4], g->indexes[5], g->indexes[6], g->indexes[7]);
                if (unlikely(ret == TIMEOUT_V))
                    return TIMEOUT_V;
                if (ret)
                    return 1;

                // 64-bit arithmetics
                ret = SUBPHASE_afl_det_arith64(
                    ctx, query, branch_condition, proof, proof_size,
                    g->indexes[0], g->indexes[1], g->indexes[2], g->indexes[3],
                    g->indexes[4], g->indexes[5], g->indexes[6], g->indexes[7]);
                if (unlikely(ret == TIMEOUT_V))
                    return TIMEOUT_V;
                if (ret)
                    return 1;

                // interesting 64
                ret = SUBPHASE_afl_det_int64(
                    ctx, query, branch_condition, proof, proof_size,
                    g->indexes[0], g->indexes[1], g->indexes[2], g->indexes[3],
                    g->indexes[4], g->indexes[5], g->indexes[6], g->indexes[7]);
                if (unlikely(ret == TIMEOUT_V))
                    return TIMEOUT_V;
                if (ret)
                    return 1;

                tmp_input[g->indexes[0]] =
                    current_testcase->values[g->indexes[0]];
                tmp_input[g->indexes[1]] =
                    current_testcase->values[g->indexes[1]];
                tmp_input[g->indexes[2]] =
                    current_testcase->values[g->indexes[2]];
                tmp_input[g->indexes[3]] =
                    current_testcase->values[g->indexes[3]];
                tmp_input[g->indexes[4]] =
                    current_testcase->values[g->indexes[4]];
                tmp_input[g->indexes[5]] =
                    current_testcase->values[g->indexes[5]];
                tmp_input[g->indexes[6]] =
                    current_testcase->values[g->indexes[6]];
                tmp_input[g->indexes[7]] =
                    current_testcase->values[g->indexes[7]];
                break;
            }
            default: {
                // group size 1 and group with strange sizes (e.g. 3, probably
                // errors in the detection method)
                unsigned i;
                for (i = 0; i < g->n; ++i) {
                    // byte flip
                    ret = SUBPHASE_afl_det_byte_flip(ctx, query,
                                                     branch_condition, proof,
                                                     proof_size, g->indexes[i]);
                    if (unlikely(ret == TIMEOUT_V))
                        return TIMEOUT_V;
                    if (ret)
                        return 1;

                    // 8-bit arithmetics
                    ret = SUBPHASE_afl_det_arith8(ctx, query, branch_condition,
                                                  proof, proof_size,
                                                  g->indexes[i]);
                    if (unlikely(ret == TIMEOUT_V))
                        return TIMEOUT_V;
                    if (ret)
                        return 1;

                    if (set_check__ulong(&ast_data.inputs->indexes,
                                         g->indexes[i] + 1)) {
                        // int 16
                        ret = SUBPHASE_afl_det_int16(
                            ctx, query, branch_condition, proof, proof_size,
                            g->indexes[i], g->indexes[i] + 1);
                        if (unlikely(ret == TIMEOUT_V))
                            return TIMEOUT_V;
                        if (ret)
                            return 1;
#if 0
                        if (set_check__ulong(&ast_data.inputs->indexes,
                                             g->indexes[i] + 2) &&
                            set_check__ulong(&ast_data.inputs->indexes,
                                             g->indexes[i] + 3)) {

                            // int 32
                            ret = SUBPHASE_afl_det_int32(
                                ctx, query, branch_condition, proof, proof_size,
                                g->indexes[i], g->indexes[i] + 1,
                                g->indexes[i] + 2, g->indexes[i] + 3);
                            if (unlikely(ret == TIMEOUT_V))
                                return TIMEOUT_V;
                            if (ret)
                                return 1;

                            if (set_check__ulong(&ast_data.inputs->indexes,
                                                 g->indexes[i] + 4) &&
                                set_check__ulong(&ast_data.inputs->indexes,
                                                 g->indexes[i] + 5) &&
                                set_check__ulong(&ast_data.inputs->indexes,
                                                 g->indexes[i] + 6) &&
                                set_check__ulong(&ast_data.inputs->indexes,
                                                 g->indexes[i] + 7)) {

                                // int 64
                                ret = SUBPHASE_afl_det_int64(
                                    ctx, query, branch_condition, proof,
                                    proof_size, g->indexes[i],
                                    g->indexes[i] + 1, g->indexes[i] + 2,
                                    g->indexes[i] + 3, g->indexes[i] + 4,
                                    g->indexes[i] + 5, g->indexes[i] + 6,
                                    g->indexes[i] + 7);
                                if (unlikely(ret == TIMEOUT_V))
                                    return TIMEOUT_V;
                                if (ret)
                                    return 1;

                                tmp_input[g->indexes[i] + 4] =
                                    (unsigned long)current_testcase
                                        ->values[g->indexes[i] + 4];
                                tmp_input[g->indexes[i] + 5] =
                                    (unsigned long)current_testcase
                                        ->values[g->indexes[i] + 5];
                                tmp_input[g->indexes[i] + 6] =
                                    (unsigned long)current_testcase
                                        ->values[g->indexes[i] + 6];
                                tmp_input[g->indexes[i] + 7] =
                                    (unsigned long)current_testcase
                                        ->values[g->indexes[i] + 7];
                            }

                            tmp_input[g->indexes[i] + 2] =
                                (unsigned long)
                                    current_testcase->values[g->indexes[i] + 2];
                            tmp_input[g->indexes[i] + 3] =
                                (unsigned long)
                                    current_testcase->values[g->indexes[i] + 3];
                        }
#endif

                        tmp_input[g->indexes[i] + 1] =
                            (unsigned long)
                                current_testcase->values[g->indexes[i] + 1];
                    }

                    tmp_input[g->indexes[i]] =
                        (unsigned long)current_testcase->values[g->indexes[i]];
                }
                break;
            }
        }
    }
    return 0;
}

static __always_inline int
PHASE_afl_deterministic(fuzzy_ctx_t* ctx, Z3_ast query, Z3_ast branch_condition,
                        unsigned char const** proof, unsigned long* proof_size)
{

    if (unlikely(skip_afl_deterministic))
        return 0;

    testcase_t* current_testcase = &ctx->testcases.data[0];

#ifdef DEBUG_CHECK_LIGHT
    Z3FUZZ_LOG("Trying AFL Deterministic\n");
#endif
    ulong*        p;
    unsigned long input_index_0, input_index_1, input_index_2, input_index_3;
    int           ret;

    set_reset_iter__ulong(&ast_data.inputs->indexes, 1);
    while (set_iter_next__ulong(&ast_data.inputs->indexes, 1, &p)) {
        input_index_0 = *p;
        // ****************
        // ***** byte *****
        // ****************

        // single waling bit
        ret = SUBPHASE_afl_det_single_waliking_bit(
            ctx, query, branch_condition, proof, proof_size, input_index_0);
        if (unlikely(ret == TIMEOUT_V))
            return TIMEOUT_V;
        if (ret)
            return 1;

        // two walking bits
        ret = SUBPHASE_afl_det_two_waliking_bits(
            ctx, query, branch_condition, proof, proof_size, input_index_0);
        if (unlikely(ret == TIMEOUT_V))
            return TIMEOUT_V;
        if (ret)
            return 1;

        // four walking bits
        ret = SUBPHASE_afl_det_four_waliking_bits(
            ctx, query, branch_condition, proof, proof_size, input_index_0);
        if (unlikely(ret == TIMEOUT_V))
            return TIMEOUT_V;
        if (ret)
            return 1;

        // byte flip
        ret = SUBPHASE_afl_det_byte_flip(ctx, query, branch_condition, proof,
                                         proof_size, input_index_0);
        if (unlikely(ret == TIMEOUT_V))
            return TIMEOUT_V;
        if (ret)
            return 1;

        // 8-bit arithmetics
        ret = SUBPHASE_afl_det_arith8(ctx, query, branch_condition, proof,
                                      proof_size, input_index_0);
        if (unlikely(ret == TIMEOUT_V))
            return TIMEOUT_V;
        if (ret)
            return 1;

        // interesting 8
        ret = SUBPHASE_afl_det_int8(ctx, query, branch_condition, proof,
                                    proof_size, input_index_0);
        if (unlikely(ret == TIMEOUT_V))
            return TIMEOUT_V;
        if (ret)
            return 1;

        tmp_input[input_index_0] =
            (unsigned long)current_testcase->values[input_index_0];
        if (!set_check__ulong(&ast_data.inputs->indexes, input_index_0 + 1))
            continue; // only one byte. Skip

        // ****************
        // ***** word *****
        // ****************
        input_index_1 = input_index_0 + 1;

        // flip short
        ret = SUBPHASE_afl_det_flip_short(ctx, query, branch_condition, proof,
                                          proof_size, input_index_0,
                                          input_index_1);
        if (unlikely(ret == TIMEOUT_V))
            return TIMEOUT_V;
        if (ret)
            return 1;

        // 16-bit arithmetics
        ret =
            SUBPHASE_afl_det_arith16(ctx, query, branch_condition, proof,
                                     proof_size, input_index_0, input_index_1);
        if (unlikely(ret == TIMEOUT_V))
            return TIMEOUT_V;
        if (ret)
            return 1;

        // interesting 16
        ret = SUBPHASE_afl_det_int16(ctx, query, branch_condition, proof,
                                     proof_size, input_index_0, input_index_1);
        if (unlikely(ret == TIMEOUT_V))
            return TIMEOUT_V;
        if (ret)
            return 1;

        tmp_input[input_index_0] = current_testcase->values[input_index_0];
        tmp_input[input_index_1] = current_testcase->values[input_index_1];

        if (!set_check__ulong(&ast_data.inputs->indexes, input_index_0 + 2) ||
            !set_check__ulong(&ast_data.inputs->indexes, input_index_0 + 3))
            continue; // not enough bytes. Skip

        // ***************
        // **** dword ****
        // ***************
        input_index_2 = input_index_0 + 2;
        input_index_3 = input_index_0 + 3;

        // flip int
        ret = SUBPHASE_afl_det_flip_int(
            ctx, query, branch_condition, proof, proof_size, input_index_0,
            input_index_1, input_index_2, input_index_3);
        if (unlikely(ret == TIMEOUT_V))
            return TIMEOUT_V;
        if (ret)
            return 1;

        // 32-bit arithmetics
        ret = SUBPHASE_afl_det_arith32(ctx, query, branch_condition, proof,
                                       proof_size, input_index_0, input_index_1,
                                       input_index_2, input_index_3);
        if (unlikely(ret == TIMEOUT_V))
            return TIMEOUT_V;
        if (ret)
            return 1;

        // interesting 32
        ret = SUBPHASE_afl_det_int32(ctx, query, branch_condition, proof,
                                     proof_size, input_index_0, input_index_1,
                                     input_index_2, input_index_3);
        if (unlikely(ret == TIMEOUT_V))
            return TIMEOUT_V;
        if (ret)
            return 1;

        tmp_input[input_index_0] = current_testcase->values[input_index_0];
        tmp_input[input_index_1] = current_testcase->values[input_index_1];
        tmp_input[input_index_2] = current_testcase->values[input_index_2];
        tmp_input[input_index_3] = current_testcase->values[input_index_3];
    }

    return 0;
}

static __always_inline int PHASE_afl_havoc_mod(fuzzy_ctx_t* ctx, Z3_ast query,
                                               Z3_ast branch_condition,
                                               unsigned char const** proof,
                                               unsigned long*        proof_size)
{

    if (skip_afl_havoc)
        return 0;

#ifdef DEBUG_CHECK_LIGHT
    Z3FUZZ_LOG("Trying AFL Havoc\n");
#endif

    int             havoc_res;
    index_group_t*  random_group;
    unsigned long   random_index;
    unsigned long   index_0;
    unsigned long   index_1;
    unsigned long   index_2;
    unsigned long   index_3;
    unsigned char   val_0;
    unsigned char   val_1;
    unsigned char   val_2;
    unsigned char   val_3;
    unsigned        tmp;
    unsigned        random_tmp;
    unsigned        mutation_pool;
    unsigned        score;
    unsigned long*  indexes;
    unsigned long   indexes_size;
    index_group_t** ig_16;
    unsigned long   ig_16_size;
    index_group_t** ig_32;
    unsigned long   ig_32_size;
    index_group_t** ig_64;
    unsigned long   ig_64_size;
    testcase_t*     current_testcase = &ctx->testcases.data[0];
    index_group_t*  group;

    unsigned i;
    ulong*   p;

    // initialize list input
    indexes      = (unsigned long*)malloc(ast_data.inputs->indexes.size *
                                     sizeof(unsigned long));
    indexes_size = ast_data.inputs->indexes.size;
    // initialize groups input
    ig_16      = (index_group_t**)malloc(ast_data.inputs->index_groups.size *
                                    sizeof(index_group_t*));
    ig_16_size = 0;
    ig_32      = (index_group_t**)malloc(ast_data.inputs->index_groups.size *
                                    sizeof(index_group_t*));
    ig_32_size = 0;
    ig_64      = (index_group_t**)malloc(ast_data.inputs->index_groups.size *
                                    sizeof(index_group_t*));
    ig_64_size = 0;

    i = 0;
    set_reset_iter__ulong(&ast_data.inputs->indexes, 1);
    while (set_iter_next__ulong(&ast_data.inputs->indexes, 1, &p)) {
        indexes[i++] = *p;
    }
    set_reset_iter__index_group_t(&ast_data.inputs->index_groups, 1);
    while (set_iter_next__index_group_t(&ast_data.inputs->index_groups, 1,
                                        &group)) {
        switch (group->n) {
            case 1:
                break;
            case 2:
                ig_16[ig_16_size++] = group;
                break;
            case 4:
                ig_32[ig_32_size++] = group;
                break;
            case 8:
                ig_64[ig_64_size++] = group;
                break;
        }
    }

    havoc_res     = 0;
    mutation_pool = 5 + (ig_64_size + ig_32_size + ig_16_size > 0 ? 3 : 0) +
                    (ig_64_size + ig_32_size > 0 ? 3 : 0);
    score = ast_data.inputs->indexes.size *
            HAVOC_C;                     // HAVOC_C mutations per input (mean)
    score = score > 1000 ? 1000 : score; // no more than 1000 mutations
    for (i = 0; i < score; ++i) {
        switch (UR(mutation_pool)) {
            case 0: {
                // flip bit
                random_index = indexes[UR(indexes_size)];
                tmp_input[random_index] =
                    (unsigned long)FLIP_BIT(tmp_input[random_index], UR(8));
                break;
            }
            case 1: {
                // set interesting byte
                random_index            = indexes[UR(indexes_size)];
                tmp_input[random_index] = (unsigned long)
                    interesting8[UR(sizeof(interesting8) / sizeof(char))];
                break;
            }
            case 2: {
                // random subtract byte
                random_index = indexes[UR(indexes_size)];
                tmp_input[random_index] -= (unsigned char)(UR(35) + 1);
                break;
            }
            case 3: {
                // random add byte
                random_index = indexes[UR(indexes_size)];
                tmp_input[random_index] += (unsigned char)(UR(35) + 1);
                break;
            }
            case 4: {
                // random, byte set
                random_index = indexes[UR(indexes_size)];
                tmp_input[random_index] ^= (unsigned char)(UR(255) + 1);
                break;
            }
            case 5: {
                // set interesting word
                unsigned pool = UR(ig_16_size + ig_32_size + ig_64_size);
                if (pool < ig_16_size) {
                    // word group
                    random_group = ig_16[pool];
                    index_0      = random_group->indexes[0];
                    index_1      = random_group->indexes[1];
                } else if (pool < ig_32_size + ig_16_size) {
                    // dword group
                    random_group = ig_32[pool - ig_16_size];
                    random_tmp   = UR(3);
                    index_0      = random_group->indexes[random_tmp];
                    index_1      = random_group->indexes[random_tmp + 1];
                } else {
                    // qword group
                    random_group = ig_64[pool - ig_32_size - ig_16_size];
                    random_tmp   = UR(7);
                    index_0      = random_group->indexes[random_tmp];
                    index_1      = random_group->indexes[random_tmp + 1];
                }

                short interesting_16 =
                    interesting16[UR(sizeof(interesting16) / sizeof(short))];
                val_0 = interesting_16 & 0xff;
                val_1 = (interesting_16 >> 8) & 0xff;
                if (UR(2)) {
                    tmp     = index_0;
                    index_0 = index_1;
                    index_1 = tmp;
                }
                tmp_input[index_0] = val_0;
                tmp_input[index_1] = val_1;
                break;
            }
            case 6: {
                // random subtract word
                unsigned pool = UR(ig_16_size + ig_32_size + ig_64_size);
                if (pool < ig_16_size) {
                    // word group
                    random_group = ig_16[pool];
                    index_0      = random_group->indexes[0];
                    index_1      = random_group->indexes[1];
                } else if (pool < ig_32_size + ig_16_size) {
                    // dword group
                    random_group = ig_32[pool - ig_16_size];
                    random_tmp   = UR(3);
                    index_0      = random_group->indexes[random_tmp];
                    index_1      = random_group->indexes[random_tmp + 1];
                } else {
                    // qword group
                    random_group = ig_64[pool - ig_32_size - ig_16_size];
                    random_tmp   = UR(7);
                    index_0      = random_group->indexes[random_tmp];
                    index_1      = random_group->indexes[random_tmp + 1];
                }

                if (UR(2)) {
                    tmp     = index_0;
                    index_0 = index_1;
                    index_1 = tmp;
                }
                short val = (tmp_input[index_1] << 8) | tmp_input[index_0];
                val -= UR(35) + 1;
                tmp_input[index_0] = val & 0xff;
                tmp_input[index_1] = (val >> 8) & 0xff;
                break;
            }
            case 7: {
                // random add word
                unsigned pool = UR(ig_16_size + ig_32_size + ig_64_size);
                if (pool < ig_16_size) {
                    // word group
                    random_group = ig_16[pool];
                    index_0      = random_group->indexes[0];
                    index_1      = random_group->indexes[1];
                } else if (pool < ig_32_size + ig_16_size) {
                    // dword group
                    random_group = ig_32[pool - ig_16_size];
                    random_tmp   = UR(3);
                    index_0      = random_group->indexes[random_tmp];
                    index_1      = random_group->indexes[random_tmp + 1];
                } else {
                    // qword group
                    random_group = ig_64[pool - ig_32_size - ig_16_size];
                    random_tmp   = UR(7);
                    index_0      = random_group->indexes[random_tmp];
                    index_1      = random_group->indexes[random_tmp + 1];
                }

                if (UR(2)) {
                    tmp     = index_0;
                    index_0 = index_1;
                    index_1 = tmp;
                }
                short val = (tmp_input[index_1] << 8) | tmp_input[index_0];
                val += UR(35) + 1;
                tmp_input[index_0] = val & 0xff;
                tmp_input[index_1] = (val >> 8) & 0xff;
                break;
            }
            case 8: {
                // set interesting dword
                unsigned pool = UR(ig_32_size + ig_64_size);
                if (pool < ig_32_size) {
                    // dword group
                    random_group = ig_32[pool];
                    index_0      = random_group->indexes[0];
                    index_1      = random_group->indexes[1];
                    index_2      = random_group->indexes[2];
                    index_3      = random_group->indexes[3];
                } else {
                    // qword group
                    random_group = ig_64[pool - ig_32_size];
                    random_tmp   = UR(5);
                    index_0      = random_group->indexes[random_tmp];
                    index_1      = random_group->indexes[random_tmp + 1];
                    index_2      = random_group->indexes[random_tmp + 2];
                    index_3      = random_group->indexes[random_tmp + 3];
                }

                int interesting_32 =
                    interesting32[UR(sizeof(interesting32) / sizeof(int))];
                val_0 = interesting_32 & 0xff;
                val_1 = (interesting_32 >> 8) & 0xff;
                val_2 = (interesting_32 >> 16) & 0xff;
                val_3 = (interesting_32 >> 24) & 0xff;
                if (UR(2)) {
                    tmp     = index_0;
                    index_0 = index_3;
                    index_3 = tmp;

                    tmp     = index_2;
                    index_2 = index_3;
                    index_3 = tmp;
                }
                tmp_input[index_0] = val_0;
                tmp_input[index_1] = val_1;
                tmp_input[index_2] = val_2;
                tmp_input[index_3] = val_3;
                break;
            }
            case 9: {
                // random subtract dword
                unsigned pool = UR(ig_32_size + ig_64_size);
                if (pool < ig_32_size) {
                    // dword group
                    random_group = ig_32[pool];
                    index_0      = random_group->indexes[0];
                    index_1      = random_group->indexes[1];
                    index_2      = random_group->indexes[2];
                    index_3      = random_group->indexes[3];
                } else {
                    // qword group
                    random_group = ig_64[pool - ig_32_size];
                    random_tmp   = UR(5);
                    index_0      = random_group->indexes[random_tmp];
                    index_1      = random_group->indexes[random_tmp + 1];
                    index_2      = random_group->indexes[random_tmp + 2];
                    index_3      = random_group->indexes[random_tmp + 3];
                }
                if (UR(2)) {
                    tmp     = index_0;
                    index_0 = index_3;
                    index_3 = tmp;

                    tmp     = index_2;
                    index_2 = index_3;
                    index_3 = tmp;
                }

                int val = (tmp_input[index_3] << 24) |
                          (tmp_input[index_2] << 16) |
                          (tmp_input[index_1] << 8) | tmp_input[index_0];
                val -= UR(35) + 1;
                tmp_input[index_0] = val & 0xff;
                tmp_input[index_1] = (val >> 8) & 0xff;
                tmp_input[index_2] = (val >> 16) & 0xff;
                tmp_input[index_3] = (val >> 24) & 0xff;
                break;
            }
            case 10: {
                // random add dword
                unsigned pool = UR(ig_32_size + ig_64_size);
                if (pool < ig_32_size) {
                    // dword group
                    random_group = ig_32[pool];
                    index_0      = random_group->indexes[0];
                    index_1      = random_group->indexes[1];
                    index_2      = random_group->indexes[2];
                    index_3      = random_group->indexes[3];
                } else {
                    // qword group
                    random_group = ig_64[pool - ig_32_size];
                    random_tmp   = UR(5);
                    index_0      = random_group->indexes[random_tmp];
                    index_1      = random_group->indexes[random_tmp + 1];
                    index_2      = random_group->indexes[random_tmp + 2];
                    index_3      = random_group->indexes[random_tmp + 3];
                }
                if (UR(2)) {
                    tmp     = index_0;
                    index_0 = index_3;
                    index_3 = tmp;

                    tmp     = index_2;
                    index_2 = index_3;
                    index_3 = tmp;
                }

                int val = (tmp_input[index_3] << 24) |
                          (tmp_input[index_2] << 16) |
                          (tmp_input[index_1] << 8) | tmp_input[index_0];
                val += UR(35) + 1;
                tmp_input[index_0] = val & 0xff;
                tmp_input[index_1] = (val >> 8) & 0xff;
                tmp_input[index_2] = (val >> 16) & 0xff;
                tmp_input[index_3] = (val >> 24) & 0xff;
                break;
            }
            default: {
                ASSERT_OR_ABORT(0, "havoc default case");
            }
        }
        // do evaluate
        int eval_v = __evaluate_branch_query(
            ctx, query, branch_condition, tmp_input,
            current_testcase->value_sizes, current_testcase->values_len);
        if (eval_v == 1) {
#ifdef PRINT_SAT
            Z3FUZZ_LOG("[havoc L5] "
                       "Query is SAT\n");
#endif
            ctx->stats.havoc++;
            ctx->stats.num_sat++;
            __vals_long_to_char(tmp_input, tmp_proof,
                                current_testcase->testcase_len);
            *proof      = tmp_proof;
            *proof_size = current_testcase->testcase_len;
            havoc_res   = 1;
        } else if (unlikely(eval_v == TIMEOUT_V)) {
            havoc_res = TIMEOUT_V;
            break;
        }
    }

    free(indexes);
    free(ig_16);
    free(ig_32);
    free(ig_64);
    return havoc_res;
}

static __always_inline int PHASE_afl_havoc(fuzzy_ctx_t* ctx, Z3_ast query,
                                           Z3_ast branch_condition,
                                           unsigned char const** proof,
                                           unsigned long*        proof_size)
{

    if (skip_afl_havoc)
        return 0;

#ifdef DEBUG_CHECK_LIGHT
    Z3FUZZ_LOG("Trying AFL Havoc\n");
#endif

    int             havoc_res;
    index_group_t*  random_group;
    unsigned long   random_index;
    unsigned long   index_0;
    unsigned long   index_1;
    unsigned long   index_2;
    unsigned long   index_3;
    unsigned char   val_0;
    unsigned char   val_1;
    unsigned char   val_2;
    unsigned char   val_3;
    unsigned        tmp;
    unsigned        random_tmp;
    unsigned        mutation_pool;
    unsigned        score;
    unsigned long*  indexes;
    unsigned long   indexes_size;
    index_group_t** ig_16;
    unsigned long   ig_16_size;
    index_group_t** ig_32;
    unsigned long   ig_32_size;
    index_group_t** ig_64;
    unsigned long   ig_64_size;
    testcase_t*     current_testcase = &ctx->testcases.data[0];
    index_group_t*  group;

    unsigned i, j;
    ulong*   p;

    // initialize list input
    indexes      = (unsigned long*)malloc(ast_data.inputs->indexes.size *
                                     sizeof(unsigned long));
    indexes_size = ast_data.inputs->indexes.size;
    // initialize groups input
    ig_16      = (index_group_t**)malloc(ast_data.inputs->index_groups.size *
                                    sizeof(index_group_t*));
    ig_16_size = 0;
    ig_32      = (index_group_t**)malloc(ast_data.inputs->index_groups.size *
                                    sizeof(index_group_t*));
    ig_32_size = 0;
    ig_64      = (index_group_t**)malloc(ast_data.inputs->index_groups.size *
                                    sizeof(index_group_t*));
    ig_64_size = 0;

    i = 0;
    set_reset_iter__ulong(&ast_data.inputs->indexes, 1);
    while (set_iter_next__ulong(&ast_data.inputs->indexes, 1, &p)) {
        indexes[i++] = *p;
    }
    set_reset_iter__index_group_t(&ast_data.inputs->index_groups, 1);
    while (set_iter_next__index_group_t(&ast_data.inputs->index_groups, 1,
                                        &group)) {
        switch (group->n) {
            case 1:
                break;
            case 2:
                ig_16[ig_16_size++] = group;
                break;
            case 4:
                ig_32[ig_32_size++] = group;
                break;
            case 8:
                ig_64[ig_64_size++] = group;
                break;
        }
    }

    havoc_res     = 0;
    mutation_pool = 5 + (ig_64_size + ig_32_size + ig_16_size > 0 ? 3 : 0) +
                    (ig_64_size + ig_32_size > 0 ? 3 : 0);
    score = ast_data.inputs->indexes.size * HAVOC_C;
    for (i = 0; i < score; ++i) {
        unsigned K = 1 << (1 + UR(HAVOC_STACK_POW2));
        for (j = 0; j < K; ++j) {
            switch (UR(mutation_pool)) {
                case 0: {
                    // flip bit
                    random_index = indexes[UR(indexes_size)];
                    tmp_input[random_index] =
                        (unsigned long)FLIP_BIT(tmp_input[random_index], UR(8));
                    break;
                }
                case 1: {
                    // set interesting byte
                    random_index            = indexes[UR(indexes_size)];
                    tmp_input[random_index] = (unsigned long)
                        interesting8[UR(sizeof(interesting8) / sizeof(char))];
                    break;
                }
                case 2: {
                    // random subtract byte
                    random_index = indexes[UR(indexes_size)];
                    tmp_input[random_index] -= (unsigned char)(UR(35) + 1);
                    break;
                }
                case 3: {
                    // random add byte
                    random_index = indexes[UR(indexes_size)];
                    tmp_input[random_index] += (unsigned char)(UR(35) + 1);
                    break;
                }
                case 4: {
                    // random, byte set
                    random_index = indexes[UR(indexes_size)];
                    tmp_input[random_index] ^= (unsigned char)(UR(255) + 1);
                    break;
                }
                case 5: {
                    // set interesting word
                    unsigned pool = UR(ig_16_size + ig_32_size + ig_64_size);
                    if (pool < ig_16_size) {
                        // word group
                        random_group = ig_16[pool];
                        index_0      = random_group->indexes[0];
                        index_1      = random_group->indexes[1];
                    } else if (pool < ig_32_size + ig_16_size) {
                        // dword group
                        random_group = ig_32[pool - ig_16_size];
                        random_tmp   = UR(3);
                        index_0      = random_group->indexes[random_tmp];
                        index_1      = random_group->indexes[random_tmp + 1];
                    } else {
                        // qword group
                        random_group = ig_64[pool - ig_32_size - ig_16_size];
                        random_tmp   = UR(7);
                        index_0      = random_group->indexes[random_tmp];
                        index_1      = random_group->indexes[random_tmp + 1];
                    }

                    short interesting_16 = interesting16[UR(
                        sizeof(interesting16) / sizeof(short))];
                    val_0                = interesting_16 & 0xff;
                    val_1                = (interesting_16 >> 8) & 0xff;
                    if (UR(2)) {
                        tmp     = index_0;
                        index_0 = index_1;
                        index_1 = tmp;
                    }
                    tmp_input[index_0] = val_0;
                    tmp_input[index_1] = val_1;
                    break;
                }
                case 6: {
                    // random subtract word
                    unsigned pool = UR(ig_16_size + ig_32_size + ig_64_size);
                    if (pool < ig_16_size) {
                        // word group
                        random_group = ig_16[pool];
                        index_0      = random_group->indexes[0];
                        index_1      = random_group->indexes[1];
                    } else if (pool < ig_32_size + ig_16_size) {
                        // dword group
                        random_group = ig_32[pool - ig_16_size];
                        random_tmp   = UR(3);
                        index_0      = random_group->indexes[random_tmp];
                        index_1      = random_group->indexes[random_tmp + 1];
                    } else {
                        // qword group
                        random_group = ig_64[pool - ig_32_size - ig_16_size];
                        random_tmp   = UR(7);
                        index_0      = random_group->indexes[random_tmp];
                        index_1      = random_group->indexes[random_tmp + 1];
                    }

                    if (UR(2)) {
                        tmp     = index_0;
                        index_0 = index_1;
                        index_1 = tmp;
                    }
                    short val = (tmp_input[index_1] << 8) | tmp_input[index_0];
                    val -= UR(35) + 1;
                    tmp_input[index_0] = val & 0xff;
                    tmp_input[index_1] = (val >> 8) & 0xff;
                    break;
                }
                case 7: {
                    // random add word
                    unsigned pool = UR(ig_16_size + ig_32_size + ig_64_size);
                    if (pool < ig_16_size) {
                        // word group
                        random_group = ig_16[pool];
                        index_0      = random_group->indexes[0];
                        index_1      = random_group->indexes[1];
                    } else if (pool < ig_32_size + ig_16_size) {
                        // dword group
                        random_group = ig_32[pool - ig_16_size];
                        random_tmp   = UR(3);
                        index_0      = random_group->indexes[random_tmp];
                        index_1      = random_group->indexes[random_tmp + 1];
                    } else {
                        // qword group
                        random_group = ig_64[pool - ig_32_size - ig_16_size];
                        random_tmp   = UR(7);
                        index_0      = random_group->indexes[random_tmp];
                        index_1      = random_group->indexes[random_tmp + 1];
                    }

                    if (UR(2)) {
                        tmp     = index_0;
                        index_0 = index_1;
                        index_1 = tmp;
                    }
                    short val = (tmp_input[index_1] << 8) | tmp_input[index_0];
                    val += UR(35) + 1;
                    tmp_input[index_0] = val & 0xff;
                    tmp_input[index_1] = (val >> 8) & 0xff;
                    break;
                }
                case 8: {
                    // set interesting dword
                    unsigned pool = UR(ig_32_size + ig_64_size);
                    if (pool < ig_32_size) {
                        // dword group
                        random_group = ig_32[pool];
                        index_0      = random_group->indexes[0];
                        index_1      = random_group->indexes[1];
                        index_2      = random_group->indexes[2];
                        index_3      = random_group->indexes[3];
                    } else {
                        // qword group
                        random_group = ig_64[pool - ig_32_size];
                        random_tmp   = UR(5);
                        index_0      = random_group->indexes[random_tmp];
                        index_1      = random_group->indexes[random_tmp + 1];
                        index_2      = random_group->indexes[random_tmp + 2];
                        index_3      = random_group->indexes[random_tmp + 3];
                    }

                    int interesting_32 =
                        interesting32[UR(sizeof(interesting32) / sizeof(int))];
                    val_0 = interesting_32 & 0xff;
                    val_1 = (interesting_32 >> 8) & 0xff;
                    val_2 = (interesting_32 >> 16) & 0xff;
                    val_3 = (interesting_32 >> 24) & 0xff;
                    if (UR(2)) {
                        tmp     = index_0;
                        index_0 = index_3;
                        index_3 = tmp;

                        tmp     = index_2;
                        index_2 = index_3;
                        index_3 = tmp;
                    }
                    tmp_input[index_0] = val_0;
                    tmp_input[index_1] = val_1;
                    tmp_input[index_2] = val_2;
                    tmp_input[index_3] = val_3;
                    break;
                }
                case 9: {
                    // random subtract dword
                    unsigned pool = UR(ig_32_size + ig_64_size);
                    if (pool < ig_32_size) {
                        // dword group
                        random_group = ig_32[pool];
                        index_0      = random_group->indexes[0];
                        index_1      = random_group->indexes[1];
                        index_2      = random_group->indexes[2];
                        index_3      = random_group->indexes[3];
                    } else {
                        // qword group
                        random_group = ig_64[pool - ig_32_size];
                        random_tmp   = UR(5);
                        index_0      = random_group->indexes[random_tmp];
                        index_1      = random_group->indexes[random_tmp + 1];
                        index_2      = random_group->indexes[random_tmp + 2];
                        index_3      = random_group->indexes[random_tmp + 3];
                    }
                    if (UR(2)) {
                        tmp     = index_0;
                        index_0 = index_3;
                        index_3 = tmp;

                        tmp     = index_2;
                        index_2 = index_3;
                        index_3 = tmp;
                    }

                    int val = (tmp_input[index_3] << 24) |
                              (tmp_input[index_2] << 16) |
                              (tmp_input[index_1] << 8) | tmp_input[index_0];
                    val -= UR(35) + 1;
                    tmp_input[index_0] = val & 0xff;
                    tmp_input[index_1] = (val >> 8) & 0xff;
                    tmp_input[index_2] = (val >> 16) & 0xff;
                    tmp_input[index_3] = (val >> 24) & 0xff;
                    break;
                }
                case 10: {
                    // random add dword
                    unsigned pool = UR(ig_32_size + ig_64_size);
                    if (pool < ig_32_size) {
                        // dword group
                        random_group = ig_32[pool];
                        index_0      = random_group->indexes[0];
                        index_1      = random_group->indexes[1];
                        index_2      = random_group->indexes[2];
                        index_3      = random_group->indexes[3];
                    } else {
                        // qword group
                        random_group = ig_64[pool - ig_32_size];
                        random_tmp   = UR(5);
                        index_0      = random_group->indexes[random_tmp];
                        index_1      = random_group->indexes[random_tmp + 1];
                        index_2      = random_group->indexes[random_tmp + 2];
                        index_3      = random_group->indexes[random_tmp + 3];
                    }
                    if (UR(2)) {
                        tmp     = index_0;
                        index_0 = index_3;
                        index_3 = tmp;

                        tmp     = index_2;
                        index_2 = index_3;
                        index_3 = tmp;
                    }

                    int val = (tmp_input[index_3] << 24) |
                              (tmp_input[index_2] << 16) |
                              (tmp_input[index_1] << 8) | tmp_input[index_0];
                    val += UR(35) + 1;
                    tmp_input[index_0] = val & 0xff;
                    tmp_input[index_1] = (val >> 8) & 0xff;
                    tmp_input[index_2] = (val >> 16) & 0xff;
                    tmp_input[index_3] = (val >> 24) & 0xff;
                    break;
                }
                default: {
                    ASSERT_OR_ABORT(0, "havoc default case");
                }
            }
        }
        // do evaluate
        int eval_v = __evaluate_branch_query(
            ctx, query, branch_condition, tmp_input,
            current_testcase->value_sizes, current_testcase->values_len);
        if (eval_v == 1) {
#ifdef PRINT_SAT
            Z3FUZZ_LOG("[havoc L5] "
                       "Query is SAT\n");
#endif
            ctx->stats.havoc++;
            ctx->stats.num_sat++;
            __vals_long_to_char(tmp_input, tmp_proof,
                                current_testcase->testcase_len);
            *proof      = tmp_proof;
            *proof_size = current_testcase->testcase_len;
            havoc_res   = 1;
            break;
        } else if (unlikely(eval_v == TIMEOUT_V)) {
            havoc_res = TIMEOUT_V;
            break;
        }
    }

    free(indexes);
    free(ig_16);
    free(ig_32);
    free(ig_64);
    return havoc_res;
}

static __always_inline int PHASE_afl_havoc_whole_pi(fuzzy_ctx_t* ctx,
                                                    Z3_ast       query,
                                                    Z3_ast branch_condition,
                                                    unsigned char const** proof,
                                                    unsigned long* proof_size)
{

    if (skip_afl_havoc)
        return 0;

#ifdef DEBUG_CHECK_LIGHT
    Z3FUZZ_LOG("Trying AFL Havoc on whole PI\n");
#endif

    int             havoc_res;
    index_group_t*  random_group;
    unsigned long   random_index;
    unsigned long   index_0;
    unsigned long   index_1;
    unsigned long   index_2;
    unsigned long   index_3;
    unsigned char   val_0;
    unsigned char   val_1;
    unsigned char   val_2;
    unsigned char   val_3;
    unsigned        tmp;
    unsigned        random_tmp;
    unsigned        mutation_pool;
    unsigned        score;
    unsigned long*  indexes;
    unsigned long   indexes_size;
    index_group_t** ig_16;
    unsigned long   ig_16_size;
    index_group_t** ig_32;
    unsigned long   ig_32_size;
    index_group_t** ig_64;
    unsigned long   ig_64_size;
    testcase_t*     current_testcase = &ctx->testcases.data[0];
    index_group_t*  group;

    unsigned i;
    ulong*   p;

    ast_info_ptr tmp_ast_info;
    detect_involved_inputs_wrapper(ctx, query, &tmp_ast_info);

    // initialize list input
    indexes      = (unsigned long*)malloc(tmp_ast_info->indexes.size *
                                     sizeof(unsigned long));
    indexes_size = tmp_ast_info->indexes.size;
    // initialize groups input
    ig_16      = (index_group_t**)malloc(tmp_ast_info->index_groups.size *
                                    sizeof(index_group_t*));
    ig_16_size = 0;
    ig_32      = (index_group_t**)malloc(tmp_ast_info->index_groups.size *
                                    sizeof(index_group_t*));
    ig_32_size = 0;
    ig_64      = (index_group_t**)malloc(tmp_ast_info->index_groups.size *
                                    sizeof(index_group_t*));
    ig_64_size = 0;

    i = 0;
    set_reset_iter__ulong(&tmp_ast_info->indexes, 1);
    while (set_iter_next__ulong(&tmp_ast_info->indexes, 1, &p)) {
        indexes[i++] = *p;
    }
    set_reset_iter__index_group_t(&tmp_ast_info->index_groups, 1);
    while (
        set_iter_next__index_group_t(&tmp_ast_info->index_groups, 1, &group)) {
        switch (group->n) {
            case 1:
                break;
            case 2:
                ig_16[ig_16_size++] = group;
                break;
            case 4:
                ig_32[ig_32_size++] = group;
                break;
            case 8:
                ig_64[ig_64_size++] = group;
                break;
        }
    }

#if 0
    int g;
    for (g = 0; g < indexes_size; ++g)
        Z3FUZZ_LOG("pool: idx %ld\n", indexes[g]);
    for (g = 0; g < ig_16_size; ++g) {
        Z3FUZZ_LOG("in pool: \n");
        print_index_group(ig_16[g]);
    }
    for (g = 0; g < ig_32_size; ++g) {
        Z3FUZZ_LOG("in pool: \n");
        print_index_group(ig_32[g]);
    }
    for (g = 0; g < ig_64_size; ++g) {
        Z3FUZZ_LOG("in pool: \n");
        print_index_group(ig_64[g]);
    }
#endif

    havoc_res     = 0;
    mutation_pool = 5 + (ig_64_size + ig_32_size + ig_16_size > 0 ? 3 : 0) +
                    (ig_64_size + ig_32_size > 0 ? 3 : 0);
    score = tmp_ast_info->indexes.size *
            HAVOC_C;                     // HAVOC_C mutations per input (mean)
    score = score > 1000 ? 1000 : score; // no more than 1000 mutations
    for (i = 0; i < score; ++i) {
        switch (UR(mutation_pool)) {
            case 0: {
                // flip bit
                random_index = indexes[UR(indexes_size)];
                tmp_input[random_index] =
                    (unsigned long)FLIP_BIT(tmp_input[random_index], UR(8));
                break;
            }
            case 1: {
                // set interesting byte
                random_index            = indexes[UR(indexes_size)];
                tmp_input[random_index] = (unsigned long)
                    interesting8[UR(sizeof(interesting8) / sizeof(char))];
                break;
            }
            case 2: {
                // random subtract byte
                random_index = indexes[UR(indexes_size)];
                tmp_input[random_index] -= (unsigned char)(UR(35) + 1);
                break;
            }
            case 3: {
                // random add byte
                random_index = indexes[UR(indexes_size)];
                tmp_input[random_index] += (unsigned char)(UR(35) + 1);
                break;
            }
            case 4: {
                // random, byte set
                random_index = indexes[UR(indexes_size)];
                tmp_input[random_index] ^= (unsigned char)(UR(255) + 1);
                break;
            }
            case 5: {
                // set interesting word
                unsigned pool = UR(ig_16_size + ig_32_size + ig_64_size);
                if (pool < ig_16_size) {
                    // word group
                    random_group = ig_16[pool];
                    index_0      = random_group->indexes[0];
                    index_1      = random_group->indexes[1];
                } else if (pool < ig_32_size + ig_16_size) {
                    // dword group
                    random_group = ig_32[pool - ig_16_size];
                    random_tmp   = UR(3);
                    index_0      = random_group->indexes[random_tmp];
                    index_1      = random_group->indexes[random_tmp + 1];
                } else {
                    // qword group
                    random_group = ig_64[pool - ig_32_size - ig_16_size];
                    random_tmp   = UR(7);
                    index_0      = random_group->indexes[random_tmp];
                    index_1      = random_group->indexes[random_tmp + 1];
                }

                short interesting_16 =
                    interesting16[UR(sizeof(interesting16) / sizeof(short))];
                val_0 = interesting_16 & 0xff;
                val_1 = (interesting_16 >> 8) & 0xff;
                if (UR(2)) {
                    tmp     = index_0;
                    index_0 = index_1;
                    index_1 = tmp;
                }
                tmp_input[index_0] = val_0;
                tmp_input[index_1] = val_1;
                break;
            }
            case 6: {
                // random subtract word
                unsigned pool = UR(ig_16_size + ig_32_size + ig_64_size);
                if (pool < ig_16_size) {
                    // word group
                    random_group = ig_16[pool];
                    index_0      = random_group->indexes[0];
                    index_1      = random_group->indexes[1];
                } else if (pool < ig_32_size + ig_16_size) {
                    // dword group
                    random_group = ig_32[pool - ig_16_size];
                    random_tmp   = UR(3);
                    index_0      = random_group->indexes[random_tmp];
                    index_1      = random_group->indexes[random_tmp + 1];
                } else {
                    // qword group
                    random_group = ig_64[pool - ig_32_size - ig_16_size];
                    random_tmp   = UR(7);
                    index_0      = random_group->indexes[random_tmp];
                    index_1      = random_group->indexes[random_tmp + 1];
                }

                if (UR(2)) {
                    tmp     = index_0;
                    index_0 = index_1;
                    index_1 = tmp;
                }
                short val = (tmp_input[index_1] << 8) | tmp_input[index_0];
                val -= UR(35) + 1;
                tmp_input[index_0] = val & 0xff;
                tmp_input[index_1] = (val >> 8) & 0xff;
                break;
            }
            case 7: {
                // random add word
                unsigned pool = UR(ig_16_size + ig_32_size + ig_64_size);
                if (pool < ig_16_size) {
                    // word group
                    random_group = ig_16[pool];
                    index_0      = random_group->indexes[0];
                    index_1      = random_group->indexes[1];
                } else if (pool < ig_32_size + ig_16_size) {
                    // dword group
                    random_group = ig_32[pool - ig_16_size];
                    random_tmp   = UR(3);
                    index_0      = random_group->indexes[random_tmp];
                    index_1      = random_group->indexes[random_tmp + 1];
                } else {
                    // qword group
                    random_group = ig_64[pool - ig_32_size - ig_16_size];
                    random_tmp   = UR(7);
                    index_0      = random_group->indexes[random_tmp];
                    index_1      = random_group->indexes[random_tmp + 1];
                }

                if (UR(2)) {
                    tmp     = index_0;
                    index_0 = index_1;
                    index_1 = tmp;
                }
                short val = (tmp_input[index_1] << 8) | tmp_input[index_0];
                val += UR(35) + 1;
                tmp_input[index_0] = val & 0xff;
                tmp_input[index_1] = (val >> 8) & 0xff;
                break;
            }
            case 8: {
                // set interesting dword
                unsigned pool = UR(ig_32_size + ig_64_size);
                if (pool < ig_32_size) {
                    // dword group
                    random_group = ig_32[pool];
                    index_0      = random_group->indexes[0];
                    index_1      = random_group->indexes[1];
                    index_2      = random_group->indexes[2];
                    index_3      = random_group->indexes[3];
                } else {
                    // qword group
                    random_group = ig_64[pool - ig_32_size];
                    random_tmp   = UR(5);
                    index_0      = random_group->indexes[random_tmp];
                    index_1      = random_group->indexes[random_tmp + 1];
                    index_2      = random_group->indexes[random_tmp + 2];
                    index_3      = random_group->indexes[random_tmp + 3];
                }

                int interesting_32 =
                    interesting32[UR(sizeof(interesting32) / sizeof(int))];
                val_0 = interesting_32 & 0xff;
                val_1 = (interesting_32 >> 8) & 0xff;
                val_2 = (interesting_32 >> 16) & 0xff;
                val_3 = (interesting_32 >> 24) & 0xff;
                if (UR(2)) {
                    tmp     = index_0;
                    index_0 = index_3;
                    index_3 = tmp;

                    tmp     = index_2;
                    index_2 = index_3;
                    index_3 = tmp;
                }
                tmp_input[index_0] = val_0;
                tmp_input[index_1] = val_1;
                tmp_input[index_2] = val_2;
                tmp_input[index_3] = val_3;
                break;
            }
            case 9: {
                // random subtract dword
                unsigned pool = UR(ig_32_size + ig_64_size);
                if (pool < ig_32_size) {
                    // dword group
                    random_group = ig_32[pool];
                    index_0      = random_group->indexes[0];
                    index_1      = random_group->indexes[1];
                    index_2      = random_group->indexes[2];
                    index_3      = random_group->indexes[3];
                } else {
                    // qword group
                    random_group = ig_64[pool - ig_32_size];
                    random_tmp   = UR(5);
                    index_0      = random_group->indexes[random_tmp];
                    index_1      = random_group->indexes[random_tmp + 1];
                    index_2      = random_group->indexes[random_tmp + 2];
                    index_3      = random_group->indexes[random_tmp + 3];
                }
                if (UR(2)) {
                    tmp     = index_0;
                    index_0 = index_3;
                    index_3 = tmp;

                    tmp     = index_2;
                    index_2 = index_3;
                    index_3 = tmp;
                }

                int val = (tmp_input[index_3] << 24) |
                          (tmp_input[index_2] << 16) |
                          (tmp_input[index_1] << 8) | tmp_input[index_0];
                val -= UR(35) + 1;
                tmp_input[index_0] = val & 0xff;
                tmp_input[index_1] = (val >> 8) & 0xff;
                tmp_input[index_2] = (val >> 16) & 0xff;
                tmp_input[index_3] = (val >> 24) & 0xff;
                break;
            }
            case 10: {
                // random add dword
                unsigned pool = UR(ig_32_size + ig_64_size);
                if (pool < ig_32_size) {
                    // dword group
                    random_group = ig_32[pool];
                    index_0      = random_group->indexes[0];
                    index_1      = random_group->indexes[1];
                    index_2      = random_group->indexes[2];
                    index_3      = random_group->indexes[3];
                } else {
                    // qword group
                    random_group = ig_64[pool - ig_32_size];
                    random_tmp   = UR(5);
                    index_0      = random_group->indexes[random_tmp];
                    index_1      = random_group->indexes[random_tmp + 1];
                    index_2      = random_group->indexes[random_tmp + 2];
                    index_3      = random_group->indexes[random_tmp + 3];
                }
                if (UR(2)) {
                    tmp     = index_0;
                    index_0 = index_3;
                    index_3 = tmp;

                    tmp     = index_2;
                    index_2 = index_3;
                    index_3 = tmp;
                }

                int val = (tmp_input[index_3] << 24) |
                          (tmp_input[index_2] << 16) |
                          (tmp_input[index_1] << 8) | tmp_input[index_0];
                val += UR(35) + 1;
                tmp_input[index_0] = val & 0xff;
                tmp_input[index_1] = (val >> 8) & 0xff;
                tmp_input[index_2] = (val >> 16) & 0xff;
                tmp_input[index_3] = (val >> 24) & 0xff;
                break;
            }
            default: {
                ASSERT_OR_ABORT(0, "havoc default case");
            }
        }
        // do evaluate
        int eval_v = __evaluate_branch_query(
            ctx, query, branch_condition, tmp_input,
            current_testcase->value_sizes, current_testcase->values_len);
        if (eval_v == 1) {
#ifdef PRINT_SAT
            Z3FUZZ_LOG("[havoc L5] "
                       "Query is SAT\n");
#endif
            ctx->stats.havoc++;
            ctx->stats.num_sat++;
            __vals_long_to_char(tmp_input, tmp_proof,
                                current_testcase->testcase_len);
            *proof      = tmp_proof;
            *proof_size = current_testcase->testcase_len;
            havoc_res   = 1;
        } else if (unlikely(eval_v == TIMEOUT_V))
            return TIMEOUT_V;
    }

    free(indexes);
    free(ig_16);
    free(ig_32);
    free(ig_64);
    return havoc_res;
}

static __always_inline int
PHASE_range_bruteforce(fuzzy_ctx_t* ctx, Z3_ast query, Z3_ast branch_condition,
                       unsigned char const** proof, unsigned long* proof_size)
{
    unsigned long i;
    int           j, k;
    uint64_t      c;

    if (unlikely(skip_range_brute_force))
        return 0;
    if (performing_aggressive_optimistic)
        return 0;

#ifdef DEBUG_CHECK_LIGHT
    Z3FUZZ_LOG("Trying range bruteforce\n");
#endif

    set__interval_group_ptr* group_intervals =
        (set__interval_group_ptr*)ctx->group_intervals;
    testcase_t* current_testcase = &ctx->testcases.data[0];

    if (ast_data.inputs->index_groups.size != 1)
        return 0; // more than one group or no groups

    index_group_t* ig = NULL;
    set_reset_iter__index_group_t(&ast_data.inputs->index_groups, 0);
    set_iter_next__index_group_t(&ast_data.inputs->index_groups, 0, &ig);
    ASSERT_OR_ABORT(
        ig->n > 0,
        "PHASE_range_bruteforce() - group size <= 0. It shouldn't happen");

    wrapped_interval_t* interval =
        interval_group_get_interval(group_intervals, ig);
    if (interval == 0)
        return 0; // no interval

    if (wi_get_range(interval) > RANGE_MAX_WIDTH_BRUTE_FORCE)
        goto TRY_MIN_MAX; // range too wide

    wrapped_interval_iter_t it = wi_init_iter_values(interval);
    uint64_t                val;
    while (wi_iter_get_next(&it, &val)) {
        set_tmp_input_group_to_value(ig, val);
        int eval_v = __evaluate_branch_query(
            ctx, query, branch_condition, tmp_input,
            current_testcase->value_sizes, current_testcase->values_len);
        if (eval_v == 1) {
#ifdef PRINT_SAT
            Z3FUZZ_LOG("[check light - range bruteforce] Query is SAT\n");
#endif
            ctx->stats.range_brute_force++;
            ctx->stats.num_sat++;
            __vals_long_to_char(tmp_input, tmp_proof,
                                current_testcase->testcase_len);
            *proof      = tmp_proof;
            *proof_size = current_testcase->testcase_len;
            return 1;
        } else if (unlikely(eval_v == TIMEOUT_V))
            return TIMEOUT_V;
    }

    // the query is unsat
    return 2;

TRY_MIN_MAX:
    for (j = 0; j < 2; ++j) {
        if (j == 0)
            c = interval->min;
        else
            c = interval->max;

        for (k = 0; k < ig->n; ++k) {
            unsigned int  index = ig->indexes[ig->n - k - 1];
            unsigned char b     = __extract_from_long(c, k);

#ifdef DEBUG_CHECK_LIGHT
            Z3FUZZ_LOG("range bruteforce - inj byte: 0x%x @ %d\n", b, index);
#endif
            if (current_testcase->values[index] == (unsigned long)b)
                continue;

            tmp_input[index] = b;
        }
        int valid_eval = is_valid_eval_group(ctx, ig, tmp_input,
                                             current_testcase->value_sizes,
                                             current_testcase->values_len);
        if (valid_eval) {
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG("[check light - range bruteforce] Query "
                           "is SAT\n");
#endif
                ctx->stats.range_brute_force++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->values_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
    }
    for (k = 0; k < ig->n; ++k) {
        i            = ig->indexes[ig->n - k - 1];
        tmp_input[i] = current_testcase->values[i];
    }
    return 0;
}

static __always_inline int
PHASE_range_bruteforce_opt(fuzzy_ctx_t* ctx, Z3_ast query,
                           Z3_ast branch_condition, unsigned char const** proof,
                           unsigned long* proof_size)
{
    if (unlikely(skip_range_brute_force_opt))
        return 0;

#ifdef DEBUG_CHECK_LIGHT
    Z3FUZZ_LOG("Trying range bruteforce optimistic\n");
#endif

    set__interval_group_ptr* group_intervals =
        (set__interval_group_ptr*)ctx->group_intervals;
    testcase_t* current_testcase = &ctx->testcases.data[0];

    wrapped_interval_t* interval = NULL;
    index_group_t*      ig       = NULL;
    set_reset_iter__index_group_t(&ast_data.inputs->index_groups, 0);
    while (
        set_iter_next__index_group_t(&ast_data.inputs->index_groups, 0, &ig)) {
        ASSERT_OR_ABORT(ig->n > 0, "PHASE_range_bruteforce_opt() - group size "
                                   "< 0. It shouldn't happen");

        interval = interval_group_get_interval(group_intervals, ig);
        if (interval == 0)
            continue; // no interval

        int                     i  = 0;
        wrapped_interval_iter_t it = wi_init_iter_values(interval);
        uint64_t                val;
        while (wi_iter_get_next(&it, &val)) {
            if (i++ > RANGE_MAX_WIDTH_BRUTE_FORCE / 4)
                break;
            set_tmp_input_group_to_value(ig, val);
            int eval_v = __evaluate_branch_query(
                ctx, query, branch_condition, tmp_input,
                current_testcase->value_sizes, current_testcase->values_len);
            if (eval_v == 1) {
#ifdef PRINT_SAT
                Z3FUZZ_LOG(
                    "[check light - range bruteforce opt] Query is SAT\n");
#endif
                ctx->stats.range_brute_force_opt++;
                ctx->stats.num_sat++;
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                *proof      = tmp_proof;
                *proof_size = current_testcase->testcase_len;
                return 1;
            } else if (unlikely(eval_v == TIMEOUT_V))
                return TIMEOUT_V;
        }
    }
    return 0;
}

static int __query_check_light(fuzzy_ctx_t* ctx, Z3_ast query,
                               Z3_ast                branch_condition,
                               unsigned char const** proof,
                               unsigned long*        proof_size)
{
    // 1 -> succeded
#ifdef DEBUG_CHECK_LIGHT
    // Z3FUZZ_LOG("query: \n%s\n", Z3_ast_to_string(ctx->z3_ctx, query));
    Z3FUZZ_LOG("branch condition: \n%s\n\n",
               Z3_ast_to_string(ctx->z3_ctx, branch_condition));
    print_index_queue(ast_data.inputs);
    print_interval_groups(ctx);
    print_univocally_defined(ctx);
#endif
    testcase_t* current_testcase = &ctx->testcases.data[0];
    int         res;

    // check if sat in seed
    int eval_v = __evaluate_branch_query(
        ctx, query, branch_condition, tmp_input, current_testcase->value_sizes,
        current_testcase->values_len);
    if (eval_v == 1) {
#ifdef DEBUG_CHECK_LIGHT
        Z3FUZZ_LOG("sat in seed... [opt_found = %d]\n", opt_found);
#endif
        ctx->stats.sat_in_seed++;
        __vals_long_to_char(tmp_input, tmp_proof,
                            current_testcase->testcase_len);
        *proof      = tmp_proof;
        *proof_size = current_testcase->testcase_len;
        return 1;
    } else if (unlikely(eval_v == TIMEOUT_V))
        return TIMEOUT_V;

    // Reuse Phase
    if (ctx->testcases.size > 1) {
        res = PHASE_reuse(ctx, query, branch_condition, proof, proof_size);
        if (unlikely(res == TIMEOUT_V))
            return TIMEOUT_V;
        if (res)
            return 1;
    }

    if (log_query_stats)
        fprintf(query_log, "\n%p;%lu;%lu;%lu;%s;%u;%u", ctx,
                ast_data.inputs->query_size, ast_data.inputs->indexes.size,
                ast_data.inputs->index_groups.size,
                ast_data.is_input_to_state ? "true" : "false",
                ast_data.inputs->linear_arithmetic_operations,
                ast_data.inputs->nonlinear_arithmetic_operations);
    if (ast_data.inputs->indexes.size == 0) { // constant branch condition!
        int eval_v = __evaluate_branch_query(
            ctx, query, branch_condition, tmp_input,
            current_testcase->value_sizes, current_testcase->values_len);
        if (eval_v == 1) {
            __vals_long_to_char(tmp_input, tmp_proof,
                                current_testcase->testcase_len);
            *proof      = tmp_proof;
            *proof_size = current_testcase->testcase_len;
            return 1;
        } else if (unlikely(eval_v == TIMEOUT_V))
            return TIMEOUT_V;
        return 0;
    }

    // Input to State
    if (ast_data.is_input_to_state) {
        // input to state detected
        res = PHASE_input_to_state(ctx, query, branch_condition, proof,
                                   proof_size);
        if (unlikely(res == TIMEOUT_V))
            return TIMEOUT_V;
        if (res == 2)
            return 0;
        if (res == 1)
            return 1;
    }

    // Simple math
    res = PHASE_simple_math(ctx, query, branch_condition, proof, proof_size);
    if (unlikely(res == TIMEOUT_V))
        return TIMEOUT_V;
    if (res == 1)
        return 1;
    if (res == 2)
        return 0;

    // Range bruteforce
    res =
        PHASE_range_bruteforce(ctx, query, branch_condition, proof, proof_size);
    if (unlikely(res == TIMEOUT_V))
        return TIMEOUT_V;
    if (res == 2)
        return 0;
    if (res == 1)
        return 1;

    // Range bruteforce optimistic
    res = PHASE_range_bruteforce_opt(ctx, query, branch_condition, proof,
                                     proof_size);
    if (unlikely(res == TIMEOUT_V))
        return TIMEOUT_V;
    if (res == 1)
        return 1;

    // Input to State Extended
    if (ast_data.values.size > 0 ||
        ast_data.inputs->inp_to_state_ite.size > 0) {
        int res = PHASE_input_to_state_extended(ctx, query, branch_condition,
                                                proof, proof_size);
        if (unlikely(res == TIMEOUT_V))
            return TIMEOUT_V;
        if (res)
            return 1;
    }

    // Pure Brute Force - Only One Byte is Involved
    if (ast_data.inputs->indexes.size == 1) {
        // if the fase fails, we exit -> the query is UNSAT
        res =
            PHASE_brute_force(ctx, query, branch_condition, proof, proof_size);
        if (unlikely(res == TIMEOUT_V))
            return TIMEOUT_V;
        if (res != 2)
            return res;
    }

    // Gradient Based Transformation
    res =
        PHASE_gradient_descend(ctx, query, branch_condition, proof, proof_size);
    if (unlikely(res == TIMEOUT_V))
        return TIMEOUT_V;
    if (res)
        return 1;

        // Afl Deterministic Transformations
#ifdef USE_AFL_DET_GROUPS
    res = PHASE_afl_deterministic_groups(ctx, query, branch_condition, proof,
                                         proof_size);
#else
    res = PHASE_afl_deterministic(ctx, query, branch_condition, proof,
                                  proof_size);
#endif
    if (unlikely(res == TIMEOUT_V))
        return TIMEOUT_V;
    if (res)
        return 1;

        // Afl Havoc Transformation
#ifndef USE_HAVOC_ON_WHOLE_PI
    res = PHASE_afl_havoc(ctx, query, branch_condition, proof, proof_size);
#elif USE_HAVOC_MOD
    res = PHASE_afl_havoc_mod(ctx, query, branch_condition, proof, proof_size);
#else
    res = PHASE_afl_havoc_whole_pi(ctx, query, branch_condition, proof,
                                   proof_size);
#endif
    if (unlikely(res == TIMEOUT_V))
        return TIMEOUT_V;
    if (res)
        return 1;

    return 0;
}

static inline int ig_has_index(index_group_t* ig, ulong idx)
{
    unsigned i;
    for (i = 0; i < ig->n; ++i) {
        if (ig->indexes[i] == idx)
            return 1;
    }
    return 0;
}

static inline int find_group_with_all_inputs(index_group_t* ig)
{
    ulong*         p;
    index_group_t* tmp_ig;
    set_reset_iter__index_group_t(&ast_data.inputs->index_groups, 0);
    while (set_iter_next__index_group_t(&ast_data.inputs->index_groups, 0,
                                        &tmp_ig)) {
        int has_all = 1;
        set_reset_iter__ulong(&ast_data.inputs->indexes, 0);
        while (set_iter_next__ulong(&ast_data.inputs->indexes, 0, &p)) {
            if (!ig_has_index(tmp_ig, *p)) {
                has_all = 0;
                break;
            }
        }
        if (has_all) {
            *ig = *tmp_ig;
            return 1;
        }
    }
    return 0;
}

static inline int PHASE_freeze_neighbours(fuzzy_ctx_t* ctx, Z3_ast query,
                                          Z3_ast branch_condition,
                                          unsigned char const** proof,
                                          unsigned long*        proof_size)
{
    if (skip_freeze_neighbours)
        return 0;

    index_group_t ig;
    if (ast_data.inputs->index_groups.size == 1) {
        index_group_t* tmp_ig = NULL;
        set_reset_iter__index_group_t(&ast_data.inputs->index_groups, 0);
        set_iter_next__index_group_t(&ast_data.inputs->index_groups, 0,
                                     &tmp_ig);
        assert(tmp_input != NULL && "unreachable");
        ig = *tmp_ig;
    } else if (!find_group_with_all_inputs(&ig))
        return 0;

    testcase_t* current_testcase = &ctx->testcases.data[0];
    uint64_t    initial_val      = get_group_value_in_tmp_input(&ig);
    unsigned    i;
    for (i = 1; i < 256; ++i) {
        set_tmp_input_group_to_value(&ig, initial_val + i);
        int eval_v = __evaluate_branch_query(
            ctx, query, branch_condition, tmp_input,
            current_testcase->value_sizes, current_testcase->values_len);
        if (eval_v == 1) {
#ifdef PRINT_SAT
            Z3FUZZ_LOG("[check light - freeze neighbours] "
                       "Query is SAT\n");
#endif
            ctx->stats.multigoal++;
            ctx->stats.num_sat++;
            __vals_long_to_char(tmp_input, tmp_proof,
                                current_testcase->testcase_len);
            *proof      = tmp_proof;
            *proof_size = current_testcase->testcase_len;
            return 1;
        } else if (unlikely(eval_v == TIMEOUT_V))
            return TIMEOUT_V;
    }
    set_tmp_input_group_to_value(&ig, initial_val);

    uint64_t initial_val_inv = get_group_value_in_tmp_input_inv(&ig);
    for (i = 1; i < 256; ++i) {
        set_tmp_input_group_to_value_inv(&ig, initial_val_inv + i);
        int eval_v = __evaluate_branch_query(
            ctx, query, branch_condition, tmp_input,
            current_testcase->value_sizes, current_testcase->values_len);
        if (eval_v == 1) {
#ifdef PRINT_SAT
            Z3FUZZ_LOG("[check light - freeze neighbours] "
                       "Query is SAT\n");
#endif
            ctx->stats.multigoal++;
            ctx->stats.num_sat++;
            __vals_long_to_char(tmp_input, tmp_proof,
                                current_testcase->testcase_len);
            *proof      = tmp_proof;
            *proof_size = current_testcase->testcase_len;
            return 1;
        } else if (unlikely(eval_v == TIMEOUT_V))
            return TIMEOUT_V;
    }
    set_tmp_input_group_to_value(&ig, initial_val);
    return 0;
}

static inline int check_if_range_for_indexes(fuzzy_ctx_t* ctx,
                                             set__ulong*  indexes)
{
    dict__da__interval_group_ptr* index_to_group_intervals =
        (dict__da__interval_group_ptr*)ctx->index_to_group_intervals;

    ulong* i;
    set_reset_iter__ulong(indexes, 0);
    while (set_iter_next__ulong(indexes, 0, &i)) {
        if (dict_get_ref__da__interval_group_ptr(index_to_group_intervals,
                                                 *i) != NULL)
            return 1;
    }
    return 0;
}

static inline void aggressive_optimistic(fuzzy_ctx_t* ctx,
                                         Z3_ast       branch_condition)
{
#ifndef ENABLE_AGGRESSIVE_OPTIMISTIC
    return;
#endif
#ifdef DEBUG_CHECK_LIGHT
    Z3FUZZ_LOG("Trying aggressive optimistic\n");
#endif
    if ((ast_data.inputs->indexes_ud.size |
         ast_data.inputs->index_groups_ud.size) == 0 &&
        !check_if_range_for_indexes(ctx, &ast_data.inputs->indexes))
        return;

    fuzzy_stats_t tmp_stats;
    memcpy(&tmp_stats, &ctx->stats, sizeof(fuzzy_stats_t));

    unsigned long i;
    for (i = 0; i < ast_data.inputs->indexes_ud.size; ++i)
        set_add__ulong(&ast_data.inputs->indexes,
                       ast_data.inputs->indexes_ud.data[i]);
    for (i = 0; i < ast_data.inputs->index_groups_ud.size; ++i)
        set_add__index_group_t(&ast_data.inputs->index_groups,
                               ast_data.inputs->index_groups_ud.data[i]);

    check_is_valid                   = 0;
    performing_aggressive_optimistic = 1;
    unsigned char const* dummy_proof;
    unsigned long        dummy_proof_size;
    z3fuzz_query_check_light(ctx, Z3_mk_true(ctx->z3_ctx), branch_condition,
                             &dummy_proof, &dummy_proof_size);
    performing_aggressive_optimistic = 0;
    check_is_valid                   = 1;

    unsigned long delta_aggressive_opt_evaluate =
        ctx->stats.num_evaluate - tmp_stats.num_evaluate;
    memcpy(&ctx->stats, &tmp_stats, sizeof(fuzzy_stats_t));
    ctx->stats.aggressive_opt_evaluate += delta_aggressive_opt_evaluate;
}

static inline int query_check_light_and_multigoal(fuzzy_ctx_t* ctx,
                                                  Z3_ast       query,
                                                  Z3_ast       branch_condition,
                                                  unsigned char const** proof,
                                                  unsigned long* proof_size)
{
    testcase_t* curr_t = &ctx->testcases.data[0];

    int res =
        __query_check_light(ctx, query, branch_condition, proof, proof_size);
    if (unlikely(res == TIMEOUT_V))
        return 0;
#if 0
    Z3_app   __app = Z3_to_app(ctx->z3_ctx, query);
    unsigned i;
    for (i = 0; i < Z3_get_app_num_args(ctx->z3_ctx, __app); ++i) {
        if (!ctx->model_eval(ctx->z3_ctx, Z3_get_app_arg(ctx->z3_ctx, __app, i),
                             tmp_opt_input, curr_t->value_sizes,
                             curr_t->values_len, NULL)) {
            puts("this is UNSAT");
            z3fuzz_print_expr(ctx, Z3_get_app_arg(ctx->z3_ctx, __app, i));
        }
    }
#endif
    if (res == 1 || performing_aggressive_optimistic)
        // the query is SAT
        goto END_FUN_2;
    if (opt_found == 0) {
        // we were not able to flip the branch condition, aggressive optimistic
        // (ignore univocally defined and ranges)
        aggressive_optimistic(ctx, branch_condition);
        goto END_FUN_2;
    }

    // check if there are expressions (marked as conflicting) that operate on
    // the same inputs of the branch condition
    set__ulong local_conflicting_asts;
    set_init__ulong(&local_conflicting_asts, index_hash, index_equals);

    dict__conflicting_ptr* conflicting_asts =
        (dict__conflicting_ptr*)ctx->conflicting_asts;
    ulong* idx;
    set_reset_iter__ulong(&ast_data.inputs->indexes, 0);
    while (set_iter_next__ulong(&ast_data.inputs->indexes, 0, &idx)) {
        set__ast_ptr** s =
            dict_get_ref__conflicting_ptr(conflicting_asts, *idx);
        if (s == NULL)
            continue;

        ast_ptr* ast_p;
        set_reset_iter__ast_ptr(*s, 0);
        while (set_iter_next__ast_ptr(*s, 0, &ast_p)) {
            set_add__ulong(&local_conflicting_asts, (ulong)ast_p->ast);
        }
    }

    if (local_conflicting_asts.size == 0)
        // no conflicting AST
        goto END_FUN_1;

#ifdef DEBUG_CHECK_LIGHT
    Z3FUZZ_LOG("Trying multigoal\n");
#endif

    Z3_ast qb[] = {branch_condition, query};
    query       = Z3_mk_and(ctx->z3_ctx, 2, qb);
    Z3_inc_ref(ctx->z3_ctx, query);

    fuzzy_stats_t bk_stats;
    memcpy(&bk_stats, &ctx->stats, sizeof(fuzzy_stats_t));

    // set tmp_input to the input that made the branch condition true
    memcpy(tmp_input, tmp_opt_input,
           curr_t->values_len * sizeof(unsigned long));

    // ast_info of the branch condition
    ast_info_ptr branch_ast_info = ast_data.inputs;

    // set of blacklisted indexes (i.e. fixed indexes, we do not want to mutate
    // them)
    set__ulong black_indexes;
    set_init__ulong(&black_indexes, index_hash, index_equals);

    // init blacklisted indexes with indexes from the branch condition: we do
    // not want to mutate them!
    ulong* p;
    set_reset_iter__ulong(&branch_ast_info->indexes, 0);
    while (set_iter_next__ulong(&branch_ast_info->indexes, 0, &p)) {
#ifdef DEBUG_CHECK_LIGHT
        Z3FUZZ_LOG("freezing inp[%ld] = 0x%02lx\n", *p, tmp_input[*p]);
#endif
        set_add__ulong(&black_indexes, *p);
    }

    res = PHASE_freeze_neighbours(ctx, query, branch_condition, proof,
                                  proof_size);
    if (unlikely(res == TIMEOUT_V))
        return 0;
    if (res == 1)
        goto END_FUN_0;

    ast_info_ptr new_ast_info = (ast_info_ptr)malloc(sizeof(ast_info_t));
    ast_info_init(new_ast_info);

    Z3_ast* ast;
    set_reset_iter__ulong(&local_conflicting_asts, 0);
    while (set_iter_next__ulong(&local_conflicting_asts, 0, (ulong**)&ast)) {
        if (ctx->model_eval(ctx->z3_ctx, *ast, tmp_input, curr_t->value_sizes,
                            curr_t->values_len, NULL)) {
            // conflicting AST is true
            continue;
        }

        // reset globals
        opt_found = 0;
        __reset_ast_data();
        __detect_early_constants(ctx, *ast, &ast_data);
        __put_solutions_of_current_groups_to_early_constants(ctx, &ast_data,
                                                             branch_ast_info);

        // get involved inputs from query
        ast_info_ptr ast_info;
        detect_involved_inputs_wrapper(ctx, *ast, &ast_info);

        // populate new_ast_info with ast_info BUT without blacklisted_indexes
        ast_info_populate_with_blacklist(new_ast_info, ast_info,
                                         &black_indexes);
        ast_data.inputs = new_ast_info;

        if (new_ast_info->indexes.size != 0) {
            ctx->stats.conflicting_fallbacks++;

            res = __query_check_light(ctx, query, *ast, proof, proof_size);
            if (unlikely(res == TIMEOUT_V)) {
                res = 0;
                break;
            }
            if (res == 1) {
                // the PI is true, we have fixed the input
                bk_stats.multigoal++;
                bk_stats.num_sat++;
                break;
            } else if (opt_found) {
                // PI is not True, but this ast is True
                // > make tmp_opt_input as new input
                // > update black_indexes
                // > continue the loop
#if 0
                puts("continue the loop!");
                z3fuzz_print_expr(ctx, *ast);
                Z3_app   __app = Z3_to_app(ctx->z3_ctx, query);
                unsigned i;
                for (i = 0; i < Z3_get_app_num_args(ctx->z3_ctx, __app); ++i) {
                    if (!ctx->model_eval(ctx->z3_ctx,
                                         Z3_get_app_arg(ctx->z3_ctx, __app, i),
                                         tmp_opt_input, curr_t->value_sizes,
                                         curr_t->values_len, NULL)) {
                        puts("this is UNSAT");
                        z3fuzz_print_expr(
                            ctx, Z3_get_app_arg(ctx->z3_ctx, __app, i));
                    }
                }
#endif
                memcpy(tmp_input, tmp_opt_input,
                       curr_t->values_len * sizeof(unsigned long));
                while (set_iter_next__ulong(&ast_info->indexes, 0, &p))
                    set_add__ulong(&black_indexes, *p);
                ast_info_reset(new_ast_info);
                branch_ast_info = ast_info;
            } else {
                // we are not able to make this AST true, quit
                ctx->stats.conflicting_fallbacks_no_true++;
                break;
            }
        } else
            ctx->stats.conflicting_fallbacks_same_inputs++;
    }

    // if we are here, we populated 'opt_proof' at least one time,
    // we do not want to prevent the user to ask for the optimistic
    // solution
    opt_found                      = 1;
    bk_stats.num_evaluate          = ctx->stats.num_evaluate;
    bk_stats.conflicting_fallbacks = ctx->stats.conflicting_fallbacks;
    bk_stats.conflicting_fallbacks_no_true =
        ctx->stats.conflicting_fallbacks_no_true;
    bk_stats.conflicting_fallbacks_same_inputs =
        ctx->stats.conflicting_fallbacks_same_inputs;
    memcpy(&ctx->stats, &bk_stats, sizeof(fuzzy_stats_t));

    ast_info_ptr_free(&new_ast_info);
END_FUN_0:
    Z3_dec_ref(ctx->z3_ctx, query);
    set_free__ulong(&black_indexes, NULL);
END_FUN_1:
    set_free__ulong(&local_conflicting_asts, NULL);
END_FUN_2:
    return res;
}

static inline Z3_ast get_query_without_branch_condition(fuzzy_ctx_t* ctx,
                                                        Z3_ast       query)
{
    int with_not;
    if (!is_and_constraint(ctx, query, &with_not) || with_not) {
        return Z3_mk_true(ctx->z3_ctx);
    }

    Z3_app   app      = Z3_to_app(ctx->z3_ctx, query);
    unsigned num_args = Z3_get_app_num_args(ctx->z3_ctx, app);
    if (num_args < 2)
        return Z3_mk_true(ctx->z3_ctx);

    Z3_ast args[num_args - 1];

    unsigned i;
    for (i = 1; i < num_args; ++i) {
        Z3_ast child = Z3_get_app_arg(ctx->z3_ctx, app, i);
        args[i - 1]  = child;
    }

    return Z3_mk_and(ctx->z3_ctx, num_args - 1, args);
}

static inline int handle_and_constraint(fuzzy_ctx_t* ctx, Z3_ast query,
                                        Z3_ast                branch_condition,
                                        unsigned char const** proof,
                                        unsigned long*        proof_size)
{
    int res_opt = 1;
    int res     = 1;
    int i;

#ifdef DEBUG_CHECK_LIGHT
    Z3FUZZ_LOG("In handle_and_constraint\n");
#endif

    Z3_ast query_no_branch = query;
    Z3_inc_ref(ctx->z3_ctx, query_no_branch);

    ast_info_ptr new_ast_info = (ast_info_ptr)malloc(sizeof(ast_info_t));
    ast_info_init(new_ast_info);

    set__ulong black_indexes;
    set_init__ulong(&black_indexes, index_hash, index_equals);

    da__Z3_ast args;
    da_init__Z3_ast(&args);

    flatten_and_args(ctx, branch_condition, &args);
    for (i = 0; i < args.size; ++i) {
        Z3_ast node = args.data[i];

        // detect groups with blacklist
        opt_found = 0;
        __reset_ast_data();
        __detect_early_constants(ctx, node, &ast_data);
        ast_info_ptr ast_info;
        detect_involved_inputs_wrapper(ctx, node, &ast_info);
        ast_info_populate_with_blacklist(new_ast_info, ast_info,
                                         &black_indexes);
        ast_data.inputs = new_ast_info;
        if (ast_data.inputs == 0)
            break;

        res &= query_check_light_and_multigoal(
            ctx, res_opt ? query_no_branch : node, node, proof, proof_size);
        res_opt &= opt_found;
        if (res == 0 && res_opt == 0)
            break;

        // update blacklist
        ulong* p;
        set_reset_iter__ulong(&new_ast_info->indexes, 0);
        while (set_iter_next__ulong(&new_ast_info->indexes, 0, &p))
            set_add__ulong(&black_indexes, *p);
        ast_info_reset(new_ast_info);
    }
    if (!opt_found) {
        // try with another order...
        res     = 1;
        res_opt = 1;
        ast_info_reset(new_ast_info);
        set_remove_all__ulong(&black_indexes, NULL);
        for (i = args.size - 1; i >= 0; --i) {
            Z3_ast node = args.data[i];

            // detect groups with blacklist
            opt_found = 0;
            __reset_ast_data();
            __detect_early_constants(ctx, node, &ast_data);
            ast_info_ptr ast_info;
            detect_involved_inputs_wrapper(ctx, node, &ast_info);
            ast_info_populate_with_blacklist(new_ast_info, ast_info,
                                             &black_indexes);
            ast_data.inputs = new_ast_info;
            if (ast_data.inputs == 0)
                break;

            res &= query_check_light_and_multigoal(
                ctx, res_opt ? query_no_branch : node, node, proof, proof_size);
            res_opt &= opt_found;
            if (res == 0 && res_opt == 0)
                break;

            // update blacklist
            ulong* p;
            set_reset_iter__ulong(&new_ast_info->indexes, 0);
            while (set_iter_next__ulong(&new_ast_info->indexes, 0, &p))
                set_add__ulong(&black_indexes, *p);
            ast_info_reset(new_ast_info);
        }
    }

    ast_info_ptr_free(&new_ast_info);
    set_free__ulong(&black_indexes, NULL);
    for (i = 0; i < args.size; ++i)
        Z3_dec_ref(ctx->z3_ctx, args.data[i]);
    da_free__Z3_ast(&args, NULL);
    Z3_dec_ref(ctx->z3_ctx, query_no_branch);

    opt_found = res_opt;
    return res;
}

int z3fuzz_query_check_light(fuzzy_ctx_t* ctx, Z3_ast query,
                             Z3_ast                branch_condition,
                             unsigned char const** proof,
                             unsigned long*        proof_size)
{
    Z3_inc_ref(ctx->z3_ctx, query);
    Z3_inc_ref(ctx->z3_ctx, branch_condition);

#ifdef DEBUG_CHECK_LIGHT
    Z3FUZZ_LOG("Called z3fuzz_query_check_light\n");
    z3fuzz_print_expr(ctx, branch_condition);
#endif

    int res;
    *proof_size = 0;

    timer_start_wrapper(ctx);
    g_prev_num_evaluate = ctx->stats.num_evaluate;

    __init_global_data(ctx, query, branch_condition);

    int with_not;
    if (is_and_constraint(ctx, branch_condition, &with_not))
        res = handle_and_constraint(ctx, query, branch_condition, proof,
                                    proof_size);
    else
        res = query_check_light_and_multigoal(ctx, query, branch_condition,
                                              proof, proof_size);

    if (opt_found)
        ctx->stats.opt_sat += 1;

    Z3_dec_ref(ctx->z3_ctx, query);
    Z3_dec_ref(ctx->z3_ctx, branch_condition);
    return res;
}

void z3fuzz_add_assignment(fuzzy_ctx_t* ctx, int idx, Z3_ast assignment_value)
{
    if (idx >= ctx->size_assignments) {
        unsigned old_size     = ctx->size_assignments;
        ctx->size_assignments = (idx + 1) * 3 / 2;
        ctx->assignments      = (Z3_ast*)realloc(
            ctx->assignments, sizeof(Z3_ast) * ctx->size_assignments);
        ASSERT_OR_ABORT(
            ctx->assignments != NULL,
            "z3fuzz_add_assignment() ctx->assignments - failed realloc");

        // set to zero the new memory
        memset(ctx->assignments + old_size, 0,
               ctx->size_assignments - old_size);
    }
    Z3_inc_ref(ctx->z3_ctx, assignment_value);
    ctx->assignments[idx] = assignment_value;

    // im assuming that assignment_value is a BV
    unsigned char assignment_size = Z3_get_bv_sort_size(
        ctx->z3_ctx, Z3_get_sort(ctx->z3_ctx, assignment_value));

    unsigned old_len = ctx->testcases.data[0].values_len;

    testcase_t* testcase;
    unsigned    i;
    for (i = 0; i < ctx->testcases.size; ++i) {
        testcase = &ctx->testcases.data[i];

        if (testcase->values_len <= idx) {
            testcase->values_len = (idx + 1) * 3 / 2;
            testcase->values     = (unsigned long*)realloc(
                testcase->values, sizeof(unsigned long) * testcase->values_len);
            ASSERT_OR_ABORT(
                testcase->values != 0,
                "z3fuzz_add_assignment() testcase->values - failed realloc");
            testcase->value_sizes = (unsigned char*)realloc(
                testcase->value_sizes,
                sizeof(unsigned char) * testcase->values_len);
            ASSERT_OR_ABORT(
                testcase->value_sizes != 0,
                "z3fuzz_add_assignment() testcase->value_sizes - failed "
                "realloc");
            testcase->z3_values = (Z3_ast*)realloc(
                testcase->z3_values, sizeof(Z3_ast) * testcase->values_len);
            ASSERT_OR_ABORT(
                testcase->z3_values != 0,
                "z3fuzz_add_assignment() testcase->z3_values - failed realloc");
            memset(testcase->z3_values + old_len, 0,
                   testcase->values_len - old_len);
        }

        unsigned long assignment_value_concrete =
            ctx->model_eval(ctx->z3_ctx, assignment_value, testcase->values,
                            testcase->value_sizes, testcase->values_len, NULL);

        testcase->value_sizes[idx] = assignment_size;
        testcase->values[idx]      = assignment_value_concrete;
        testcase->z3_values[idx] =
            Z3_mk_unsigned_int(ctx->z3_ctx, assignment_value_concrete,
                               Z3_mk_bv_sort(ctx->z3_ctx, assignment_size));
        Z3_inc_ref(ctx->z3_ctx, testcase->z3_values[idx]);

        testcase->values_len =
            (testcase->values_len > idx + 1) ? testcase->values_len : idx + 1;
    }

    if (old_len < ctx->testcases.data[0].values_len) {
        init_global_context(ctx->testcases.data[0].values_len);
    }
}

static int compare_ulong(const void* v1, const void* v2)
{
    return *(unsigned long*)v1 - *(unsigned long*)v2;
}

static inline unsigned long __minimize_maximize_inner_greedy(
    fuzzy_ctx_t* ctx, Z3_ast pi, Z3_ast to_maximize_minimize,
    unsigned char const** out_values, unsigned is_max)
{
    __reset_ast_data();
    detect_involved_inputs_wrapper(ctx, to_maximize_minimize, &ast_data.inputs);

    testcase_t*   current_testcase = &ctx->testcases.data[0];
    unsigned long max_min          = ctx->model_eval(
        ctx->z3_ctx, to_maximize_minimize, tmp_input,
        current_testcase->value_sizes, current_testcase->values_len, NULL);
    unsigned long tmp;
    unsigned long original_byte, max_min_byte, i, j;
    ulong*        p;
    unsigned long num_indexes = ast_data.inputs->indexes.size;
    unsigned long indexes_array[num_indexes];

    i = 0;
    set_reset_iter__ulong(&ast_data.inputs->indexes, 0);
    while (set_iter_next__ulong(&ast_data.inputs->indexes, 0, &p))
        indexes_array[i++] = *p;
    qsort(indexes_array, num_indexes, sizeof(unsigned long), compare_ulong);

    for (j = 0; j < num_indexes; ++j) {
        p             = &indexes_array[j];
        original_byte = current_testcase->values[*p];
        max_min_byte  = current_testcase->values[*p];

        for (i = 0; i < 256; ++i) {
            if (i == original_byte)
                continue;

            tmp_input[*p] = (unsigned long)i;
            if (!ctx->model_eval(ctx->z3_ctx, pi, tmp_input,
                                 current_testcase->value_sizes,
                                 current_testcase->values_len, NULL))
                continue;

            tmp = ctx->model_eval(ctx->z3_ctx, to_maximize_minimize, tmp_input,
                                  current_testcase->value_sizes,
                                  current_testcase->values_len, NULL);
            if ((is_max && tmp > max_min) || (!is_max && tmp < max_min)) {
                max_min_byte = i;
                max_min      = tmp;
            }
        }
        tmp_input[*p] = (unsigned long)max_min_byte;
    }

    __vals_long_to_char(tmp_input, tmp_proof, current_testcase->testcase_len);
    *out_values = tmp_proof;
    return max_min;
}

unsigned long z3fuzz_maximize(fuzzy_ctx_t* ctx, Z3_ast pi, Z3_ast to_maximize,
                              unsigned char const** out_values,
                              unsigned long*        out_len)
{
    Z3_inc_ref(ctx->z3_ctx, pi);

    memcpy(tmp_input, ctx->testcases.data[0].values,
           ctx->testcases.data[0].values_len * sizeof(unsigned long));

    *out_len = ctx->testcases.data[0].testcase_len;
    if (use_greedy_mamin)
        return __minimize_maximize_inner_greedy(ctx, pi, to_maximize,
                                                out_values, 1);
    testcase_t* current_testcase = &ctx->testcases.data[0];
    // // detect the strcmp pattern
    // if (__detect_strcmp_pattern(ctx, to_maximize, tmp_input)) {
    //     __vals_long_to_char(tmp_input, tmp_proof, *out_len);
    //     *out_values       = tmp_proof;
    //     unsigned long res = Z3_custom_eval(ctx->z3_ctx, to_maximize,
    //     tmp_input,
    //                                        current_testcase->value_sizes,
    //                                        current_testcase->values_len);
    //     return res;
    // }

    Z3_ast  original_to_maximize = to_maximize;
    Z3_sort arg_sort             = Z3_get_sort(ctx->z3_ctx, to_maximize);
    ASSERT_OR_ABORT(Z3_get_sort_kind(ctx->z3_ctx, arg_sort) == Z3_BV_SORT,
                    "z3fuzz_minimize requires a BV sort");
    unsigned sort_size = Z3_get_bv_sort_size(ctx->z3_ctx, arg_sort);
    ASSERT_OR_ABORT(sort_size > 1, "z3fuzz_minimize unexpected sort size");

    Z3_inc_ref(ctx->z3_ctx, original_to_maximize);
    if (sort_size < 64) {
        to_maximize = Z3_mk_zero_ext(ctx->z3_ctx, 64 - sort_size, to_maximize);
        sort_size   = 64;
    }
    to_maximize = Z3_mk_bvneg(ctx->z3_ctx, to_maximize);
    Z3_inc_ref(ctx->z3_ctx, to_maximize);

    unsigned long     res;
    eval_wapper_ctx_t ew;

    int valid_eval = __gd_init_eval(ctx, pi, to_maximize, 1, 1, &ew);
    if (!valid_eval) {
        // all inputs are fixed
        res = ctx->model_eval(ctx->z3_ctx, original_to_maximize, tmp_input,
                              current_testcase->value_sizes,
                              current_testcase->values_len, NULL);
        __vals_long_to_char(tmp_input, tmp_proof,
                            current_testcase->testcase_len);
        *out_values = tmp_proof;
        goto OUT;
    }

    eval_set_ctx(&ew);

    timer_start_wrapper(ctx);
    unsigned long max_val;
    int           gd_exit =
        gd_minimize(__gd_eval, ew.input, ew.input, &max_val, ew.mapping_size);
    if (unlikely(gd_exit == TIMEOUT_V)) {
        res = ctx->model_eval(ctx->z3_ctx, original_to_maximize, tmp_input,
                              current_testcase->value_sizes,
                              current_testcase->values_len, NULL);
        __vals_long_to_char(tmp_input, tmp_proof,
                            current_testcase->testcase_len);
        *out_values = tmp_proof;
        goto OUT;
    }

    __gd_fix_tmp_input(ew.input);
    res = ctx->model_eval(ctx->z3_ctx, original_to_maximize, tmp_input,
                          current_testcase->value_sizes,
                          current_testcase->values_len, NULL);
    __vals_long_to_char(tmp_input, tmp_proof, *out_len);
    *out_values = tmp_proof;

OUT:
    Z3_dec_ref(ctx->z3_ctx, pi);
    Z3_dec_ref(ctx->z3_ctx, to_maximize);
    Z3_dec_ref(ctx->z3_ctx, original_to_maximize);
    __gd_free_eval(&ew);
    memcpy(tmp_input, current_testcase->values,
           current_testcase->values_len * sizeof(unsigned long));
    return res;
}

unsigned long z3fuzz_minimize(fuzzy_ctx_t* ctx, Z3_ast pi, Z3_ast to_minimize,
                              unsigned char const** out_values,
                              unsigned long*        out_len)
{
    Z3_inc_ref(ctx->z3_ctx, pi);
    memcpy(tmp_input, ctx->testcases.data[0].values,
           ctx->testcases.data[0].values_len * sizeof(unsigned long));

    *out_len = ctx->testcases.data[0].testcase_len;
    if (use_greedy_mamin)
        return __minimize_maximize_inner_greedy(ctx, pi, to_minimize,
                                                out_values, 0);
    testcase_t* current_testcase = &ctx->testcases.data[0];

    Z3_sort arg_sort = Z3_get_sort(ctx->z3_ctx, to_minimize);
    ASSERT_OR_ABORT(Z3_get_sort_kind(ctx->z3_ctx, arg_sort) == Z3_BV_SORT,
                    "z3fuzz_minimize requires a BV sort");
    unsigned sort_size = Z3_get_bv_sort_size(ctx->z3_ctx, arg_sort);
    ASSERT_OR_ABORT(sort_size > 1, "z3fuzz_minimize unexpected sort size");

    Z3_ast to_minimize_original = to_minimize;
    Z3_inc_ref(ctx->z3_ctx, to_minimize_original);
    if (sort_size < 64) {
        to_minimize = Z3_mk_zero_ext(ctx->z3_ctx, 64 - sort_size, to_minimize);
        sort_size   = 64;
    }
    Z3_inc_ref(ctx->z3_ctx, to_minimize);

    eval_wapper_ctx_t ew;
    int valid_eval = __gd_init_eval(ctx, pi, to_minimize, 1, 1, &ew);
    if (!valid_eval) {
        // all inputs are fixed
        unsigned long res = ctx->model_eval(ctx->z3_ctx, to_minimize, tmp_input,
                                            current_testcase->value_sizes,
                                            current_testcase->values_len, NULL);
        __vals_long_to_char(tmp_input, tmp_proof,
                            current_testcase->testcase_len);
        *out_values = tmp_proof;
        return res;
    }
    eval_set_ctx(&ew);

    timer_start_wrapper(ctx);
    unsigned long res;
    unsigned long min_val;
    int           gd_exit =
        gd_minimize(__gd_eval, ew.input, ew.input, &min_val, ew.mapping_size);
    if (unlikely(gd_exit == TIMEOUT_V)) {
        res = ctx->model_eval(ctx->z3_ctx, to_minimize, tmp_input,
                              current_testcase->value_sizes,
                              current_testcase->values_len, NULL);
        __vals_long_to_char(tmp_input, tmp_proof,
                            current_testcase->testcase_len);
        *out_values = tmp_proof;
        goto OUT;
    }

    __gd_fix_tmp_input(ew.input);
    res = ctx->model_eval(ctx->z3_ctx, to_minimize_original, tmp_input,
                          current_testcase->value_sizes,
                          current_testcase->values_len, NULL);
    __vals_long_to_char(tmp_input, tmp_proof, *out_len);
    *out_values = tmp_proof;
OUT:
    Z3_dec_ref(ctx->z3_ctx, pi);
    Z3_dec_ref(ctx->z3_ctx, to_minimize);
    Z3_dec_ref(ctx->z3_ctx, to_minimize_original);
    __gd_free_eval(&ew);
    return res;
}

void z3fuzz_find_all_values(fuzzy_ctx_t* ctx, Z3_ast expr, Z3_ast pi,
                            fuzzy_findall_res_t (*callback)(
                                unsigned char const* out_bytes,
                                unsigned long out_bytes_len, unsigned long val))
{
    Z3_inc_ref(ctx->z3_ctx, pi);
    Z3_inc_ref(ctx->z3_ctx, expr);

    testcase_t* current_testcase = &ctx->testcases.data[0];
    memcpy(tmp_input, current_testcase->values,
           current_testcase->values_len * sizeof(unsigned long));
    __reset_ast_data();
    detect_involved_inputs_wrapper(ctx, expr, &ast_data.inputs);

    set__ulong output_vals;
    set_init__ulong(&output_vals, index_hash, index_equals);

    // Perform the first evaluation in the seed
    __vals_long_to_char(tmp_input, tmp_proof, current_testcase->testcase_len);
    unsigned long value_in_seed = ctx->model_eval(
        ctx->z3_ctx, expr, tmp_input, current_testcase->value_sizes,
        current_testcase->values_len, NULL);
    fuzzy_findall_res_t res_seed_call =
        callback(tmp_proof, current_testcase->testcase_len, value_in_seed);
    if (res_seed_call == Z3FUZZ_STOP)
        goto END;

    index_group_t* g;

    set_reset_iter__index_group_t(&ast_data.inputs->index_groups, 1);
    while (
        set_iter_next__index_group_t(&ast_data.inputs->index_groups, 1, &g)) {

        unsigned long original_val =
            index_group_to_value(g, current_testcase->values);

        set__interval_group_ptr* group_intervals =
            (set__interval_group_ptr*)ctx->group_intervals;
        wrapped_interval_t* interval =
            interval_group_get_interval(group_intervals, g);

        if (interval != NULL && wi_get_range(interval) < 256) {
            // the group is within a (small) known interval, brute force it
            wrapped_interval_iter_t it = wi_init_iter_values(interval);
            uint64_t                val;
            while (wi_iter_get_next(&it, &val)) {
                set_tmp_input_group_to_value(g, val);
                if (ctx->model_eval(ctx->z3_ctx, pi, tmp_input,
                                    current_testcase->value_sizes,
                                    current_testcase->values_len, NULL)) {
                    __vals_long_to_char(tmp_input, tmp_proof,
                                        current_testcase->testcase_len);
                    unsigned long expr_val =
                        ctx->model_eval(ctx->z3_ctx, expr, tmp_input,
                                        current_testcase->value_sizes,
                                        current_testcase->values_len, NULL);
                    fuzzy_findall_res_t res = callback(
                        tmp_proof, current_testcase->testcase_len, expr_val);
                    if (res == Z3FUZZ_STOP)
                        goto END;
                }
            }
        } else if (g->n == 1) {
            // it is a single byte, brute-force it
            uint64_t i;
            for (i = 0; i < 256; ++i) {
                set_tmp_input_group_to_value(g, i);
                if (ctx->model_eval(ctx->z3_ctx, pi, tmp_input,
                                    current_testcase->value_sizes,
                                    current_testcase->values_len, NULL)) {
                    __vals_long_to_char(tmp_input, tmp_proof,
                                        current_testcase->testcase_len);
                    unsigned long expr_val =
                        ctx->model_eval(ctx->z3_ctx, expr, tmp_input,
                                        current_testcase->value_sizes,
                                        current_testcase->values_len, NULL);
                    fuzzy_findall_res_t res = callback(
                        tmp_proof, current_testcase->testcase_len, expr_val);
                    if (res == Z3FUZZ_STOP)
                        goto END;
                }
            }
        } else {
            // greedy +1, -1
            unsigned      max_iter = 5;
            unsigned      i        = 0;
            unsigned long val      = original_val + 1;

            // sum value
            set_tmp_input_group_to_value(g, val);
            while (i++ < max_iter &&
                   ctx->model_eval(ctx->z3_ctx, pi, tmp_input,
                                   current_testcase->value_sizes,
                                   current_testcase->values_len, NULL)) {
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                unsigned long expr_val = ctx->model_eval(
                    ctx->z3_ctx, expr, tmp_input, current_testcase->value_sizes,
                    current_testcase->values_len, NULL);
                if (set_check__ulong(&output_vals, expr_val))
                    continue;
                set_add__ulong(&output_vals, expr_val);

                fuzzy_findall_res_t res = callback(
                    tmp_proof, current_testcase->testcase_len, expr_val);
                if (res == Z3FUZZ_STOP)
                    goto END;
                val += 1;
                set_tmp_input_group_to_value(g, val);
            }

            val = original_val - 1;
            i   = 0;

            // subtract value
            set_tmp_input_group_to_value(g, val);
            while (i++ < max_iter &&
                   ctx->model_eval(ctx->z3_ctx, pi, tmp_input,
                                   current_testcase->value_sizes,
                                   current_testcase->values_len, NULL)) {
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                unsigned long expr_val = ctx->model_eval(
                    ctx->z3_ctx, expr, tmp_input, current_testcase->value_sizes,
                    current_testcase->values_len, NULL);
                if (set_check__ulong(&output_vals, expr_val))
                    continue;
                set_add__ulong(&output_vals, expr_val);

                fuzzy_findall_res_t res = callback(
                    tmp_proof, current_testcase->testcase_len, expr_val);
                if (res == Z3FUZZ_STOP)
                    goto END;
                val -= 1;
                set_tmp_input_group_to_value(g, val);
            }

            // sum and subtract single byte
            int j;
            for (j = 0; j < g->n - 1; ++j) {
                unsigned long original_val = tmp_input[g->indexes[j]];
                unsigned long byte_val     = original_val + 1;
                tmp_input[g->indexes[j]]   = byte_val;
                i                          = 0;
                while (i++ < max_iter &&
                       ctx->model_eval(ctx->z3_ctx, pi, tmp_input,
                                       current_testcase->value_sizes,
                                       current_testcase->values_len, NULL)) {
                    __vals_long_to_char(tmp_input, tmp_proof,
                                        current_testcase->testcase_len);
                    unsigned long expr_val =
                        ctx->model_eval(ctx->z3_ctx, expr, tmp_input,
                                        current_testcase->value_sizes,
                                        current_testcase->values_len, NULL);
                    if (set_check__ulong(&output_vals, expr_val))
                        continue;
                    set_add__ulong(&output_vals, expr_val);

                    fuzzy_findall_res_t res = callback(
                        tmp_proof, current_testcase->testcase_len, expr_val);
                    if (res == Z3FUZZ_STOP)
                        goto END;
                    byte_val += 1;
                    tmp_input[g->indexes[j]] = byte_val;
                }

                tmp_input[g->indexes[j]] = original_val;
                byte_val                 = original_val - 1;
                tmp_input[g->indexes[j]] = byte_val;
                i                        = 0;
                while (i++ < max_iter &&
                       ctx->model_eval(ctx->z3_ctx, pi, tmp_input,
                                       current_testcase->value_sizes,
                                       current_testcase->values_len, NULL)) {
                    __vals_long_to_char(tmp_input, tmp_proof,
                                        current_testcase->testcase_len);
                    unsigned long expr_val =
                        ctx->model_eval(ctx->z3_ctx, expr, tmp_input,
                                        current_testcase->value_sizes,
                                        current_testcase->values_len, NULL);
                    if (set_check__ulong(&output_vals, expr_val))
                        continue;
                    set_add__ulong(&output_vals, expr_val);

                    fuzzy_findall_res_t res = callback(
                        tmp_proof, current_testcase->testcase_len, expr_val);
                    if (res == Z3FUZZ_STOP)
                        goto END;
                    byte_val -= 1;
                    tmp_input[g->indexes[j]] = byte_val;
                }
            }

            // set deterministic
            for (j = 0; j < g->n; ++j)
                tmp_input[g->indexes[j]] = 0;
            if (ctx->model_eval(ctx->z3_ctx, pi, tmp_input,
                                current_testcase->value_sizes,
                                current_testcase->values_len, NULL)) {
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                unsigned long expr_val = ctx->model_eval(
                    ctx->z3_ctx, expr, tmp_input, current_testcase->value_sizes,
                    current_testcase->values_len, NULL);
                if (set_check__ulong(&output_vals, expr_val))
                    continue;
                set_add__ulong(&output_vals, expr_val);

                fuzzy_findall_res_t res = callback(
                    tmp_proof, current_testcase->testcase_len, expr_val);
                if (res == Z3FUZZ_STOP)
                    goto END;
            }
            for (j = 0; j < g->n; ++j)
                tmp_input[g->indexes[j]] = 0xff;
            if (ctx->model_eval(ctx->z3_ctx, pi, tmp_input,
                                current_testcase->value_sizes,
                                current_testcase->values_len, NULL)) {
                __vals_long_to_char(tmp_input, tmp_proof,
                                    current_testcase->testcase_len);
                unsigned long expr_val = ctx->model_eval(
                    ctx->z3_ctx, expr, tmp_input, current_testcase->value_sizes,
                    current_testcase->values_len, NULL);
                if (set_check__ulong(&output_vals, expr_val))
                    continue;
                set_add__ulong(&output_vals, expr_val);

                fuzzy_findall_res_t res = callback(
                    tmp_proof, current_testcase->testcase_len, expr_val);
                if (res == Z3FUZZ_STOP)
                    goto END;
            }
        }
        set_tmp_input_group_to_value(g, original_val);
    }

END:
    set_free__ulong(&output_vals, NULL);
    Z3_dec_ref(ctx->z3_ctx, pi);
    Z3_dec_ref(ctx->z3_ctx, expr);
}

void z3fuzz_find_all_values_gd(
    fuzzy_ctx_t* ctx, Z3_ast expr, Z3_ast pi, int to_min,
    fuzzy_findall_res_t (*callback)(unsigned char const* out_bytes,
                                    unsigned long        out_bytes_len,
                                    unsigned long        val))
{
    Z3_inc_ref(ctx->z3_ctx, expr);
    Z3_inc_ref(ctx->z3_ctx, pi);

    testcase_t* current_testcase = &ctx->testcases.data[0];
    Z3_ast      expr_original    = expr;
    Z3_sort     arg_sort         = Z3_get_sort(ctx->z3_ctx, expr);
    ASSERT_OR_ABORT(Z3_get_sort_kind(ctx->z3_ctx, arg_sort) == Z3_BV_SORT,
                    "z3fuzz_find_all_values_gd requires a BV sort");
    unsigned sort_size = Z3_get_bv_sort_size(ctx->z3_ctx, arg_sort);
    ASSERT_OR_ABORT(sort_size > 1,
                    "z3fuzz_find_all_values_gd unexpected sort size");

    if (sort_size < 64) {
        expr      = Z3_mk_sign_ext(ctx->z3_ctx, 64 - sort_size, expr);
        sort_size = 64;
    }
    if (!to_min)
        expr = Z3_mk_bvneg(ctx->z3_ctx, expr);
    Z3_inc_ref(ctx->z3_ctx, expr);

    eval_wapper_ctx_t ew;
    if (!__gd_init_eval(ctx, pi, expr, 1, 1, &ew))
        // all inputs are fixed
        goto OUT_2;

    eval_set_ctx(&ew);
    timer_start_wrapper(ctx);

    uint64_t max_grad;
    if (!gd_max_gradient(__gd_eval, ew.input, ew.mapping_size, &max_grad))
        goto OUT_2;

    if (max_grad == 0) {
        int i;
        for (i = 0; i < ew.mapping_size; ++i)
            ew.input[i] = 0;
        if (!gd_max_gradient(__gd_eval, ew.input, ew.mapping_size, &max_grad))
            goto OUT_2;
    }
    if (max_grad == 0) {
        int i;
        for (i = 0; i < ew.mapping_size; ++i)
            ew.input[i] = 0xff;
        if (!gd_max_gradient(__gd_eval, ew.input, ew.mapping_size, &max_grad))
            goto OUT_2;
    }

    set__digest_t digest_set;
    set_init__digest_t(&digest_set, digest_64bit_hash, digest_equals);

    unsigned long last_val;
    int           no_callback = 0;
    int           gd_ret;
    int           at_least_once = 0;
    uint64_t      val;
    while (
        ((gd_ret = gd_descend_transf(__gd_eval, ew.input, ew.input, &val,
                                     ew.mapping_size)) == 0) &&
        (__check_or_add_digest(&digest_set, (unsigned char*)ew.input,
                               ew.mapping_size * sizeof(unsigned long)) == 0)) {

        __gd_fix_tmp_input(ew.input);
        if (!ctx->model_eval(ctx->z3_ctx, pi, tmp_input,
                             current_testcase->value_sizes,
                             current_testcase->values_len, NULL))
            continue;

        at_least_once = 1;
        last_val      = ctx->model_eval(ctx->z3_ctx, expr_original, tmp_input,
                                   current_testcase->value_sizes,
                                   current_testcase->values_len, NULL);
        __vals_long_to_char(tmp_input, tmp_proof,
                            current_testcase->testcase_len);

        if (no_callback)
            continue;

        fuzzy_findall_res_t res =
            callback(tmp_proof, current_testcase->testcase_len, last_val);
        if (res == Z3FUZZ_STOP)
            goto OUT_1;
        else if (res == Z3FUZZ_JUST_LAST)
            no_callback = 1;
    }

    if (at_least_once)
        callback(tmp_proof, current_testcase->testcase_len, last_val);

OUT_1:
    set_free__digest_t(&digest_set, NULL);
OUT_2:
    Z3_dec_ref(ctx->z3_ctx, pi);
    Z3_dec_ref(ctx->z3_ctx, expr);
    Z3_dec_ref(ctx->z3_ctx, expr_original);
    __gd_free_eval(&ew);
    return;
}

void z3fuzz_notify_constraint(fuzzy_ctx_t* ctx, Z3_ast constraint)
{
    // this is a visit of the AST of the constraint... Too slow? I don't know
    if (unlikely(skip_notify))
        return;

#ifdef DEBUG_CHECK_LIGHT
    Z3FUZZ_LOG("Called z3fuzz_notify_constraint\n");
#endif

    if (unlikely(notify_count++ & 16)) {
        notify_count = 0;
        dict__ast_info_ptr* ast_info_cache =
            (dict__ast_info_ptr*)ctx->ast_info_cache;
        if (unlikely(ast_info_cache->size > max_ast_info_cache_size))
            dict_remove_all__ast_info_ptr(ast_info_cache);
    }

    unsigned long hash                = Z3_UNIQUE(ctx->z3_ctx, constraint);
    set__ulong* processed_constraints = (set__ulong*)ctx->processed_constraints;
    if (set_check__ulong(processed_constraints, hash))
        return;
    set_add__ulong(processed_constraints, hash);

    Z3_inc_ref(ctx->z3_ctx, constraint);

    int with_not;
    if (is_and_constraint(ctx, constraint, &with_not)) {
        da__Z3_ast args;
        da_init__Z3_ast(&args);
        flatten_and_args(ctx, constraint, &args);

        unsigned i;
        for (i = 0; i < args.size; ++i) {
            z3fuzz_notify_constraint(ctx, args.data[i]);
            Z3_dec_ref(ctx->z3_ctx, args.data[i]);
        }
        da_free__Z3_ast(&args, NULL);

        Z3_dec_ref(ctx->z3_ctx, constraint);
        return;
    }

    if (__check_univocally_defined(ctx, constraint)) {
        ctx->stats.num_univocally_defined++;

        // invalidate ast_info_cache
        dict__ast_info_ptr* ast_info_cache =
            (dict__ast_info_ptr*)ctx->ast_info_cache;
        dict_remove_all__ast_info_ptr(ast_info_cache);
    } else {
        ctx->stats.num_conflicting +=
            __check_conflicting_constraint(ctx, constraint);

        ctx->stats.num_range_constraints +=
            __check_range_constraint(ctx, constraint);
    }

    Z3_dec_ref(ctx->z3_ctx, constraint);
}

int z3fuzz_get_optimistic_sol(fuzzy_ctx_t* ctx, unsigned char const** proof,
                              unsigned long* proof_size)
{
    if (opt_found) {
        testcase_t* t = &ctx->testcases.data[0];
        *proof_size   = t->testcase_len;
        *proof        = tmp_opt_proof;
    }
    return opt_found;
}

void z3fuzz_dump_proof(fuzzy_ctx_t* ctx, const char* filename,
                       unsigned char const* proof, unsigned long proof_size)
{
    FILE* fp = fopen(filename, "w");
    ASSERT_OR_ABORT(fp != NULL, "z3fuzz_dump_proof() open failed");

    // Z3FUZZ_LOG("dumping proof in %s\n", filename);

    unsigned long i;
    for (i = 0; i < proof_size; i++) {
        fwrite(&proof[i], sizeof(char), 1, fp);
    }
    fclose(fp);
}

unsigned long z3fuzz_evaluate_expression(fuzzy_ctx_t* ctx, Z3_ast value,
                                         unsigned char* values)
{
    __vals_char_to_long(values, tmp_input, ctx->testcases.data[0].values_len);

    unsigned long res = ctx->model_eval(
        ctx->z3_ctx, value, tmp_input, ctx->testcases.data[0].value_sizes,
        ctx->testcases.data[0].values_len, NULL);
    return res;
}

unsigned long z3fuzz_evaluate_expression_z3(fuzzy_ctx_t* ctx, Z3_ast query,
                                            Z3_ast* values)
{
    // evaluate query using [input <- input_val] as interpretation

    // build a model and assign an interpretation for the input symbols
    unsigned long res;
    Z3_model      z3_m = Z3_mk_model(ctx->z3_ctx);
    Z3_model_inc_ref(ctx->z3_ctx, z3_m);
    testcase_t* current_testcase = &ctx->testcases.data[0];

    unsigned i;
    for (i = 0; i < current_testcase->values_len; ++i) {
        unsigned int index = i;
        Z3_sort      sort =
            Z3_mk_bv_sort(ctx->z3_ctx, current_testcase->value_sizes[i]);
        Z3_symbol    s    = Z3_mk_int_symbol(ctx->z3_ctx, index);
        Z3_func_decl decl = Z3_mk_func_decl(ctx->z3_ctx, s, 0, NULL, sort);
        Z3_add_const_interp(ctx->z3_ctx, z3_m, decl, values[index]);
    }

    // evaluate the query in the model
    Z3_ast  solution;
    Z3_bool successfulEval =
        Z3_model_eval(ctx->z3_ctx, z3_m, query, Z3_TRUE, &solution);
    ASSERT_OR_ABORT(successfulEval, "Failed to evaluate model");

    Z3_model_dec_ref(ctx->z3_ctx, z3_m);
    if (Z3_get_ast_kind(ctx->z3_ctx, solution) == Z3_NUMERAL_AST) {
        Z3_bool successGet = Z3_get_numeral_uint64(ctx->z3_ctx, solution, &res);
        ASSERT_OR_ABORT(successGet == Z3_TRUE,
                        "z3fuzz_evaluate_expression_z3() failed to get "
                        "constant");
    } else
        res = Z3_get_bool_value(ctx->z3_ctx, solution) == Z3_L_TRUE ? 1UL : 0UL;
    Z3_dec_ref(ctx->z3_ctx, solution);
    return res;
}

void z3fuzz_get_mem_stats(fuzzy_ctx_t* ctx, memory_impact_stats_t* stats)
{
    stats->univocally_defined_size =
        ((set__ulong*)ctx->univocally_defined_inputs)->size;
    stats->ast_info_cache_size =
        ((dict__ast_info_ptr*)ctx->ast_info_cache)->size;
    stats->conflicting_ast_size =
        ((dict__conflicting_ptr*)ctx->conflicting_asts)->size;
    stats->group_intervals_size =
        ((set__interval_group_ptr*)ctx->group_intervals)->size;
    stats->index_to_group_intervals_size =
        ((dict__da__interval_group_ptr*)ctx->index_to_group_intervals)->size;
    stats->n_assignments = (unsigned long)ctx->size_assignments;
}
