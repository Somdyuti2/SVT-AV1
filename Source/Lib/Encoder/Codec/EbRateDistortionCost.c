/*
* Copyright(c) 2019 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

/*
* Copyright (c) 2016, Alliance for Open Media. All rights reserved
*
* This source code is subject to the terms of the BSD 2 Clause License and
* the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at www.aomedia.org/license/software. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at www.aomedia.org/license/patent.
*/

/***************************************
* Includes
***************************************/
#include "EbRateDistortionCost.h"
#include "EbCommonUtils.h"
#include "aom_dsp_rtcd.h"
#include "EbLog.h"

#include <assert.h>
#define FIRST_PASS_COST_PENALTY 20 // The penalty is added in cost calculation of the first pass.
#define AV1_COST_PRECISION 0
#define MV_COST_WEIGHT 108
int av1_get_reference_mode_context_new(const MacroBlockD *xd);
int eb_av1_get_pred_context_uni_comp_ref_p(const MacroBlockD *xd);
int eb_av1_get_pred_context_uni_comp_ref_p1(const MacroBlockD *xd);
int eb_av1_get_pred_context_uni_comp_ref_p2(const MacroBlockD *xd);
int av1_get_comp_reference_type_context_new(const MacroBlockD *xd);

int  av1_get_palette_bsize_ctx(BlockSize bsize);
int  av1_get_palette_mode_ctx(const MacroBlockD *xd);
int  write_uniform_cost(int n, int v);
int  eb_get_palette_cache(const MacroBlockD *const xd, int plane, uint16_t *cache);
int  av1_palette_color_cost_y(const PaletteModeInfo *const pmi, uint16_t *color_cache, int n_cache,
                              int bit_depth);
int  av1_cost_color_map(PaletteInfo *palette_info, MdRateEstimationContext *rate_table,
                        BlkStruct *blk_ptr, int plane, BlockSize bsize, COLOR_MAP_TYPE type);
void av1_get_block_dimensions(BlockSize bsize, int plane, const MacroBlockD *xd, int *width,
                              int *height, int *rows_within_bounds, int *cols_within_bounds);
int  av1_allow_palette(int allow_screen_content_tools, BlockSize sb_type);
int av1_allow_intrabc(const FrameHeader *frm_hdr, EB_SLICE slice_type);

uint8_t av1_drl_ctx(const CandidateMv *ref_mv_stack, int32_t ref_idx) {
    if (ref_mv_stack[ref_idx].weight >= REF_CAT_LEVEL &&
        ref_mv_stack[ref_idx + 1].weight >= REF_CAT_LEVEL)
        return 0;

    if (ref_mv_stack[ref_idx].weight >= REF_CAT_LEVEL &&
        ref_mv_stack[ref_idx + 1].weight < REF_CAT_LEVEL)
        return 1;

    if (ref_mv_stack[ref_idx].weight < REF_CAT_LEVEL &&
        ref_mv_stack[ref_idx + 1].weight < REF_CAT_LEVEL)
        return 2;

    return 0;
}

/* Symbols for coding which components are zero jointly */
//#define MV_JOINTS 4
//typedef enum {
//    MV_JOINT_ZERO = 0,   /* zero vector */
//    MV_JOINT_HNZVZ = 1,  /* Vert zero, hor nonzero */
//    MV_JOINT_HZVNZ = 2,  /* Hor zero, vert nonzero */
//    MV_JOINT_HNZVNZ = 3, /* Both components nonzero */
//} MvJointType;

MvJointType av1_get_mv_joint(const MV *mv) {
    if (mv->row == 0)
        return mv->col == 0 ? MV_JOINT_ZERO : MV_JOINT_HNZVZ;
    else
        return mv->col == 0 ? MV_JOINT_HZVNZ : MV_JOINT_HNZVNZ;
}
int32_t mv_cost(const MV *mv, const int32_t *joint_cost, int32_t *const comp_cost[2]) {
    int32_t jn_c = av1_get_mv_joint(mv);
    int32_t res  = joint_cost[jn_c] + comp_cost[0][mv->row] + comp_cost[1][mv->col];

    return res;
}

int32_t eb_av1_mv_bit_cost(const MV *mv, const MV *ref, const int32_t *mvjcost, int32_t *mvcost[2],
                           int32_t weight) {
    const MV diff = {mv->row - ref->row, mv->col - ref->col};
    return ROUND_POWER_OF_TWO(mv_cost(&diff, mvjcost, mvcost) * weight, 7);
}

/////////////////////////////COEFFICIENT CALCULATION //////////////////////////////////////////////
static INLINE int32_t get_golomb_cost(int32_t abs_qc) {
    if (abs_qc >= 1 + NUM_BASE_LEVELS + COEFF_BASE_RANGE) {
        const int32_t r      = abs_qc - COEFF_BASE_RANGE - NUM_BASE_LEVELS;
        const int32_t length = get_msb(r) + 1;
        return av1_cost_literal(2 * length - 1);
    }
    return 0;
}

void eb_av1_txb_init_levels_c(const TranLow *const coeff, const int32_t width, const int32_t height,
                              uint8_t *const levels) {
    const int32_t stride = width + TX_PAD_HOR;
    uint8_t *     ls     = levels;

    memset(levels - TX_PAD_TOP * stride, 0, sizeof(*levels) * TX_PAD_TOP * stride);
    memset(levels + stride * height, 0, sizeof(*levels) * (TX_PAD_BOTTOM * stride + TX_PAD_END));

    for (int32_t i = 0; i < height; i++) {
        for (int32_t j = 0; j < width; j++)
            *ls++ = (uint8_t)clamp(abs(coeff[i * width + j]), 0, INT8_MAX);
        for (int32_t j = 0; j < TX_PAD_HOR; j++) *ls++ = 0;
    }
}

// TODO(angiebird): use this function whenever it's possible
int32_t av1_transform_type_rate_estimation(uint8_t allow_update_cdf, FRAME_CONTEXT *fc,
                                           struct ModeDecisionCandidateBuffer *candidate_buffer_ptr,
                                           EbBool is_inter, TxSize transform_size,
                                           TxType transform_type, EbBool reduced_tx_set_used) {
    //const MbModeInfo *mbmi = &xd->mi[0]->mbmi;
    //const int32_t is_inter = is_inter_block(mbmi);

    if (get_ext_tx_types(transform_size, is_inter, reduced_tx_set_used) >
        1 /*&&    !xd->lossless[xd->mi[0]->mbmi.segment_id]  WE ARE NOT LOSSLESS*/) {
        const TxSize square_tx_size = txsize_sqr_map[transform_size];
        assert(square_tx_size < EXT_TX_SIZES);

        const int32_t ext_tx_set = get_ext_tx_set(transform_size, is_inter, reduced_tx_set_used);
        if (is_inter) {
            if (ext_tx_set > 0) {
                if (allow_update_cdf) {
                    const TxSetType tx_set_type =
                        get_ext_tx_set_type(transform_size, is_inter, reduced_tx_set_used);

                    update_cdf(fc->inter_ext_tx_cdf[ext_tx_set][square_tx_size],
                               av1_ext_tx_ind[tx_set_type][transform_type],
                               av1_num_ext_tx_set[tx_set_type]);
                }
                return candidate_buffer_ptr->candidate_ptr->md_rate_estimation_ptr
                    ->inter_tx_type_fac_bits[ext_tx_set][square_tx_size][transform_type];
            }
        } else {
            if (ext_tx_set > 0) {
                PredictionMode intra_dir;
                if (candidate_buffer_ptr->candidate_ptr->filter_intra_mode != FILTER_INTRA_MODES)
                    intra_dir =
                        fimode_to_intradir[candidate_buffer_ptr->candidate_ptr->filter_intra_mode];
                else
                    intra_dir = candidate_buffer_ptr->candidate_ptr->pred_mode;
                assert(intra_dir < INTRA_MODES);
                const TxSetType tx_set_type =
                    get_ext_tx_set_type(transform_size, is_inter, reduced_tx_set_used);

                if (allow_update_cdf) {
                    update_cdf(fc->intra_ext_tx_cdf[ext_tx_set][square_tx_size][intra_dir],
                               av1_ext_tx_ind[tx_set_type][transform_type],
                               av1_num_ext_tx_set[tx_set_type]);
                }
                return candidate_buffer_ptr->candidate_ptr->md_rate_estimation_ptr
                    ->intra_tx_type_fac_bits[ext_tx_set][square_tx_size][intra_dir][transform_type];
            }
        }
    }
    return 0;
}

static const int8_t eob_to_pos_small[33] = {
    0, 1, 2, // 0-2
    3, 3, // 3-4
    4, 4, 4, 4, // 5-8
    5, 5, 5, 5, 5, 5, 5, 5, // 9-16
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6 // 17-32
};

static const int8_t eob_to_pos_large[17] = {
    6, // place holder
    7, // 33-64
    8,
    8, // 65-128
    9,
    9,
    9,
    9, // 129-256
    10,
    10,
    10,
    10,
    10,
    10,
    10,
    10, // 257-512
    11 // 513-
};

static INLINE int32_t get_eob_pos_token(const int32_t eob, int32_t *const extra) {
    int32_t t;

    if (eob < 33)
        t = eob_to_pos_small[eob];
    else {
        const int32_t e = AOMMIN((eob - 1) >> 5, 16);
        t               = eob_to_pos_large[e];
    }

    *extra = eob - eb_k_eob_group_start[t];

    return t;
}
#define TX_SIZE TxSize
static INLINE TX_SIZE get_txsize_entropy_ctx(TX_SIZE txsize) {
    return (TX_SIZE)((txsize_sqr_map[txsize] + txsize_sqr_up_map[txsize] + 1) >> 1);
}
void eb_av1_update_eob_context(int eob, TX_SIZE tx_size, TxClass tx_class, PlaneType plane,
                               FRAME_CONTEXT *ec_ctx, uint8_t allow_update_cdf) {
    int       eob_extra;
    const int eob_pt  = get_eob_pos_token(eob, &eob_extra);
    TX_SIZE   txs_ctx = get_txsize_entropy_ctx(tx_size);
    assert(txs_ctx < TX_SIZES);
    const int eob_multi_size = txsize_log2_minus4[tx_size];
    const int eob_multi_ctx  = (tx_class == TX_CLASS_2D) ? 0 : 1;

    switch (eob_multi_size) {
    case 0:
#if CONFIG_ENTROPY_STATS
        ++counts->eob_multi16[cdf_idx][plane][eob_multi_ctx][eob_pt - 1];
#endif
        if (allow_update_cdf)
            update_cdf(ec_ctx->eob_flag_cdf16[plane][eob_multi_ctx], eob_pt - 1, 5);
        break;
    case 1:
#if CONFIG_ENTROPY_STATS
        ++counts->eob_multi32[cdf_idx][plane][eob_multi_ctx][eob_pt - 1];
#endif
        if (allow_update_cdf)
            update_cdf(ec_ctx->eob_flag_cdf32[plane][eob_multi_ctx], eob_pt - 1, 6);
        break;
    case 2:
#if CONFIG_ENTROPY_STATS
        ++counts->eob_multi64[cdf_idx][plane][eob_multi_ctx][eob_pt - 1];
#endif
        if (allow_update_cdf)
            update_cdf(ec_ctx->eob_flag_cdf64[plane][eob_multi_ctx], eob_pt - 1, 7);
        break;
    case 3:
#if CONFIG_ENTROPY_STATS
        ++counts->eob_multi128[cdf_idx][plane][eob_multi_ctx][eob_pt - 1];
#endif
        if (allow_update_cdf) {
            update_cdf(ec_ctx->eob_flag_cdf128[plane][eob_multi_ctx], eob_pt - 1, 8);
        }
        break;
    case 4:
#if CONFIG_ENTROPY_STATS
        ++counts->eob_multi256[cdf_idx][plane][eob_multi_ctx][eob_pt - 1];
#endif
        if (allow_update_cdf) {
            update_cdf(ec_ctx->eob_flag_cdf256[plane][eob_multi_ctx], eob_pt - 1, 9);
        }
        break;
    case 5:
#if CONFIG_ENTROPY_STATS
        ++counts->eob_multi512[cdf_idx][plane][eob_multi_ctx][eob_pt - 1];
#endif
        if (allow_update_cdf) {
            update_cdf(ec_ctx->eob_flag_cdf512[plane][eob_multi_ctx], eob_pt - 1, 10);
        }
        break;
    case 6:
    default:
#if CONFIG_ENTROPY_STATS
        ++counts->eob_multi1024[cdf_idx][plane][eob_multi_ctx][eob_pt - 1];
#endif
        if (allow_update_cdf) {
            update_cdf(ec_ctx->eob_flag_cdf1024[plane][eob_multi_ctx], eob_pt - 1, 11);
        }
        break;
    }

    if (eb_k_eob_offset_bits[eob_pt] > 0) {
        int eob_ctx   = eob_pt - 3;
        int eob_shift = eb_k_eob_offset_bits[eob_pt] - 1;
        int bit       = (eob_extra & (1 << eob_shift)) ? 1 : 0;
#if CONFIG_ENTROPY_STATS
        counts->eob_extra[cdf_idx][txs_ctx][plane][eob_pt][bit]++;
#endif // CONFIG_ENTROPY_STATS
        if (allow_update_cdf) update_cdf(ec_ctx->eob_extra_cdf[txs_ctx][plane][eob_ctx], bit, 2);
    }
}
#if TXS_DEPTH_2
// Transform end of block bit estimation
static int get_eob_cost(int eob, const LvMapEobCost *txb_eob_costs,
    const LvMapCoeffCost *txb_costs, TxClass tx_class) {
    int eob_extra;
    const int eob_pt = get_eob_pos_token(eob, &eob_extra);
    int eob_cost = 0;
    const int eob_multi_ctx = (tx_class == TX_CLASS_2D) ? 0 : 1;
    eob_cost = txb_eob_costs->eob_cost[eob_multi_ctx][eob_pt - 1];

    if (eb_k_eob_offset_bits[eob_pt] > 0) {
        const int eob_ctx = eob_pt - 3;
        const int eob_shift = eb_k_eob_offset_bits[eob_pt] - 1;
        const int bit = (eob_extra & (1 << eob_shift)) ? 1 : 0;
        eob_cost += txb_costs->eob_extra_cost[eob_ctx][bit];
        const int offset_bits = eb_k_eob_offset_bits[eob_pt];
        if (offset_bits > 1) eob_cost += av1_cost_literal(offset_bits - 1);
    }
    return eob_cost;
}
#else
static int32_t get_eob_cost(int32_t eob, const LvMapEobCost *txb_eob_costs,
                            const LvMapCoeffCost *txb_costs, TxType tx_type) {
    int32_t       eob_extra;
    const int32_t eob_pt        = get_eob_pos_token(eob, &eob_extra);
    int32_t       eob_cost      = 0;
    const int32_t eob_multi_ctx = (tx_type_to_class[tx_type] == TX_CLASS_2D) ? 0 : 1;
    eob_cost                    = txb_eob_costs->eob_cost[eob_multi_ctx][eob_pt - 1];

    if (eb_k_eob_offset_bits[eob_pt] > 0) {
        const int32_t eob_shift = eb_k_eob_offset_bits[eob_pt] - 1;
        const int32_t bit       = (eob_extra & (1 << eob_shift)) ? 1 : 0;
        eob_cost += txb_costs->eob_extra_cost[eob_pt][bit];
        const int32_t offset_bits = eb_k_eob_offset_bits[eob_pt];
        if (offset_bits > 1) eob_cost += av1_cost_literal(offset_bits - 1);
    }
    return eob_cost;
}
#endif
static INLINE int32_t av1_cost_skip_txb(uint8_t allow_update_cdf, FRAME_CONTEXT *ec_ctx,
                                        struct ModeDecisionCandidateBuffer *candidate_buffer_ptr,
                                        TxSize transform_size, PlaneType plane_type,
                                        int16_t txb_skip_ctx) {
    const TxSize txs_ctx =
        (TxSize)((txsize_sqr_map[transform_size] + txsize_sqr_up_map[transform_size] + 1) >> 1);
    assert(txs_ctx < TX_SIZES);
    const LvMapCoeffCost *const coeff_costs =
        &candidate_buffer_ptr->candidate_ptr->md_rate_estimation_ptr
             ->coeff_fac_bits[txs_ctx][plane_type];
    if (allow_update_cdf) update_cdf(ec_ctx->txb_skip_cdf[txs_ctx][txb_skip_ctx], 1, 2);
    return coeff_costs->txb_skip_cost[txb_skip_ctx][1];
}

static INLINE int32_t av1_cost_coeffs_txb_loop_cost_eob(uint16_t eob, const int16_t *const scan,
                                                        const TranLow *const  qcoeff,
                                                        int8_t *const         coeff_contexts,
                                                        const LvMapCoeffCost *coeff_costs,
                                                        int16_t dc_sign_ctx, uint8_t *const levels,
                                                        const int32_t bwl, TxType transform_type) {
    const uint32_t cost_literal = av1_cost_literal(1);
    int32_t        cost         = 0;
    int32_t        c;

    /* Loop reduced to touch only first (eob - 1) and last (0) index */
    int32_t decr = eob - 1;
    if (decr < 1) decr = 1;
    for (c = eob - 1; c >= 0; c -= decr) {
        const int32_t pos       = scan[c];
        const TranLow v         = qcoeff[pos];
        const int32_t is_nz     = (v != 0);
        const int32_t level     = abs(v);
        const int32_t coeff_ctx = coeff_contexts[pos];

        if (c == eob - 1) {
            assert((AOMMIN(level, 3) - 1) >= 0);
            cost += coeff_costs->base_eob_cost[coeff_ctx][AOMMIN(level, 3) - 1];
        } else {
            cost += coeff_costs->base_cost[coeff_ctx][AOMMIN(level, 3)];
        }

        if (is_nz) {
            if (c == 0) {
                const int32_t sign = (v < 0) ? 1 : 0;
                // sign bit cost

                cost += coeff_costs->dc_sign_cost[dc_sign_ctx][sign];
            } else {
                cost += cost_literal;
            }

            if (level > NUM_BASE_LEVELS) {
                int32_t ctx;
#if TXS_DEPTH_2
                ctx = get_br_ctx(levels, pos, bwl, tx_type_to_class[transform_type]);
#else
                ctx = get_br_ctx(levels, pos, bwl, transform_type);
#endif
                const int32_t base_range = level - 1 - NUM_BASE_LEVELS;

                if (base_range < COEFF_BASE_RANGE)
                    cost += coeff_costs->lps_cost[ctx][base_range];
                else
                    cost += coeff_costs->lps_cost[ctx][COEFF_BASE_RANGE];

                if (level >= 1 + NUM_BASE_LEVELS + COEFF_BASE_RANGE) cost += get_golomb_cost(level);
            }
        }
    }

    /* Optimized Loop, omitted first (eob - 1) and last (0) index */
    for (c = eob - 2; c >= 1; --c) {
        const int32_t pos   = scan[c];
        const int32_t level = abs(qcoeff[pos]);
        if (level > NUM_BASE_LEVELS) {
#if TXS_DEPTH_2
            const int32_t ctx = get_br_ctx(levels, pos, bwl, tx_type_to_class[transform_type]);
#else
            const int32_t ctx        = get_br_ctx(levels, pos, bwl, transform_type);
#endif
            const int32_t base_range = level - 1 - NUM_BASE_LEVELS;

            if (base_range < COEFF_BASE_RANGE) {
                cost += cost_literal + coeff_costs->lps_cost[ctx][base_range] +
                        coeff_costs->base_cost[coeff_contexts[pos]][3];
            } else {
                cost += get_golomb_cost(level) + cost_literal +
                        coeff_costs->lps_cost[ctx][COEFF_BASE_RANGE] +
                        coeff_costs->base_cost[coeff_contexts[pos]][3];
            }
        } else if (level) {
            cost += cost_literal + coeff_costs->base_cost[coeff_contexts[pos]][level];
        } else {
            cost += coeff_costs->base_cost[coeff_contexts[pos]][0];
        }
    }
    return cost;
}

// Note: don't call this function when eob is 0.
uint64_t eb_av1_cost_coeffs_txb(uint8_t allow_update_cdf, FRAME_CONTEXT *ec_ctx,
                                struct ModeDecisionCandidateBuffer *candidate_buffer_ptr,
                                const TranLow *const qcoeff, uint16_t eob, PlaneType plane_type,
                                TxSize transform_size, TxType transform_type, int16_t txb_skip_ctx,
                                int16_t dc_sign_ctx, EbBool reduced_transform_set_flag)

{
    //Note: there is a different version of this function in AOM that seems to be efficient as its name is:
    //warehouse_efficients_txb

    const TxSize txs_ctx =
        (TxSize)((txsize_sqr_map[transform_size] + txsize_sqr_up_map[transform_size] + 1) >> 1);
    const TxClass          tx_class = tx_type_to_class[transform_type];
    int32_t                cost;
    const int32_t          bwl    = get_txb_bwl(transform_size);
    const int32_t          width  = get_txb_wide(transform_size);
    const int32_t          height = get_txb_high(transform_size);
    const ScanOrder *const scan_order =
        &av1_scan_orders[transform_size][transform_type]; // get_scan(tx_size, tx_type);
    const int16_t *const scan = scan_order->scan;
    uint8_t              levels_buf[TX_PAD_2D];
    uint8_t *const       levels = set_levels(levels_buf, width);
    DECLARE_ALIGNED(16, int8_t, coeff_contexts[MAX_TX_SQUARE]);
    assert(txs_ctx < TX_SIZES);
    const LvMapCoeffCost *const coeff_costs =
        &candidate_buffer_ptr->candidate_ptr->md_rate_estimation_ptr
             ->coeff_fac_bits[txs_ctx][plane_type];

    const int32_t             eob_multi_size = txsize_log2_minus4[transform_size];
    const LvMapEobCost *const eob_bits =
        &candidate_buffer_ptr->candidate_ptr->md_rate_estimation_ptr
             ->eob_frac_bits[eob_multi_size][plane_type];
    // eob must be greater than 0 here.
    assert(eob > 0);
    cost = coeff_costs->txb_skip_cost[txb_skip_ctx][0];

    if (allow_update_cdf) update_cdf(ec_ctx->txb_skip_cdf[txs_ctx][txb_skip_ctx], eob == 0, 2);
    eb_av1_txb_init_levels(
        qcoeff,
        width,
        height,
        levels); // NM - Needs to be optimized - to be combined with the quantisation.

    // Transform type bit estimation
    cost += plane_type > PLANE_TYPE_Y
                ? 0
                : av1_transform_type_rate_estimation(
                      allow_update_cdf,
                      ec_ctx,
                      candidate_buffer_ptr,
                      candidate_buffer_ptr->candidate_ptr->type == INTER_MODE ? EB_TRUE : EB_FALSE,
                      transform_size,
                      transform_type,
                      reduced_transform_set_flag);

    // Transform eob bit estimation
#if TXS_DEPTH_2
    int32_t eob_cost = get_eob_cost(eob, eob_bits, coeff_costs, tx_class);
#else
    int32_t eob_cost = get_eob_cost(eob, eob_bits, coeff_costs, transform_type);
#endif
    cost += eob_cost;
    if (allow_update_cdf)
        eb_av1_update_eob_context(
            eob, transform_size, tx_class, plane_type, ec_ctx, allow_update_cdf);
    // Transform non-zero coeff bit estimation
    eb_av1_get_nz_map_contexts(levels,
                               scan,
                               eob,
                               transform_size,
                               tx_class,
                               coeff_contexts); // NM - Assembly version is available in AOM

    if (allow_update_cdf) {
        for (int c = eob - 1; c >= 0; --c) {
            const int     pos       = scan[c];
            const int     coeff_ctx = coeff_contexts[pos];
            const TranLow v         = qcoeff[pos];
            const TranLow level     = abs(v);

            if (allow_update_cdf) {
                if (c == eob - 1) {
                    assert(coeff_ctx < 4);
                    update_cdf(ec_ctx->coeff_base_eob_cdf[txs_ctx][plane_type][coeff_ctx],
                               AOMMIN(level, 3) - 1,
                               3);
                } else {
                    update_cdf(ec_ctx->coeff_base_cdf[txs_ctx][plane_type][coeff_ctx],
                               AOMMIN(level, 3),
                               4);
                }
            }

            {
                if (c == eob - 1) {
                    assert(coeff_ctx < 4);
#if CONFIG_ENTROPY_STATS
                    ++td->counts->coeff_base_eob_multi[cdf_idx][txsize_ctx][plane_type][coeff_ctx]
                                                      [AOMMIN(level, 3) - 1];
                } else {
                    ++td->counts->coeff_base_multi[cdf_idx][txsize_ctx][plane_type][coeff_ctx]
                                                  [AOMMIN(level, 3)];
#endif
                }
            }

            if (level > NUM_BASE_LEVELS) {
                const int base_range = level - 1 - NUM_BASE_LEVELS;
#if TXS_DEPTH_2
                const int br_ctx = get_br_ctx(levels, pos, bwl, tx_class);
#else
                const int br_ctx     = get_br_ctx(levels, pos, bwl, (const TxType)tx_class);
#endif

                for (int idx = 0; idx < COEFF_BASE_RANGE; idx += BR_CDF_SIZE - 1) {
                    const int k = AOMMIN(base_range - idx, BR_CDF_SIZE - 1);
                    if (allow_update_cdf) {
                        update_cdf(
                            ec_ctx->coeff_br_cdf[AOMMIN(txs_ctx, TX_32X32)][plane_type][br_ctx],
                            k,
                            BR_CDF_SIZE);
                    }
                    for (int lps = 0; lps < BR_CDF_SIZE - 1; lps++) {
#if CONFIG_ENTROPY_STATS
                        ++td->counts->coeff_lps[AOMMIN(txsize_ctx, TX_32X32)][plane_type][lps]
                                               [br_ctx][lps == k];
#endif // CONFIG_ENTROPY_STATS
                        if (lps == k) break;
                    }
#if CONFIG_ENTROPY_STATS
                    ++td->counts->coeff_lps_multi[cdf_idx][AOMMIN(txsize_ctx, TX_32X32)][plane_type]
                                                 [br_ctx][k];
#endif
                    if (k < BR_CDF_SIZE - 1) break;
                }
            }
        }

        if (qcoeff[0] != 0) {
            const int dc_sign = (qcoeff[0] < 0) ? 1 : 0;
            if (allow_update_cdf)
                update_cdf(ec_ctx->dc_sign_cdf[plane_type][dc_sign_ctx], dc_sign, 2);
        }

        //TODO: CHKN  for 128x128 where we need more than one TXb, we need to update the txb_context(dc_sign+skip_ctx) in a Txb basis.

        return 0;
    }

    cost += av1_cost_coeffs_txb_loop_cost_eob(
        eob, scan, qcoeff, coeff_contexts, coeff_costs, dc_sign_ctx, levels, bwl, transform_type);

    return cost;
}
int av1_filter_intra_allowed_bsize(uint8_t enable_filter_intra, BlockSize bs);
int av1_filter_intra_allowed(uint8_t enable_filter_intra, BlockSize bsize, uint8_t palette_size,
                             uint32_t mode);

/*static*/ void model_rd_from_sse(BlockSize bsize, int16_t quantizer,
                                  //const Av1Comp *const cpi,
                                  //const MacroBlockD *const xd,
                                  //BlockSize bsize,
                                  //int32_t plane,
                                  uint64_t sse, uint32_t *rate, uint64_t *dist);

uint64_t av1_intra_fast_cost(BlkStruct *blk_ptr, ModeDecisionCandidate *candidate_ptr, uint32_t qp,
                             uint64_t luma_distortion, uint64_t chroma_distortion, uint64_t lambda,
                             EbBool use_ssd, PictureControlSet *pcs_ptr, CandidateMv *ref_mv_stack,
                             const BlockGeom *blk_geom, uint32_t miRow, uint32_t miCol,
                             uint8_t enable_inter_intra, EbBool full_cost_shut_fast_rate_flag,
                             uint8_t md_pass, uint32_t left_neighbor_mode,
                             uint32_t top_neighbor_mode)

{
    UNUSED(qp);
    UNUSED(ref_mv_stack);
    UNUSED(miRow);
    UNUSED(miCol);
    UNUSED(left_neighbor_mode);
    UNUSED(top_neighbor_mode);
    UNUSED(md_pass);
    UNUSED(enable_inter_intra);
    FrameHeader *frm_hdr = &pcs_ptr->parent_pcs_ptr->frm_hdr;
    if (av1_allow_intrabc(&pcs_ptr->parent_pcs_ptr->frm_hdr, pcs_ptr->parent_pcs_ptr->slice_type)
        && candidate_ptr->use_intrabc) {
        uint64_t luma_sad         = (LUMA_WEIGHT * luma_distortion) << AV1_COST_PRECISION;
        uint64_t chromasad_       = chroma_distortion << AV1_COST_PRECISION;
        uint64_t total_distortion = luma_sad + chromasad_;

        uint64_t rate = 0;

        EbReflist ref_list_idx = 0;
        int16_t   pred_ref_x   = candidate_ptr->motion_vector_pred_x[ref_list_idx];
        int16_t   pred_ref_y   = candidate_ptr->motion_vector_pred_y[ref_list_idx];
        int16_t   mv_ref_x     = candidate_ptr->motion_vector_xl0;
        int16_t   mv_ref_y     = candidate_ptr->motion_vector_yl0;
        MV        mv;
        mv.row = mv_ref_y;
        mv.col = mv_ref_x;
        MV ref_mv;
        ref_mv.row = pred_ref_y;
        ref_mv.col = pred_ref_x;

        int *dvcost[2] = {(int *)&candidate_ptr->md_rate_estimation_ptr->dv_cost[0][MV_MAX],
                          (int *)&candidate_ptr->md_rate_estimation_ptr->dv_cost[1][MV_MAX]};

        int32_t mv_rate = eb_av1_mv_bit_cost(&mv,
                                             &ref_mv,
                                             candidate_ptr->md_rate_estimation_ptr->dv_joint_cost,
                                             dvcost,
                                             MV_COST_WEIGHT_SUB);

        rate = mv_rate +
               candidate_ptr->md_rate_estimation_ptr->intrabc_fac_bits[candidate_ptr->use_intrabc];

        candidate_ptr->fast_luma_rate   = rate;
        candidate_ptr->fast_chroma_rate = 0;

        luma_sad         = (LUMA_WEIGHT * luma_distortion) << AV1_COST_PRECISION;
        chromasad_       = chroma_distortion << AV1_COST_PRECISION;
        total_distortion = luma_sad + chromasad_;

        return (RDCOST(lambda, rate, total_distortion));
    } else {
        EbBool is_monochrome_flag = EB_FALSE; // NM - is_monochrome_flag is harcoded to false.
        EbBool is_cfl_allowed     = (blk_geom->bwidth <= 32 && blk_geom->bheight <= 32) ? 1 : 0;

        uint8_t sub_sampling_x = 1; // NM - subsampling_x is harcoded to 1 for 420 chroma sampling.
        uint8_t sub_sampling_y = 1; // NM - subsampling_y is harcoded to 1 for 420 chroma sampling.
        // In fast loop CFL alphas are not know yet. The chroma mode bits are calculated based on DC Mode, and if CFL is the winner compared to CFL, ChromaBits are updated
        uint32_t chroma_mode = candidate_ptr->intra_chroma_mode == UV_CFL_PRED
                                   ? UV_DC_PRED
                                   : candidate_ptr->intra_chroma_mode;

        // Number of bits for each synatax element
        uint64_t intra_mode_bits_num            = 0;
        uint64_t intra_luma_mode_bits_num       = 0;
        uint64_t intra_luma_ang_mode_bits_num   = 0;
        uint64_t intra_filter_mode_bits_num     = 0;
        uint64_t intra_chroma_mode_bits_num     = 0;
        uint64_t intra_chroma_ang_mode_bits_num = 0;
        uint64_t skip_mode_rate                 = 0;
        uint8_t  skip_mode_ctx =
            blk_ptr->skip_flag_context; // NM - Harcoded to 1 until the skip_mode context is added.
        PredictionMode intra_mode = (PredictionMode)candidate_ptr->pred_mode;
        // Luma and chroma rate
        uint32_t rate;
        uint32_t luma_rate   = 0;
        uint32_t chroma_rate = 0;
        uint64_t luma_sad, chromasad_;

        // Luma and chroma distortion
        uint64_t      total_distortion;
        const int32_t above_ctx = intra_mode_context[top_neighbor_mode];
        const int32_t left_ctx  = intra_mode_context[left_neighbor_mode];
        intra_mode_bits_num =
            pcs_ptr->slice_type != I_SLICE
                ? (uint64_t)candidate_ptr->md_rate_estimation_ptr
                      ->mb_mode_fac_bits[size_group_lookup[blk_geom->bsize]][intra_mode]
                : ZERO_COST;
        skip_mode_rate =
            pcs_ptr->slice_type != I_SLICE
                ? (uint64_t)
                      candidate_ptr->md_rate_estimation_ptr->skip_mode_fac_bits[skip_mode_ctx][0]
                : ZERO_COST;

        // Estimate luma nominal intra mode bits
        intra_luma_mode_bits_num = pcs_ptr->slice_type == I_SLICE
                                       ? (uint64_t)candidate_ptr->md_rate_estimation_ptr
                                             ->y_mode_fac_bits[above_ctx][left_ctx][intra_mode]
                                       : ZERO_COST;
        // Estimate luma angular mode bits
        if (blk_geom->bsize >= BLOCK_8X8 && candidate_ptr->is_directional_mode_flag) {
            assert((intra_mode - V_PRED) < 8);
            assert((intra_mode - V_PRED) >= 0);
            intra_luma_ang_mode_bits_num =
                candidate_ptr->md_rate_estimation_ptr
                    ->angle_delta_fac_bits[intra_mode - V_PRED]
                                          [MAX_ANGLE_DELTA +
                                           candidate_ptr->angle_delta[PLANE_TYPE_Y]];
        }
        if (av1_allow_palette(pcs_ptr->parent_pcs_ptr->frm_hdr.allow_screen_content_tools,
                              blk_geom->bsize) &&
            intra_mode == DC_PRED) {
            const int use_palette = candidate_ptr->palette_info.pmi.palette_size[0] > 0;
            const int bsize_ctx   = av1_get_palette_bsize_ctx(blk_geom->bsize);
            const int mode_ctx    = av1_get_palette_mode_ctx(blk_ptr->av1xd);
            intra_luma_mode_bits_num +=
                candidate_ptr->md_rate_estimation_ptr
                    ->palette_ymode_fac_bits[bsize_ctx][mode_ctx][use_palette];
            if (use_palette) {
                const uint8_t *const color_map = candidate_ptr->palette_info.color_idx_map;
                int                  block_width, block_height, rows, cols;
                av1_get_block_dimensions(
                    blk_geom->bsize, 0, blk_ptr->av1xd, &block_width, &block_height, &rows, &cols);
                const int plt_size = candidate_ptr->palette_info.pmi.palette_size[0];
                int       palette_mode_cost =
                    candidate_ptr->md_rate_estimation_ptr
                        ->palette_ysize_fac_bits[bsize_ctx][plt_size - PALETTE_MIN_SIZE] +
                    write_uniform_cost(plt_size, color_map[0]);
                uint16_t  color_cache[2 * PALETTE_MAX_SIZE];
                const int n_cache = eb_get_palette_cache(blk_ptr->av1xd, 0, color_cache);
                palette_mode_cost += av1_palette_color_cost_y(
                    &candidate_ptr->palette_info.pmi, color_cache, n_cache,
                    pcs_ptr->parent_pcs_ptr->scs_ptr->encoder_bit_depth);
                palette_mode_cost += av1_cost_color_map(&candidate_ptr->palette_info,
                                                        candidate_ptr->md_rate_estimation_ptr,
                                                        blk_ptr,
                                                        0,
                                                        blk_geom->bsize,
                                                        PALETTE_MAP);
                intra_luma_mode_bits_num += palette_mode_cost;
            }
        }

        if (av1_filter_intra_allowed(
                pcs_ptr->parent_pcs_ptr->scs_ptr->seq_header.enable_filter_intra,
                blk_geom->bsize,
                candidate_ptr->palette_info.pmi.palette_size[0],
                intra_mode)) {
            intra_filter_mode_bits_num =
                candidate_ptr->md_rate_estimation_ptr
                    ->filter_intra_fac_bits[blk_geom->bsize]
                                           [candidate_ptr->filter_intra_mode != FILTER_INTRA_MODES];
            if (candidate_ptr->filter_intra_mode != FILTER_INTRA_MODES) {
                intra_filter_mode_bits_num +=
                    candidate_ptr->md_rate_estimation_ptr
                        ->filter_intra_mode_fac_bits[candidate_ptr->filter_intra_mode];
            }
        }
        if (blk_geom->has_uv) {
            if (!is_monochrome_flag &&
                is_chroma_reference(
                    miRow, miCol, blk_geom->bsize, sub_sampling_x, sub_sampling_y)) {
                // Estimate luma nominal intra mode bits
                intra_chroma_mode_bits_num =
                    (uint64_t)candidate_ptr->md_rate_estimation_ptr
                        ->intra_uv_mode_fac_bits[is_cfl_allowed][intra_mode][chroma_mode];
                // Estimate luma angular mode bits
                if (blk_geom->bsize >= BLOCK_8X8 &&
                    candidate_ptr->is_directional_chroma_mode_flag) {
                    intra_chroma_ang_mode_bits_num =
                        candidate_ptr->md_rate_estimation_ptr
                            ->angle_delta_fac_bits[chroma_mode - V_PRED]
                                                  [MAX_ANGLE_DELTA +
                                                   candidate_ptr->angle_delta[PLANE_TYPE_UV]];
                }
                if (av1_allow_palette(pcs_ptr->parent_pcs_ptr->frm_hdr.allow_screen_content_tools,
                                      blk_geom->bsize) &&
                    chroma_mode == UV_DC_PRED) {
                    const PaletteModeInfo *pmi         = &candidate_ptr->palette_info.pmi;
                    const int              use_palette = pmi->palette_size[1] > 0;
                    intra_chroma_ang_mode_bits_num +=
                        candidate_ptr->md_rate_estimation_ptr
                            ->palette_uv_mode_fac_bits[pmi->palette_size[0] > 0][use_palette];
                }
            }
        }

        uint32_t is_inter_rate = pcs_ptr->slice_type != I_SLICE
                                     ? candidate_ptr->md_rate_estimation_ptr
                                           ->intra_inter_fac_bits[blk_ptr->is_inter_ctx][0]
                                     : 0;
        luma_rate =
            (uint32_t)(intra_mode_bits_num + skip_mode_rate + intra_luma_mode_bits_num +
                       intra_luma_ang_mode_bits_num + is_inter_rate + intra_filter_mode_bits_num);
        if (av1_allow_intrabc(&pcs_ptr->parent_pcs_ptr->frm_hdr, pcs_ptr->parent_pcs_ptr->slice_type))
            luma_rate +=
                candidate_ptr->md_rate_estimation_ptr->intrabc_fac_bits[candidate_ptr->use_intrabc];

        chroma_rate = (uint32_t)(intra_chroma_mode_bits_num + intra_chroma_ang_mode_bits_num);

        // Keep the Fast Luma and Chroma rate for future use
        candidate_ptr->fast_luma_rate   = (full_cost_shut_fast_rate_flag) ? 0 : luma_rate;
        candidate_ptr->fast_chroma_rate = (full_cost_shut_fast_rate_flag) ? 0 : chroma_rate;

        if (use_ssd) {
            int32_t         current_q_index = frm_hdr->quantization_params.base_q_idx;
            Dequants *const dequants        = &pcs_ptr->parent_pcs_ptr->deq;

            int16_t quantizer = dequants->y_dequant_q3[current_q_index][1];
            rate              = 0;
            model_rd_from_sse(blk_geom->bsize, quantizer, luma_distortion, &rate, &luma_sad);
            luma_rate += rate;
            total_distortion = luma_sad;

            rate = 0;
            model_rd_from_sse(
                blk_geom->bsize_uv, quantizer, chroma_distortion, &chroma_rate, &chromasad_);
            chroma_rate += rate;
            total_distortion += chromasad_;

            rate = luma_rate + chroma_rate;

            return (RDCOST(lambda, rate, total_distortion));
        } else {
            luma_sad         = (LUMA_WEIGHT * luma_distortion) << AV1_COST_PRECISION;
            chromasad_       = chroma_distortion << AV1_COST_PRECISION;
            total_distortion = luma_sad + chromasad_;

            rate = luma_rate + chroma_rate;

            // Assign fast cost
            return (RDCOST(lambda, rate, total_distortion));
        }
    }
}

//extern INLINE int32_t have_newmv_in_inter_mode(PredictionMode mode);
static INLINE int32_t have_newmv_in_inter_mode(PredictionMode mode) {
    return (mode == NEWMV || mode == NEW_NEWMV || mode == NEAREST_NEWMV || mode == NEW_NEARESTMV ||
            mode == NEAR_NEWMV || mode == NEW_NEARMV);
}

extern void av1_set_ref_frame(MvReferenceFrame *rf, int8_t ref_frame_type);

static INLINE int has_second_ref(const MbModeInfo *mbmi) {
    return mbmi->block_mi.ref_frame[1] > INTRA_FRAME;
}

static INLINE int has_uni_comp_refs(const MbModeInfo *mbmi) {
    return has_second_ref(mbmi) && (!((mbmi->block_mi.ref_frame[0] >= BWDREF_FRAME) ^
                                      (mbmi->block_mi.ref_frame[1] >= BWDREF_FRAME)));
}

// This function encodes the reference frame
uint64_t estimate_ref_frames_num_bits(PictureControlSet *    pcs_ptr,
                                      ModeDecisionCandidate *candidate_ptr, BlkStruct *blk_ptr,
                                      uint32_t bwidth, uint32_t bheight, uint8_t ref_frame_type,
                                      uint8_t md_pass, EbBool is_compound) {
    FrameHeader *frm_hdr       = &pcs_ptr->parent_pcs_ptr->frm_hdr;
    uint64_t     ref_rate_bits = 0;

    if (md_pass == 1) {
        uint64_t ref_rate_a = 0;
        uint64_t ref_rate_b = 0;
        uint64_t ref_rate_c = 0;
        uint64_t ref_rate_d = 0;
        uint64_t ref_rate_e = 0;
        uint64_t ref_rate_f = 0;
        uint64_t ref_rate_g = 0;
        uint64_t ref_rate_h = 0;
        uint64_t ref_rate_i = 0;
        uint64_t ref_rate_j = 0;
        uint64_t ref_rate_k = 0;
        uint64_t ref_rate_l = 0;
        uint64_t ref_rate_m = 0;
        uint64_t ref_rate_n = 0;
        uint64_t ref_rate_o = 0;
        uint64_t ref_rate_p = 0;
        // const MbModeInfo *const mbmi = &blk_ptr->av1xd->mi[0]->mbmi;
        MbModeInfo *const mbmi = &blk_ptr->av1xd->mi[0]->mbmi;
        MvReferenceFrame  ref_type[2];
        av1_set_ref_frame(ref_type, ref_frame_type);
        mbmi->block_mi.ref_frame[0] = ref_type[0];
        mbmi->block_mi.ref_frame[1] = ref_type[1];
        //const int is_compound = has_second_ref(mbmi);
        {
            // does the feature use compound prediction or not
            // (if not specified at the frame/segment level)
            if (frm_hdr->reference_mode == REFERENCE_MODE_SELECT) {
                if (MIN(bwidth, bheight) >= 8) {
                    //aom_write_symbol(w, is_compound, av1_get_reference_mode_cdf(blk_ptr->av1xd), 2);
                    int32_t context = av1_get_reference_mode_context_new(blk_ptr->av1xd);
                    ref_rate_a      = candidate_ptr->md_rate_estimation_ptr
                                     ->comp_inter_fac_bits[context][is_compound];
                }
            } else {
                assert((!is_compound) == (frm_hdr->reference_mode == SINGLE_REFERENCE));
            }

            if (is_compound) {
                const CompReferenceType comp_ref_type =
                    has_uni_comp_refs(mbmi) ? UNIDIR_COMP_REFERENCE : BIDIR_COMP_REFERENCE;

                const int pred_context = av1_get_comp_reference_type_context_new(blk_ptr->av1xd);
                ref_rate_b             = candidate_ptr->md_rate_estimation_ptr
                                 ->comp_ref_type_fac_bits[pred_context][comp_ref_type];
                /*aom_write_symbol(w, comp_ref_type, av1_get_comp_reference_type_cdf(blk_ptr->av1xd),
                    2);*/

                if (comp_ref_type == UNIDIR_COMP_REFERENCE) {
                    //SVT_LOG("ERROR[AN]: UNIDIR_COMP_REFERENCE not supported\n");
                    const int bit = mbmi->block_mi.ref_frame[0] == BWDREF_FRAME;

                    const int pred_context = eb_av1_get_pred_context_uni_comp_ref_p(blk_ptr->av1xd);
                    ref_rate_c             = candidate_ptr->md_rate_estimation_ptr
                                     ->uni_comp_ref_fac_bits[pred_context][0][bit];
                    //blk_ptr->av1xd->tile_ctx->uni_comp_ref_cdf[pred_context][0];
                    //WRITE_REF_BIT(bit, uni_comp_ref_p);

                    if (!bit) {
                        assert(mbmi->block_mi.ref_frame[0] == LAST_FRAME);
                        const int bit1 = mbmi->block_mi.ref_frame[1] == LAST3_FRAME ||
                                         mbmi->block_mi.ref_frame[1] == GOLDEN_FRAME;
                        const int pred_context =
                            eb_av1_get_pred_context_uni_comp_ref_p1(blk_ptr->av1xd);
                        ref_rate_d = candidate_ptr->md_rate_estimation_ptr
                                         ->uni_comp_ref_fac_bits[pred_context][1][bit1];
                        //ref_rate_d = blk_ptr->av1xd->tile_ctx->uni_comp_ref_cdf[pred_context][1];
                        //WRITE_REF_BIT(bit1, uni_comp_ref_p1);
                        if (bit1) {
                            const int bit2 = mbmi->block_mi.ref_frame[1] == GOLDEN_FRAME;
                            const int pred_context =
                                eb_av1_get_pred_context_uni_comp_ref_p2(blk_ptr->av1xd);
                            ref_rate_e = candidate_ptr->md_rate_estimation_ptr
                                             ->uni_comp_ref_fac_bits[pred_context][2][bit2];

                            // ref_rate_e = blk_ptr->av1xd->tile_ctx->uni_comp_ref_cdf[pred_context][2];
                            //WRITE_REF_BIT(bit2, uni_comp_ref_p2);
                        }
                    }
                    //else {
                    //    assert(mbmi->block_mi.ref_frame[1] == ALTREF_FRAME);
                    //}
                    ref_rate_bits = ref_rate_a + ref_rate_b + ref_rate_c + ref_rate_d + ref_rate_e +
                                    ref_rate_f + ref_rate_g + ref_rate_h + ref_rate_i + ref_rate_j +
                                    ref_rate_k + ref_rate_l + ref_rate_m;
                    return ref_rate_bits;
                    //return;
                }

                assert(comp_ref_type == BIDIR_COMP_REFERENCE);

                const int bit      = (mbmi->block_mi.ref_frame[0] == GOLDEN_FRAME ||
                                 mbmi->block_mi.ref_frame[0] == LAST3_FRAME);
                const int pred_ctx = eb_av1_get_pred_context_comp_ref_p(blk_ptr->av1xd);
                ref_rate_f =
                    candidate_ptr->md_rate_estimation_ptr->comp_ref_fac_bits[pred_ctx][0][bit];
                //ref_rate_f = blk_ptr->av1xd->tile_ctx->comp_ref_cdf[pred_ctx][0];
                //WRITE_REF_BIT(bit, comp_ref_p);

                if (!bit) {
                    const int bit1         = mbmi->block_mi.ref_frame[0] == LAST2_FRAME;
                    const int pred_context = eb_av1_get_pred_context_comp_ref_p1(blk_ptr->av1xd);
                    ref_rate_g             = candidate_ptr->md_rate_estimation_ptr
                                     ->comp_ref_fac_bits[pred_context][1][bit1];
                    //ref_rate_g = blk_ptr->av1xd->tile_ctx->comp_ref_cdf[pred_context][1];
                    //WRITE_REF_BIT(bit1, comp_ref_p1);
                } else {
                    const int bit2         = mbmi->block_mi.ref_frame[0] == GOLDEN_FRAME;
                    const int pred_context = eb_av1_get_pred_context_comp_ref_p2(blk_ptr->av1xd);
                    ref_rate_h             = candidate_ptr->md_rate_estimation_ptr
                                     ->comp_ref_fac_bits[pred_context][2][bit2];
                    //ref_rate_h = blk_ptr->av1xd->tile_ctx->comp_ref_cdf[pred_context][2];
                    //WRITE_REF_BIT(bit2, comp_ref_p2);
                }

                const int bit_bwd    = mbmi->block_mi.ref_frame[1] == ALTREF_FRAME;
                const int pred_ctx_2 = eb_av1_get_pred_context_comp_bwdref_p(blk_ptr->av1xd);
                ref_rate_i           = candidate_ptr->md_rate_estimation_ptr
                                 ->comp_bwd_ref_fac_bits[pred_ctx_2][0][bit_bwd];
                //ref_rate_i = blk_ptr->av1xd->tile_ctx->comp_bwdref_cdf[pred_ctx_2][0];
                //WRITE_REF_BIT(bit_bwd, comp_bwdref_p);

                if (!bit_bwd) {
                    const int pred_context = eb_av1_get_pred_context_comp_bwdref_p1(blk_ptr->av1xd);
                    ref_rate_j =
                        candidate_ptr->md_rate_estimation_ptr
                            ->comp_bwd_ref_fac_bits[pred_context][1][ref_type[1] == ALTREF2_FRAME];
                    //ref_rate_j = blk_ptr->av1xd->tile_ctx->comp_bwdref_cdf[pred_context][1];
                    //WRITE_REF_BIT(mbmi->block_mi.ref_frame[1] == ALTREF2_FRAME, comp_bwdref_p1);
                }
            } else {
                const int bit0 = (mbmi->block_mi.ref_frame[0] <= ALTREF_FRAME &&
                                  mbmi->block_mi.ref_frame[0] >= BWDREF_FRAME);
                ref_rate_k =
                    candidate_ptr->md_rate_estimation_ptr
                        ->single_ref_fac_bits[eb_av1_get_pred_context_single_ref_p1(blk_ptr->av1xd)]
                                             [0][bit0];
                //ref_rate_k = blk_ptr->av1xd->tile_ctx->single_ref_cdf[eb_av1_get_pred_context_single_ref_p1(blk_ptr->av1xd)][0];
                //WRITE_REF_BIT(bit0, single_ref_p1);

                if (bit0) {
                    const int bit1 = mbmi->block_mi.ref_frame[0] == ALTREF_FRAME;
                    ref_rate_l     = candidate_ptr->md_rate_estimation_ptr
                                     ->single_ref_fac_bits[eb_av1_get_pred_context_single_ref_p2(
                                         blk_ptr->av1xd)][1][bit1];
                    //ref_rate_l = blk_ptr->av1xd->tile_ctx->single_ref_cdf[eb_av1_get_pred_context_single_ref_p2(blk_ptr->av1xd)][1];
                    //WRITE_REF_BIT(bit1, single_ref_p2);
                    if (!bit1) {
                        ref_rate_m =
                            candidate_ptr->md_rate_estimation_ptr
                                ->single_ref_fac_bits[eb_av1_get_pred_context_single_ref_p6(
                                    blk_ptr->av1xd)][5][ref_frame_type == ALTREF2_FRAME];
                        //ref_rate_m = blk_ptr->av1xd->tile_ctx->single_ref_cdf[eb_av1_get_pred_context_single_ref_p6(blk_ptr->av1xd)][5];
                        //WRITE_REF_BIT(mbmi->block_mi.ref_frame[0] == ALTREF2_FRAME, single_ref_p6);
                    }
                } else {
                    const int bit2 = (mbmi->block_mi.ref_frame[0] == LAST3_FRAME ||
                                      mbmi->block_mi.ref_frame[0] == GOLDEN_FRAME);
                    ref_rate_n     = candidate_ptr->md_rate_estimation_ptr
                                     ->single_ref_fac_bits[eb_av1_get_pred_context_single_ref_p3(
                                         blk_ptr->av1xd)][2][bit2];
                    //ref_rate_n = blk_ptr->av1xd->tile_ctx->single_ref_cdf[eb_av1_get_pred_context_single_ref_p3(blk_ptr->av1xd)][2];
                    //WRITE_REF_BIT(bit2, single_ref_p3);
                    if (!bit2) {
                        const int bit3 = mbmi->block_mi.ref_frame[0] != LAST_FRAME;
                        ref_rate_o =
                            candidate_ptr->md_rate_estimation_ptr
                                ->single_ref_fac_bits[eb_av1_get_pred_context_single_ref_p4(
                                    blk_ptr->av1xd)][3][bit3];
                        //ref_rate_o = blk_ptr->av1xd->tile_ctx->single_ref_cdf[eb_av1_get_pred_context_single_ref_p4(blk_ptr->av1xd)][3];
                        //WRITE_REF_BIT(bit3, single_ref_p4);
                    } else {
                        const int bit4 = mbmi->block_mi.ref_frame[0] != LAST3_FRAME;
                        ref_rate_p =
                            candidate_ptr->md_rate_estimation_ptr
                                ->single_ref_fac_bits[eb_av1_get_pred_context_single_ref_p5(
                                    blk_ptr->av1xd)][4][bit4];
                        //ref_rate_p = blk_ptr->av1xd->tile_ctx->single_ref_cdf[eb_av1_get_pred_context_single_ref_p5(blk_ptr->av1xd)][4];
                        //WRITE_REF_BIT(bit4, single_ref_p5);
                    }
                }
            }
        }
        ref_rate_bits = ref_rate_a + ref_rate_b + ref_rate_c + ref_rate_d + ref_rate_e +
                        ref_rate_f + ref_rate_g + ref_rate_h + ref_rate_i + ref_rate_j +
                        ref_rate_k + ref_rate_l + ref_rate_m + ref_rate_n + ref_rate_o + ref_rate_p;
    } else {
        uint64_t ref_rate_a = 0;
        uint64_t ref_rate_b = 0;
        uint64_t ref_rate_c = 0;
        uint64_t ref_rate_d = 0;
        uint64_t ref_rate_e = 0;
        uint64_t ref_rate_f = 0;
        uint64_t ref_rate_g = 0;
        uint64_t ref_rate_h = 0;
        uint64_t ref_rate_i = 0;
        uint64_t ref_rate_j = 0;
        uint64_t ref_rate_k = 0;
        uint64_t ref_rate_l = 0;
        uint64_t ref_rate_m = 0;

        // If segment level coding of this signal is disabled...
        // or the segment allows multiple reference frame options
        /*if (segfeature_active(&cm->seg, segment_id, SEG_LVL_REF_FRAME)) {
    assert(!is_compound);
    assert(mbmi->block_mi.ref_frame[0] ==
    get_segdata(&cm->seg, segment_id, SEG_LVL_REF_FRAME));
    }
    else if (segfeature_active(&cm->seg, segment_id, SEG_LVL_SKIP) ||
    segfeature_active(&cm->seg, segment_id, SEG_LVL_GLOBALMV)) {
    assert(!is_compound);
    assert(mbmi->block_mi.ref_frame[0] == LAST_FRAME);
    }
    else*/
        {
            // does the feature use compound prediction or not
            // (if not specified at the frame/segment level)
            if (frm_hdr->reference_mode == REFERENCE_MODE_SELECT) {
                if (MIN(bwidth, bheight) >= 8) {
                    int32_t context = 0;
                    context         = blk_ptr->reference_mode_context;
                    assert(context >= 0 && context < 5);
                    ref_rate_a = candidate_ptr->md_rate_estimation_ptr
                                     ->comp_inter_fac_bits[context][is_compound];
                }
            } else
                assert((!is_compound) == (frm_hdr->reference_mode == SINGLE_REFERENCE));
            int32_t context = 0;
            if (is_compound) {
                const CompReferenceType comp_ref_type = /*has_uni_comp_refs(mbmi)
                                                      ? UNIDIR_COMP_REFERENCE
                                                      : */
                    BIDIR_COMP_REFERENCE;
                MvReferenceFrame ref_type[2];
                av1_set_ref_frame(ref_type, ref_frame_type);

                context = blk_ptr->compoud_reference_type_context;
                assert(context >= 0 && context < 5);
                ref_rate_b = candidate_ptr->md_rate_estimation_ptr
                                 ->comp_ref_type_fac_bits[context][comp_ref_type];

                if (comp_ref_type == UNIDIR_COMP_REFERENCE) {
                    SVT_LOG("ERROR[AN]: UNIDIR_COMP_REFERENCE not supported\n");
                    //const int32_t bit = mbmi->block_mi.ref_frame[0] == BWDREF_FRAME;
                    //WRITE_REF_BIT(bit, uni_comp_ref_p);

                    //if (!bit) {
                    //    assert(mbmi->block_mi.ref_frame[0] == LAST_FRAME);
                    //    const int32_t bit1 = mbmi->block_mi.ref_frame[1] == LAST3_FRAME ||
                    //        mbmi->block_mi.ref_frame[1] == GOLDEN_FRAME;
                    //    WRITE_REF_BIT(bit1, uni_comp_ref_p1);
                    //    if (bit1) {
                    //        const int32_t bit2 = mbmi->block_mi.ref_frame[1] == GOLDEN_FRAME;
                    //        WRITE_REF_BIT(bit2, uni_comp_ref_p2);
                    //    }
                    //}
                    //else {
                    //    assert(mbmi->block_mi.ref_frame[1] == ALTREF_FRAME);
                    //}

                    //return;
                }

                assert(comp_ref_type == BIDIR_COMP_REFERENCE);

                const int32_t bit = (ref_type[0] == GOLDEN_FRAME || ref_type[0] == LAST3_FRAME);

                context = eb_av1_get_pred_context_comp_ref_p(blk_ptr->av1xd);
                assert(context >= 0 && context < 3);
                ref_rate_c =
                    candidate_ptr->md_rate_estimation_ptr->comp_ref_fac_bits[context][0][bit];
                //            WRITE_REF_BIT(bit, comp_ref_p);

                if (!bit) {
                    const int32_t bit1 = (ref_type[0] == LAST2_FRAME);
                    context            = eb_av1_get_pred_context_comp_ref_p1(blk_ptr->av1xd);
                    /*aom_write_symbol(ec_writer, bit1, frame_context->comp_ref_cdf[context][1],
                    2);*/
                    assert(context >= 0 && context < 3);
                    ref_rate_d =
                        candidate_ptr->md_rate_estimation_ptr->comp_ref_fac_bits[context][1][bit1];

                    //WRITE_REF_BIT(bit1, comp_ref_p1);
                } else {
                    const int32_t bit2 = (ref_type[0] == GOLDEN_FRAME);
                    context            = eb_av1_get_pred_context_comp_ref_p2(blk_ptr->av1xd);
                    /*aom_write_symbol(ec_writer, bit2, frame_context->comp_ref_cdf[context][2],
                    2);*/
                    assert(context >= 0 && context < 3);
                    ref_rate_e =
                        candidate_ptr->md_rate_estimation_ptr->comp_ref_fac_bits[context][2][bit2];

                    //WRITE_REF_BIT(bit2, comp_ref_p2);
                }

                const int32_t bit_bwd = (ref_type[1] == ALTREF_FRAME);
                context               = eb_av1_get_pred_context_comp_bwdref_p(blk_ptr->av1xd);
                /*aom_write_symbol(ec_writer, bit_bwd, frame_context->comp_bwdref_cdf[context][0],
                2);*/
                assert(context >= 0 && context < 3);
                ref_rate_f = candidate_ptr->md_rate_estimation_ptr
                                 ->comp_bwd_ref_fac_bits[context][0][bit_bwd];
                //WRITE_REF_BIT(bit_bwd, comp_bwdref_p);

                if (!bit_bwd) {
                    context = eb_av1_get_pred_context_comp_bwdref_p1(blk_ptr->av1xd);
                    /*aom_write_symbol(ec_writer, ref_type[1] == ALTREF2_FRAME, frame_context->comp_bwdref_cdf[context][1],
                    2);*/
                    assert(context >= 0 && context < 3);
                    ref_rate_g =
                        candidate_ptr->md_rate_estimation_ptr
                            ->comp_bwd_ref_fac_bits[context][1][ref_type[1] == ALTREF2_FRAME];
                    //WRITE_REF_BIT(mbmi->block_mi.ref_frame[1] == ALTREF2_FRAME, comp_bwdref_p1);
                }
            } else {
                const int32_t bit0 =
                    (ref_frame_type <= ALTREF_FRAME && ref_frame_type >= BWDREF_FRAME); //0

                context = eb_av1_get_pred_context_single_ref_p1(blk_ptr->av1xd);
                /*aom_write_symbol(ec_writer, bit0, frame_context->single_ref_cdf[context][0],
                2);*/
                assert(context >= 0 && context < 3);
                ref_rate_h =
                    candidate_ptr->md_rate_estimation_ptr->single_ref_fac_bits[context][0][bit0];
                //WRITE_REF_BIT(bit0, single_ref_p1);

                if (bit0) {
                    const int32_t bit1 = (ref_frame_type == ALTREF_FRAME);
                    context            = eb_av1_get_pred_context_single_ref_p2(blk_ptr->av1xd);
                    assert(context >= 0 && context < 3);
                    /*aom_write_symbol(ec_writer, bit1, frame_context->single_ref_cdf[context][1],
                    2);*/
                    ref_rate_i = candidate_ptr->md_rate_estimation_ptr
                                     ->single_ref_fac_bits[context][1][bit1];
                    //WRITE_REF_BIT(bit1, single_ref_p2);

                    if (!bit1) {
                        context = eb_av1_get_pred_context_single_ref_p6(blk_ptr->av1xd);
                        /*aom_write_symbol(ec_writer, blk_ptr->prediction_unit_array[0].ref_frame_type == ALTREF2_FRAME, frame_context->single_ref_cdf[context][5],
                        2);*/
                        assert(context >= 0 && context < 3);
                        ref_rate_j =
                            candidate_ptr->md_rate_estimation_ptr
                                ->single_ref_fac_bits[context][5][ref_frame_type == ALTREF2_FRAME];
                        //WRITE_REF_BIT(mbmi->block_mi.ref_frame[0] == ALTREF2_FRAME, single_ref_p6);
                    }
                } else {
                    const int32_t bit2 =
                        (ref_frame_type == LAST3_FRAME || ref_frame_type == GOLDEN_FRAME); //0
                    context = eb_av1_get_pred_context_single_ref_p3(blk_ptr->av1xd);
                    /*aom_write_symbol(ec_writer, bit2, frame_context->single_ref_cdf[context][2],
                    2);*/
                    assert(context >= 0 && context < 3);
                    ref_rate_k = candidate_ptr->md_rate_estimation_ptr
                                     ->single_ref_fac_bits[context][2][bit2];
                    //WRITE_REF_BIT(bit2, single_ref_p3);

                    if (!bit2) {
                        const int32_t bit3 = (ref_frame_type != LAST_FRAME); //0;
                        context            = eb_av1_get_pred_context_single_ref_p4(blk_ptr->av1xd);
                        assert(context >= 0 && context < 3);
                        /*aom_write_symbol(ec_writer, bit3, frame_context->single_ref_cdf[context][3],
                        2);*/
                        ref_rate_l = candidate_ptr->md_rate_estimation_ptr
                                         ->single_ref_fac_bits[context][3][bit3];
                        //WRITE_REF_BIT(bit3, single_ref_p4);
                    } else {
                        const int32_t bit4 = (ref_frame_type != LAST3_FRAME);
                        context            = eb_av1_get_pred_context_single_ref_p5(blk_ptr->av1xd);
                        /*aom_write_symbol(ec_writer, bit4, frame_context->single_ref_cdf[context][4],
                        2);*/
                        assert(context >= 0 && context < 3);
                        ref_rate_m = candidate_ptr->md_rate_estimation_ptr
                                         ->single_ref_fac_bits[context][4][bit4];
                        //WRITE_REF_BIT(bit4, single_ref_p5);
                    }
                }
            }
        }

        ref_rate_bits = ref_rate_a + ref_rate_b + ref_rate_c + ref_rate_d + ref_rate_e +
                        ref_rate_f + ref_rate_g + ref_rate_h + ref_rate_i + ref_rate_j +
                        ref_rate_k + ref_rate_l + ref_rate_m;
    }
    return ref_rate_bits;
}
//extern INLINE int16_t av1_mode_context_analyzer(const int16_t *const mode_context, const MvReferenceFrame *const rf);

extern int8_t av1_ref_frame_type(const MvReferenceFrame *const rf);
uint16_t      compound_mode_ctx_map_2[3][COMP_NEWMV_CTXS] = {
    {0, 1, 1, 1, 1},
    {1, 2, 3, 4, 4},
    {4, 4, 5, 6, 7},
};
static INLINE int16_t av1_mode_context_analyzer(const int16_t *const          mode_context,
                                                const MvReferenceFrame *const rf) {
    const int8_t ref_frame = av1_ref_frame_type(rf);

    if (rf[1] <= INTRA_FRAME) return mode_context[ref_frame];

    const int16_t newmv_ctx = mode_context[ref_frame] & NEWMV_CTX_MASK;
    const int16_t refmv_ctx = (mode_context[ref_frame] >> REFMV_OFFSET) & REFMV_CTX_MASK;
    assert((refmv_ctx >> 1) < 3);
    const int16_t comp_ctx =
        compound_mode_ctx_map_2[refmv_ctx >> 1][AOMMIN(newmv_ctx, COMP_NEWMV_CTXS - 1)];
    return comp_ctx;
}

int      get_comp_index_context_enc(PictureParentControlSet *pcs_ptr, int cur_frame_index,
                                    int bck_frame_index, int fwd_frame_index, const MacroBlockD *xd);
int      get_comp_group_idx_context_enc(const MacroBlockD *xd);
int      is_any_masked_compound_used(BlockSize sb_type);
uint32_t get_compound_mode_rate(uint8_t md_pass, ModeDecisionCandidate *candidate_ptr,
                                BlkStruct *blk_ptr, uint8_t ref_frame_type, BlockSize bsize,
                                SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr) {
    uint32_t comp_rate = 0;
    if (md_pass == 0) return 0;

    MbModeInfo *const mbmi = &blk_ptr->av1xd->mi[0]->mbmi;
    MvReferenceFrame  rf[2];
    av1_set_ref_frame(rf, ref_frame_type);
    mbmi->block_mi.ref_frame[0] = rf[0];
    mbmi->block_mi.ref_frame[1] = rf[1];

    //NOTE  :  Make sure, any cuPtr data is already set before   usage

    if (has_second_ref(mbmi)) {
        const int masked_compound_used =
            is_any_masked_compound_used(bsize) && scs_ptr->seq_header.enable_masked_compound;

        if (masked_compound_used) {
            const int ctx_comp_group_idx = get_comp_group_idx_context_enc(blk_ptr->av1xd);
            comp_rate =
                candidate_ptr->md_rate_estimation_ptr
                    ->comp_group_idx_fac_bits[ctx_comp_group_idx][candidate_ptr->comp_group_idx];
        } else {
            assert(candidate_ptr->comp_group_idx == 0);
        }

        if (candidate_ptr->comp_group_idx == 0) {
            if (candidate_ptr->compound_idx)
                assert(candidate_ptr->interinter_comp.type == COMPOUND_AVERAGE);

            if (scs_ptr->seq_header.order_hint_info.enable_jnt_comp) {
                const int comp_index_ctx =
                    get_comp_index_context_enc(pcs_ptr->parent_pcs_ptr,
                                               pcs_ptr->parent_pcs_ptr->cur_order_hint,
                                               pcs_ptr->parent_pcs_ptr->ref_order_hint[rf[0] - 1],
                                               pcs_ptr->parent_pcs_ptr->ref_order_hint[rf[1] - 1],
                                               blk_ptr->av1xd);
                comp_rate += candidate_ptr->md_rate_estimation_ptr
                                 ->comp_idx_fac_bits[comp_index_ctx][candidate_ptr->compound_idx];
            } else {
                assert(candidate_ptr->compound_idx == 1);
            }
        } else {
            assert(pcs_ptr->parent_pcs_ptr->frm_hdr.reference_mode != SINGLE_REFERENCE &&
                   is_inter_compound_mode(candidate_ptr->pred_mode));
            assert(masked_compound_used);
            // compound_diffwtd, wedge
            assert(candidate_ptr->interinter_comp.type == COMPOUND_WEDGE ||
                   candidate_ptr->interinter_comp.type == COMPOUND_DIFFWTD);

            if (is_interinter_compound_used(COMPOUND_WEDGE, bsize))
                comp_rate +=
                    candidate_ptr->md_rate_estimation_ptr
                        ->compound_type_fac_bits[bsize][candidate_ptr->interinter_comp.type -
                                                        COMPOUND_WEDGE];

            if (candidate_ptr->interinter_comp.type == COMPOUND_WEDGE) {
                assert(is_interinter_compound_used(COMPOUND_WEDGE, bsize));
                comp_rate +=
                    candidate_ptr->md_rate_estimation_ptr
                        ->wedge_idx_fac_bits[bsize][candidate_ptr->interinter_comp.wedge_index];
                comp_rate += av1_cost_literal(1);
            } else {
                assert(candidate_ptr->interinter_comp.type == COMPOUND_DIFFWTD);
                comp_rate += av1_cost_literal(1);
            }
        }
    }

    return comp_rate;
}
int is_interintra_wedge_used(BlockSize sb_type);
int svt_is_interintra_allowed(uint8_t enable_inter_intra, BlockSize sb_type, PredictionMode mode,
                              MvReferenceFrame ref_frame[2]);

/* two_pass_cost_update
 * This function adds some biases for distortion and rate.
 * The function is used in the first pass only and for the purpose of data collection */
void two_pass_cost_update(PictureControlSet *pcs_ptr, ModeDecisionCandidate *candidate_ptr,
                          uint32_t *rate, uint64_t *distortion) {
    MvReferenceFrame ref_type[2];
    av1_set_ref_frame(ref_type, candidate_ptr->ref_frame_type);
    if ((candidate_ptr->is_compound &&
         (ref_type[0] != LAST_FRAME || ref_type[1] != BWDREF_FRAME)) ||
        (!candidate_ptr->is_compound &&
         (ref_type[0] != LAST_FRAME && ref_type[0] != BWDREF_FRAME))) {
        *rate += (*rate) * FIRST_PASS_COST_PENALTY / 100;
        *distortion += (*distortion) * FIRST_PASS_COST_PENALTY / 100;
    }
    EbReferenceObject *ref_obj_l1 =
        (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[REF_LIST_1][0]->object_ptr;
    if (pcs_ptr->slice_type == B_SLICE &&
        (candidate_ptr->is_compound || ref_type[0] == BWDREF_FRAME) &&
        (ref_obj_l1->slice_type == I_SLICE && ref_obj_l1->ref_poc > pcs_ptr->picture_number)) {
        *rate += (*rate * 2);
        *distortion += (*distortion) * 2;
    }
}
void two_pass_cost_update_64bit(PictureControlSet *pcs_ptr, ModeDecisionCandidate *candidate_ptr,
                                uint64_t *rate, uint64_t *distortion) {
    MvReferenceFrame ref_type[2];
    av1_set_ref_frame(ref_type, candidate_ptr->ref_frame_type);
    if ((candidate_ptr->is_compound &&
         (ref_type[0] != LAST_FRAME || ref_type[1] != BWDREF_FRAME)) ||
        (!candidate_ptr->is_compound &&
         (ref_type[0] != LAST_FRAME && ref_type[0] != BWDREF_FRAME))) {
        *rate += (*rate) * FIRST_PASS_COST_PENALTY / 100;
        *distortion += (*distortion) * FIRST_PASS_COST_PENALTY / 100;
    }
    EbReferenceObject *ref_obj_l1 =
        (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[REF_LIST_1][0]->object_ptr;
    if (pcs_ptr->slice_type == B_SLICE &&
        (candidate_ptr->is_compound || ref_type[0] == BWDREF_FRAME) &&
        (ref_obj_l1->slice_type == I_SLICE && ref_obj_l1->ref_poc > pcs_ptr->picture_number)) {
        *rate += (*rate * 2);
        *distortion += (*distortion) * 2;
    }
}

uint64_t av1_inter_fast_cost(BlkStruct *blk_ptr, ModeDecisionCandidate *candidate_ptr, uint32_t qp,
                             uint64_t luma_distortion, uint64_t chroma_distortion, uint64_t lambda,
                             EbBool use_ssd, PictureControlSet *pcs_ptr, CandidateMv *ref_mv_stack,
                             const BlockGeom *blk_geom, uint32_t miRow, uint32_t miCol,
                             uint8_t enable_inter_intra, EbBool full_cost_shut_fast_rate_flag,
                             uint8_t md_pass, uint32_t left_neighbor_mode,
                             uint32_t top_neighbor_mode)

{
    UNUSED(top_neighbor_mode);
    UNUSED(left_neighbor_mode);
    UNUSED(miCol);
    UNUSED(miRow);

    FrameHeader *frm_hdr = &pcs_ptr->parent_pcs_ptr->frm_hdr;

    // Luma rate
    uint32_t luma_rate   = 0;
    uint32_t chroma_rate = 0;
    uint64_t mv_rate     = 0;
    uint64_t skip_mode_rate;
    // Luma and chroma distortion
    uint64_t luma_sad;
    uint64_t chromasad_;
    uint64_t total_distortion;

    uint32_t rate;

    int16_t pred_ref_x;
    int16_t pred_ref_y;
    int16_t mv_ref_x;
    int16_t mv_ref_y;

    EbReflist ref_list_idx;

    (void)qp;

    PredictionMode inter_mode = (PredictionMode)candidate_ptr->pred_mode;

    uint64_t inter_mode_bits_num = 0;

    uint8_t          skip_mode_ctx = blk_ptr->skip_flag_context;
    MvReferenceFrame rf[2];
    av1_set_ref_frame(rf, candidate_ptr->ref_frame_type);
    uint32_t mode_context = av1_mode_context_analyzer(blk_ptr->inter_mode_ctx, rf);
    skip_mode_rate = candidate_ptr->md_rate_estimation_ptr->skip_mode_fac_bits[skip_mode_ctx][0];
    uint64_t reference_picture_bits_num = 0;

    //Reference Type and Mode Bit estimation

    reference_picture_bits_num = estimate_ref_frames_num_bits(pcs_ptr,
                                                              candidate_ptr,
                                                              blk_ptr,
                                                              blk_geom->bwidth,
                                                              blk_geom->bheight,
                                                              candidate_ptr->ref_frame_type,
                                                              md_pass,
                                                              candidate_ptr->is_compound);

    if (candidate_ptr->is_compound)
        inter_mode_bits_num +=
            candidate_ptr->md_rate_estimation_ptr
                ->inter_compound_mode_fac_bits[mode_context][INTER_COMPOUND_OFFSET(inter_mode)];
    else {
        //uint32_t newmv_ctx = mode_context & NEWMV_CTX_MASK;
        //inter_mode_bits_num = candidate_buffer_ptr->candidate_ptr->md_rate_estimation_ptr->new_mv_mode_fac_bits[mode_ctx][0];

        int16_t newmv_ctx = mode_context & NEWMV_CTX_MASK;
        //aom_write_symbol(ec_writer, mode != NEWMV, frame_context->newmv_cdf[newmv_ctx], 2);
        inter_mode_bits_num += candidate_ptr->md_rate_estimation_ptr
                                   ->new_mv_mode_fac_bits[newmv_ctx][inter_mode != NEWMV];
        if (inter_mode != NEWMV) {
            const int16_t zero_mv_ctx = (mode_context >> GLOBALMV_OFFSET) & GLOBALMV_CTX_MASK;
            //aom_write_symbol(ec_writer, mode != GLOBALMV, frame_context->zeromv_cdf[zero_mv_ctx], 2);
            inter_mode_bits_num += candidate_ptr->md_rate_estimation_ptr
                                       ->zero_mv_mode_fac_bits[zero_mv_ctx][inter_mode != GLOBALMV];
            if (inter_mode != GLOBALMV) {
                int16_t ref_mv_ctx = (mode_context >> REFMV_OFFSET) & REFMV_CTX_MASK;
                /*aom_write_symbol(ec_writer, mode != NEARESTMV, frame_context->refmv_cdf[refmv_ctx], 2);*/
                inter_mode_bits_num +=
                    candidate_ptr->md_rate_estimation_ptr
                        ->ref_mv_mode_fac_bits[ref_mv_ctx][inter_mode != NEARESTMV];
            }
        }
    }
    if (inter_mode == NEWMV || inter_mode == NEW_NEWMV || have_nearmv_in_inter_mode(inter_mode)) {
        //drLIdex cost estimation
        const int32_t new_mv = inter_mode == NEWMV || inter_mode == NEW_NEWMV;
        if (new_mv) {
            int32_t idx;
            for (idx = 0; idx < 2; ++idx) {
                if (blk_ptr->av1xd->ref_mv_count[candidate_ptr->ref_frame_type] > idx + 1) {
                    uint8_t drl_1_ctx = av1_drl_ctx(ref_mv_stack, idx);
                    inter_mode_bits_num +=
                        candidate_ptr->md_rate_estimation_ptr
                            ->drl_mode_fac_bits[drl_1_ctx][candidate_ptr->drl_index != idx];
                    if (candidate_ptr->drl_index == idx) break;
                }
            }
        }

        if (have_nearmv_in_inter_mode(inter_mode)) {
            int32_t idx;
            // TODO(jingning): Temporary solution to compensate the NEARESTMV offset.
            for (idx = 1; idx < 3; ++idx) {
                if (blk_ptr->av1xd->ref_mv_count[candidate_ptr->ref_frame_type] > idx + 1) {
                    uint8_t drl_ctx = av1_drl_ctx(ref_mv_stack, idx);
                    inter_mode_bits_num +=
                        candidate_ptr->md_rate_estimation_ptr
                            ->drl_mode_fac_bits[drl_ctx][candidate_ptr->drl_index != (idx - 1)];

                    if (candidate_ptr->drl_index == (idx - 1)) break;
                }
            }
        }
    }

    if (have_newmv_in_inter_mode(inter_mode)) {
        if (candidate_ptr->is_compound) {
            mv_rate = 0;

            if (inter_mode == NEW_NEWMV) {
                for (ref_list_idx = 0; ref_list_idx < 2; ++ref_list_idx) {
                    pred_ref_x = candidate_ptr->motion_vector_pred_x[ref_list_idx];
                    pred_ref_y = candidate_ptr->motion_vector_pred_y[ref_list_idx];
                    mv_ref_x   = ref_list_idx == REF_LIST_1 ? candidate_ptr->motion_vector_xl1
                                                          : candidate_ptr->motion_vector_xl0;
                    mv_ref_y = ref_list_idx == REF_LIST_1 ? candidate_ptr->motion_vector_yl1
                                                          : candidate_ptr->motion_vector_yl0;

                    MV mv;
                    mv.row = mv_ref_y;
                    mv.col = mv_ref_x;

                    MV ref_mv;
                    ref_mv.row = pred_ref_y;
                    ref_mv.col = pred_ref_x;

                    mv_rate +=
                        eb_av1_mv_bit_cost(&mv,
                                           &ref_mv,
                                           candidate_ptr->md_rate_estimation_ptr->nmv_vec_cost,
                                           candidate_ptr->md_rate_estimation_ptr->nmvcoststack,
                                           MV_COST_WEIGHT);
                }
            } else if (inter_mode == NEAREST_NEWMV || inter_mode == NEAR_NEWMV) {
                pred_ref_x = candidate_ptr->motion_vector_pred_x[REF_LIST_1];
                pred_ref_y = candidate_ptr->motion_vector_pred_y[REF_LIST_1];
                mv_ref_x   = candidate_ptr->motion_vector_xl1;
                mv_ref_y   = candidate_ptr->motion_vector_yl1;

                MV mv;
                mv.row = mv_ref_y;
                mv.col = mv_ref_x;

                MV ref_mv;
                ref_mv.row = pred_ref_y;
                ref_mv.col = pred_ref_x;

                mv_rate += eb_av1_mv_bit_cost(&mv,
                                              &ref_mv,
                                              candidate_ptr->md_rate_estimation_ptr->nmv_vec_cost,
                                              candidate_ptr->md_rate_estimation_ptr->nmvcoststack,
                                              MV_COST_WEIGHT);
            } else {
                assert(inter_mode == NEW_NEARESTMV || inter_mode == NEW_NEARMV);

                pred_ref_x = candidate_ptr->motion_vector_pred_x[REF_LIST_0];
                pred_ref_y = candidate_ptr->motion_vector_pred_y[REF_LIST_0];
                mv_ref_x   = candidate_ptr->motion_vector_xl0;
                mv_ref_y   = candidate_ptr->motion_vector_yl0;

                MV mv;
                mv.row = mv_ref_y;
                mv.col = mv_ref_x;

                MV ref_mv;
                ref_mv.row = pred_ref_y;
                ref_mv.col = pred_ref_x;

                mv_rate += eb_av1_mv_bit_cost(&mv,
                                              &ref_mv,
                                              candidate_ptr->md_rate_estimation_ptr->nmv_vec_cost,
                                              candidate_ptr->md_rate_estimation_ptr->nmvcoststack,
                                              MV_COST_WEIGHT);
            }
        } else {
            ref_list_idx = candidate_ptr->prediction_direction[0] == 0 ? 0 : 1;

            pred_ref_x = candidate_ptr->motion_vector_pred_x[ref_list_idx];
            pred_ref_y = candidate_ptr->motion_vector_pred_y[ref_list_idx];

            mv_ref_x = ref_list_idx == 0 ? candidate_ptr->motion_vector_xl0
                                         : candidate_ptr->motion_vector_xl1;
            mv_ref_y = ref_list_idx == 0 ? candidate_ptr->motion_vector_yl0
                                         : candidate_ptr->motion_vector_yl1;

            MV mv;
            mv.row = mv_ref_y;
            mv.col = mv_ref_x;

            MV ref_mv;
            ref_mv.row = pred_ref_y;
            ref_mv.col = pred_ref_x;

            mv_rate = eb_av1_mv_bit_cost(&mv,
                                         &ref_mv,
                                         candidate_ptr->md_rate_estimation_ptr->nmv_vec_cost,
                                         candidate_ptr->md_rate_estimation_ptr->nmvcoststack,
                                         MV_COST_WEIGHT);
        }
    }
    if (md_pass > 0) {
        // inter intra mode rate
        if (pcs_ptr->parent_pcs_ptr->frm_hdr.reference_mode != COMPOUND_REFERENCE &&
            pcs_ptr->parent_pcs_ptr->scs_ptr->seq_header.enable_interintra_compound &&
            svt_is_interintra_allowed(
                enable_inter_intra, blk_geom->bsize, candidate_ptr->inter_mode, rf)) {
            const int interintra  = candidate_ptr->is_interintra_used;
            const int bsize_group = size_group_lookup[blk_geom->bsize];

            inter_mode_bits_num +=
                candidate_ptr->md_rate_estimation_ptr
                    ->inter_intra_fac_bits[bsize_group][candidate_ptr->is_interintra_used];

            if (interintra) {
                inter_mode_bits_num +=
                    candidate_ptr->md_rate_estimation_ptr
                        ->inter_intra_mode_fac_bits[bsize_group][candidate_ptr->interintra_mode];

                if (is_interintra_wedge_used(blk_geom->bsize)) {
                    inter_mode_bits_num +=
                        candidate_ptr->md_rate_estimation_ptr
                            ->wedge_inter_intra_fac_bits[blk_geom->bsize]
                                                        [candidate_ptr->use_wedge_interintra];

                    if (candidate_ptr->use_wedge_interintra) {
                        inter_mode_bits_num +=
                            candidate_ptr->md_rate_estimation_ptr
                                ->wedge_idx_fac_bits[blk_geom->bsize]
                                                    [candidate_ptr->interintra_wedge_index];
                    }
                }
            }
        }
    }

    EbBool is_inter = inter_mode >= SINGLE_INTER_MODE_START && inter_mode < SINGLE_INTER_MODE_END;
    if (is_inter && frm_hdr->is_motion_mode_switchable && rf[1] != INTRA_FRAME) {
        MotionMode motion_mode_rd                      = candidate_ptr->motion_mode;
        BlockSize  bsize                               = blk_geom->bsize;
        blk_ptr->prediction_unit_array[0].num_proj_ref = candidate_ptr->num_proj_ref;
        MotionMode last_motion_mode_allowed =
            motion_mode_allowed(pcs_ptr, blk_ptr, bsize, rf[0], rf[1], inter_mode);

        switch (last_motion_mode_allowed) {
        case SIMPLE_TRANSLATION: break;
        case OBMC_CAUSAL:
            inter_mode_bits_num +=
                candidate_ptr->md_rate_estimation_ptr
                    ->motion_mode_fac_bits1[bsize][motion_mode_rd == OBMC_CAUSAL];
            break;
        default:
            inter_mode_bits_num +=
                candidate_ptr->md_rate_estimation_ptr->motion_mode_fac_bits[bsize][motion_mode_rd];
        }
    }
    //this func return 0 if masked=0 and distance=0
    inter_mode_bits_num += get_compound_mode_rate(md_pass,
                                                  candidate_ptr,
                                                  blk_ptr,
                                                  candidate_ptr->ref_frame_type,
                                                  blk_geom->bsize,
                                                  pcs_ptr->parent_pcs_ptr->scs_ptr,
                                                  pcs_ptr);
    // NM - To be added when the overlappable mode is adopted
    //    read_compound_type(is_compound)
    // NM - To be added when switchable filter is adopted
    //    if (interpolation_filter == SWITCHABLE) {
    //        for (dir = 0; dir < (enable_dual_filter ? 2 : 1); dir++) {
    //            if (needs_interp_filter()) {
    //                interp_filter[dir]    S()
    //            }
    //            else {
    //                interp_filter[dir] = EIGHTTAP
    //            }
    //        }
    //        if (!enable_dual_filter)
    //            interp_filter[1] = interp_filter[0]
    //    }
    //    else {
    //        for (dir = 0; dir < 2; dir++)
    //            interp_filter[dir] = interpolation_filter
    //    }
    uint32_t is_inter_rate =
        candidate_ptr->md_rate_estimation_ptr->intra_inter_fac_bits[blk_ptr->is_inter_ctx][1];
    luma_rate = (uint32_t)(reference_picture_bits_num + skip_mode_rate + inter_mode_bits_num +
                           mv_rate + is_inter_rate);

    //chroma_rate = intra_chroma_mode_bits_num + intra_chroma_ang_mode_bits_num;

    // Keep the Fast Luma and Chroma rate for future use
    candidate_ptr->fast_luma_rate   = (full_cost_shut_fast_rate_flag) ? 0 : luma_rate;
    candidate_ptr->fast_chroma_rate = (full_cost_shut_fast_rate_flag) ? 0 : chroma_rate;
    if (use_ssd) {
        int32_t         current_q_index = frm_hdr->quantization_params.base_q_idx;
        Dequants *const dequants        = &pcs_ptr->parent_pcs_ptr->deq;

        int16_t quantizer = dequants->y_dequant_q3[current_q_index][1];
        rate              = 0;
        model_rd_from_sse(blk_geom->bsize, quantizer, luma_distortion, &rate, &luma_sad);
        luma_rate += rate;
        total_distortion = luma_sad;

        rate = 0;
        model_rd_from_sse(
            blk_geom->bsize_uv, quantizer, chroma_distortion, &chroma_rate, &chromasad_);
        chroma_rate += rate;
        total_distortion += chromasad_;

        rate = luma_rate + chroma_rate;
        if (pcs_ptr->parent_pcs_ptr->scs_ptr->use_output_stat_file) {
            two_pass_cost_update(pcs_ptr, candidate_ptr, &rate, &total_distortion);
        }
        if (candidate_ptr->merge_flag) {
            uint64_t skip_mode_rate =
                candidate_ptr->md_rate_estimation_ptr->skip_mode_fac_bits[skip_mode_ctx][1];
            if (skip_mode_rate < rate) return (RDCOST(lambda, skip_mode_rate, total_distortion));
        }
        return (RDCOST(lambda, rate, total_distortion));
    } else {
        luma_sad         = (LUMA_WEIGHT * luma_distortion) << AV1_COST_PRECISION;
        chromasad_       = chroma_distortion << AV1_COST_PRECISION;
        total_distortion = luma_sad + chromasad_;
        if (blk_geom->has_uv == 0 && chromasad_ != 0) SVT_LOG("av1_inter_fast_cost: Chroma error");
        rate = luma_rate + chroma_rate;
        if (pcs_ptr->parent_pcs_ptr->scs_ptr->use_output_stat_file) {
            two_pass_cost_update(pcs_ptr, candidate_ptr, &rate, &total_distortion);
        }
        // Assign fast cost
        if (candidate_ptr->merge_flag) {
            uint64_t skip_mode_rate =
                candidate_ptr->md_rate_estimation_ptr->skip_mode_fac_bits[skip_mode_ctx][1];
            if (skip_mode_rate < rate) return (RDCOST(lambda, skip_mode_rate, total_distortion));
        }
        return (RDCOST(lambda, rate, total_distortion));
    }
}

EbErrorType av1_txb_estimate_coeff_bits(
    struct ModeDecisionContext *md_context, uint8_t allow_update_cdf, FRAME_CONTEXT *ec_ctx,
    PictureControlSet *pcs_ptr, struct ModeDecisionCandidateBuffer *candidate_buffer_ptr,
    uint32_t txb_origin_index, uint32_t txb_chroma_origin_index, EntropyCoder *entropy_coder_ptr,
    EbPictureBufferDesc *coeff_buffer_sb, uint32_t y_eob, uint32_t cb_eob, uint32_t cr_eob,
    uint64_t *y_txb_coeff_bits, uint64_t *cb_txb_coeff_bits, uint64_t *cr_txb_coeff_bits,
    TxSize txsize, TxSize txsize_uv, TxType tx_type, TxType tx_type_uv,
    COMPONENT_TYPE component_type) {
    (void)entropy_coder_ptr;
    EbErrorType return_error = EB_ErrorNone;

    FrameHeader *frm_hdr = &pcs_ptr->parent_pcs_ptr->frm_hdr;

    int32_t *coeff_buffer;
    int16_t  luma_txb_skip_context = md_context->luma_txb_skip_context;
    int16_t  luma_dc_sign_context  = md_context->luma_dc_sign_context;
    int16_t  cb_txb_skip_context   = md_context->cb_txb_skip_context;
    int16_t  cb_dc_sign_context    = md_context->cb_dc_sign_context;
    int16_t  cr_txb_skip_context   = md_context->cr_txb_skip_context;
    int16_t  cr_dc_sign_context    = md_context->cr_dc_sign_context;

    EbBool reduced_transform_set_flag = frm_hdr->reduced_tx_set ? EB_TRUE : EB_FALSE;

    //Estimate the rate of the transform type and coefficient for Luma

    if (component_type == COMPONENT_LUMA || component_type == COMPONENT_ALL) {
        if (y_eob) {
            coeff_buffer =
                (int32_t *)&coeff_buffer_sb->buffer_y[txb_origin_index * sizeof(int32_t)];

            *y_txb_coeff_bits = eb_av1_cost_coeffs_txb(allow_update_cdf,
                                                       ec_ctx,
                                                       candidate_buffer_ptr,
                                                       coeff_buffer,
                                                       (uint16_t)y_eob,
                                                       PLANE_TYPE_Y,
                                                       txsize,
                                                       tx_type,
                                                       luma_txb_skip_context,
                                                       luma_dc_sign_context,
                                                       reduced_transform_set_flag);
        } else {
            *y_txb_coeff_bits = av1_cost_skip_txb(allow_update_cdf,
                                                  ec_ctx,
                                                  candidate_buffer_ptr,
                                                  txsize,
                                                  PLANE_TYPE_Y,
                                                  luma_txb_skip_context);
        }
    }
    //Estimate the rate of the transform type and coefficient for chroma Cb

    if (component_type == COMPONENT_CHROMA_CB || component_type == COMPONENT_CHROMA ||
        component_type == COMPONENT_ALL) {
        if (cb_eob) {
            coeff_buffer =
                (int32_t *)&coeff_buffer_sb->buffer_cb[txb_chroma_origin_index * sizeof(int32_t)];

            *cb_txb_coeff_bits = eb_av1_cost_coeffs_txb(allow_update_cdf,
                                                        ec_ctx,
                                                        candidate_buffer_ptr,
                                                        coeff_buffer,
                                                        (uint16_t)cb_eob,
                                                        PLANE_TYPE_UV,
                                                        txsize_uv,
                                                        tx_type_uv,
                                                        cb_txb_skip_context,
                                                        cb_dc_sign_context,
                                                        reduced_transform_set_flag);
        } else {
            *cb_txb_coeff_bits = av1_cost_skip_txb(allow_update_cdf,
                                                   ec_ctx,
                                                   candidate_buffer_ptr,
                                                   txsize_uv,
                                                   PLANE_TYPE_UV,
                                                   cb_txb_skip_context);
        }
    }

    if (component_type == COMPONENT_CHROMA_CR || component_type == COMPONENT_CHROMA ||
        component_type == COMPONENT_ALL) {
        //Estimate the rate of the transform type and coefficient for chroma Cr
        if (cr_eob) {
            coeff_buffer =
                (int32_t *)&coeff_buffer_sb->buffer_cr[txb_chroma_origin_index * sizeof(int32_t)];

            *cr_txb_coeff_bits = eb_av1_cost_coeffs_txb(allow_update_cdf,
                                                        ec_ctx,
                                                        candidate_buffer_ptr,
                                                        coeff_buffer,
                                                        (uint16_t)cr_eob,
                                                        PLANE_TYPE_UV,
                                                        txsize_uv,
                                                        tx_type_uv,
                                                        cr_txb_skip_context,
                                                        cr_dc_sign_context,
                                                        reduced_transform_set_flag);
        } else {
            *cr_txb_coeff_bits = av1_cost_skip_txb(allow_update_cdf,
                                                   ec_ctx,
                                                   candidate_buffer_ptr,
                                                   txsize_uv,
                                                   PLANE_TYPE_UV,
                                                   cr_txb_skip_context);
        }
    }

    return return_error;
}

/*********************************************************************************
* av1_intra_full_cost function is used to estimate the cost of an intra candidate mode
* for full mode decisoion module.
*
*   @param *blk_ptr(input)
*       blk_ptr is the pointer of the target CU.
*   @param *candidate_buffer_ptr(input)
*       chromaBufferPtr is the buffer pointer of the candidate luma mode.
*   @param qp(input)
*       qp is the quantizer parameter.
*   @param luma_distortion (input)
*       luma_distortion is the intra condidate luma distortion.
*   @param lambda(input)
*       lambda is the Lagrange multiplier
**********************************************************************************/
EbErrorType av1_full_cost(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                          struct ModeDecisionCandidateBuffer *candidate_buffer_ptr,
                          BlkStruct *blk_ptr, uint64_t *y_distortion, uint64_t *cb_distortion,
                          uint64_t *cr_distortion, uint64_t lambda, uint64_t *y_coeff_bits,
                          uint64_t *cb_coeff_bits, uint64_t *cr_coeff_bits, BlockSize bsize) {
    UNUSED(pcs_ptr);
    UNUSED(bsize);
    UNUSED(blk_ptr);
    EbErrorType return_error = EB_ErrorNone;

    // Luma and chroma rate
    uint64_t luma_rate   = 0;
    uint64_t chroma_rate = 0;
    uint64_t coeff_rate  = 0;

    // Luma and chroma SSE
    uint64_t luma_sse;
    uint64_t chroma_sse;
    uint64_t total_distortion;
    uint64_t rate;

    //Estimate the rate of the transform type and coefficient for Luma
    // Add fast rate to get the total rate of the subject mode
    luma_rate += candidate_buffer_ptr->candidate_ptr->fast_luma_rate;
    chroma_rate += candidate_buffer_ptr->candidate_ptr->fast_chroma_rate;

    // For CFL, costs of alphas are not computed in fast loop, since they are computed in the full loop. The rate costs are added to the full loop.
    // In fast loop CFL alphas are not know yet. The chroma mode bits are calculated based on DC Mode, and if CFL is the winner compared to CFL, ChromaBits are updated in Full loop
    if (context_ptr->blk_geom->has_uv) {
        if (candidate_buffer_ptr->candidate_ptr->type == INTRA_MODE &&
            candidate_buffer_ptr->candidate_ptr->intra_chroma_mode == UV_CFL_PRED) {
            EbBool is_cfl_allowed =
                (context_ptr->blk_geom->bwidth <= 32 && context_ptr->blk_geom->bheight <= 32) ? 1
                                                                                              : 0;

            chroma_rate +=
                candidate_buffer_ptr->candidate_ptr->md_rate_estimation_ptr
                    ->cfl_alpha_fac_bits[candidate_buffer_ptr->candidate_ptr->cfl_alpha_signs]
                                        [CFL_PRED_U][CFL_IDX_U(
                                            candidate_buffer_ptr->candidate_ptr->cfl_alpha_idx)] +
                candidate_buffer_ptr->candidate_ptr->md_rate_estimation_ptr
                    ->cfl_alpha_fac_bits[candidate_buffer_ptr->candidate_ptr->cfl_alpha_signs]
                                        [CFL_PRED_V][CFL_IDX_V(
                                            candidate_buffer_ptr->candidate_ptr->cfl_alpha_idx)];

            chroma_rate +=
                (uint64_t)candidate_buffer_ptr->candidate_ptr->md_rate_estimation_ptr
                    ->intra_uv_mode_fac_bits[is_cfl_allowed][candidate_buffer_ptr->candidate_ptr
                                                                 ->intra_luma_mode][UV_CFL_PRED];
            chroma_rate -=
                (uint64_t)candidate_buffer_ptr->candidate_ptr->md_rate_estimation_ptr
                    ->intra_uv_mode_fac_bits[is_cfl_allowed][candidate_buffer_ptr->candidate_ptr
                                                                 ->intra_luma_mode][UV_DC_PRED];
        }
    }

    uint64_t tx_size_bits = 0;
    if (pcs_ptr->parent_pcs_ptr->frm_hdr.tx_mode == TX_MODE_SELECT)
        tx_size_bits = get_tx_size_bits(candidate_buffer_ptr,
                                        context_ptr,
                                        pcs_ptr,
                                        candidate_buffer_ptr->candidate_ptr->tx_depth,
                                        candidate_buffer_ptr->candidate_ptr->block_has_coeff);

    // Coeff rate

    if (context_ptr->blk_skip_decision && candidate_buffer_ptr->candidate_ptr->type != INTRA_MODE) {
        uint64_t non_skip_cost =
            RDCOST(lambda,
                   (*y_coeff_bits + *cb_coeff_bits + *cr_coeff_bits + tx_size_bits +
                    (uint64_t)candidate_buffer_ptr->candidate_ptr->md_rate_estimation_ptr
                        ->skip_fac_bits[blk_ptr->skip_coeff_context][0]),
                   (y_distortion[0] + cb_distortion[0] + cr_distortion[0]));
        uint64_t skip_cost =
            RDCOST(lambda,
                   ((uint64_t)candidate_buffer_ptr->candidate_ptr->md_rate_estimation_ptr
                        ->skip_fac_bits[blk_ptr->skip_coeff_context][1]),
                   (y_distortion[1] + cb_distortion[1] + cr_distortion[1]));
        if ((candidate_buffer_ptr->candidate_ptr->block_has_coeff == 0) ||
            (skip_cost < non_skip_cost)) {
            y_distortion[0]                                      = y_distortion[1];
            cb_distortion[0]                                     = cb_distortion[1];
            cr_distortion[0]                                     = cr_distortion[1];
            candidate_buffer_ptr->candidate_ptr->block_has_coeff = 0;
        }
        if (candidate_buffer_ptr->candidate_ptr->block_has_coeff)
            coeff_rate = (*y_coeff_bits + *cb_coeff_bits + *cr_coeff_bits +
                          (uint64_t)candidate_buffer_ptr->candidate_ptr->md_rate_estimation_ptr
                              ->skip_fac_bits[blk_ptr->skip_coeff_context][0]);
        else
            coeff_rate = MIN((uint64_t)candidate_buffer_ptr->candidate_ptr->md_rate_estimation_ptr
                                 ->skip_fac_bits[blk_ptr->skip_coeff_context][1],
                             (*y_coeff_bits + *cb_coeff_bits + *cr_coeff_bits +
                              (uint64_t)candidate_buffer_ptr->candidate_ptr->md_rate_estimation_ptr
                                  ->skip_fac_bits[blk_ptr->skip_coeff_context][0]));
    } else
        coeff_rate = (*y_coeff_bits + *cb_coeff_bits + *cr_coeff_bits +
                      (uint64_t)candidate_buffer_ptr->candidate_ptr->md_rate_estimation_ptr
                          ->skip_fac_bits[blk_ptr->skip_coeff_context][0]);

    luma_sse         = y_distortion[0];
    chroma_sse       = cb_distortion[0] + cr_distortion[0];
    total_distortion = luma_sse + chroma_sse;

    rate = luma_rate + chroma_rate + coeff_rate;
    if (candidate_buffer_ptr->candidate_ptr->block_has_coeff) rate += tx_size_bits;
    if (pcs_ptr->parent_pcs_ptr->scs_ptr->use_output_stat_file &&
        candidate_buffer_ptr->candidate_ptr->type != INTRA_MODE) {
        two_pass_cost_update_64bit(
            pcs_ptr, candidate_buffer_ptr->candidate_ptr, &rate, &total_distortion);
    }
    // Assign full cost
    *(candidate_buffer_ptr->full_cost_ptr) = RDCOST(lambda, rate, total_distortion);
    return return_error;
}

/*********************************************************************************
* merge_skip_full_cost function is used to estimate the cost of an AMVPSkip candidate
* mode for full mode decisoion module.
*
*   @param *blk_ptr(input)
*       blk_ptr is the pointer of the target CU.
*   @param *candidate_buffer_ptr(input)
*       chromaBufferPtr is the buffer pointer of the candidate luma mode.
*   @param qp(input)
*       qp is the quantizer parameter.
*   @param luma_distortion (input)
*       luma_distortion is the inter condidate luma distortion.
*   @param lambda(input)
*       lambda is the Lagrange multiplier
**********************************************************************************/
EbErrorType av1_merge_skip_full_cost(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                                     struct ModeDecisionCandidateBuffer *candidate_buffer_ptr,
                                     BlkStruct *blk_ptr, uint64_t *y_distortion,
                                     uint64_t *cb_distortion, uint64_t *cr_distortion,
                                     uint64_t lambda, uint64_t *y_coeff_bits,
                                     uint64_t *cb_coeff_bits, uint64_t *cr_coeff_bits,
                                     BlockSize bsize) {
    UNUSED(bsize);
    UNUSED(context_ptr);
    UNUSED(pcs_ptr);

    EbErrorType return_error  = EB_ErrorNone;
    uint64_t    skip_mode_ctx = blk_ptr->skip_flag_context;
    uint64_t    merge_rate    = 0;
    uint64_t    skip_rate     = 0;
    // Merge
    //uint64_t mergeChromaRate;
    uint64_t merge_distortion;
    uint64_t merge_cost;
    //uint64_t mergeLumaCost;
    uint64_t merge_luma_sse;
    uint64_t merge_chroma_sse;
    uint64_t coeff_rate;
    //uint64_t lumaCoeffRate;

    // SKIP
    uint64_t skip_distortion;
    uint64_t skip_cost;
    //uint64_t skipLumaCost;

    // Luma and chroma transform size shift for the distortion
    uint64_t skip_luma_sse;
    uint64_t skip_chroma_sse;

    uint64_t skip_mode_rate = candidate_buffer_ptr->candidate_ptr->md_rate_estimation_ptr
                                  ->skip_mode_fac_bits[skip_mode_ctx][1];

    // Coeff rate
    coeff_rate = (*y_coeff_bits + *cb_coeff_bits + *cr_coeff_bits);

    // Compute Merge Cost
    merge_luma_sse   = y_distortion[0] << AV1_COST_PRECISION;
    merge_chroma_sse = (cb_distortion[0] + cr_distortion[0]) << AV1_COST_PRECISION;

    skip_luma_sse   = y_distortion[1] << AV1_COST_PRECISION;
    skip_chroma_sse = (cb_distortion[1] + cr_distortion[1]) << AV1_COST_PRECISION;

    // *Note - As in JCTVC-G1102, the JCT-VC uses the Mode Decision forumula where the chroma_sse has been weighted
    //  CostMode = (luma_sse + wchroma * chroma_sse) + lambda_sse * rateMode

    //if (pcs_ptr->parent_pcs_ptr->pred_structure == EB_PRED_RANDOM_ACCESS) {
    //    // Random Access
    //    if (pcs_ptr->temporal_layer_index == 0) {
    //        merge_chroma_sse = (((merge_chroma_sse * chroma_weight_factor_ra[qp]) + CHROMA_WEIGHT_OFFSET) >> CHROMA_WEIGHT_SHIFT);
    //    }
    //    else if (pcs_ptr->temporal_layer_index < 3) {
    //        merge_chroma_sse = (((merge_chroma_sse * chroma_weight_factor_ra_qp_scaling_l1[qp]) + CHROMA_WEIGHT_OFFSET) >> CHROMA_WEIGHT_SHIFT);
    //    }
    //    else {
    //        merge_chroma_sse = (((merge_chroma_sse * chroma_weight_factor_ra_qp_scaling_l3[qp]) + CHROMA_WEIGHT_OFFSET) >> CHROMA_WEIGHT_SHIFT);
    //    }
    //}
    //else {
    //    // Low delay
    //    if (pcs_ptr->temporal_layer_index == 0) {
    //        merge_chroma_sse = (((merge_chroma_sse * chroma_weight_factor_ld[qp]) + CHROMA_WEIGHT_OFFSET) >> CHROMA_WEIGHT_SHIFT);
    //    }
    //    else {
    //        merge_chroma_sse = (((merge_chroma_sse * chroma_weight_factor_ld_qp_scaling[qp]) + CHROMA_WEIGHT_OFFSET) >> CHROMA_WEIGHT_SHIFT);
    //    }
    //}

    // Add fast rate to get the total rate of the subject mode
    merge_rate += candidate_buffer_ptr->candidate_ptr->fast_luma_rate;
    merge_rate += candidate_buffer_ptr->candidate_ptr->fast_chroma_rate;

    merge_rate += coeff_rate;
    uint64_t tx_size_bits = 0;
    if (pcs_ptr->parent_pcs_ptr->frm_hdr.tx_mode == TX_MODE_SELECT)
        tx_size_bits = get_tx_size_bits(candidate_buffer_ptr,
                                        context_ptr,
                                        pcs_ptr,
                                        candidate_buffer_ptr->candidate_ptr->tx_depth,
                                        candidate_buffer_ptr->candidate_ptr->block_has_coeff);
    merge_rate += tx_size_bits;

    merge_distortion = (merge_luma_sse + merge_chroma_sse);

    //merge_cost = merge_distortion + (((lambda * coeff_rate + lambda * mergeLumaRate + lambda_chroma * mergeChromaRate) + MD_OFFSET) >> MD_SHIFT);

    merge_cost = RDCOST(lambda, merge_rate, merge_distortion);
    // mergeLumaCost = merge_luma_sse    + (((lambda * lumaCoeffRate + lambda * mergeLumaRate) + MD_OFFSET) >> MD_SHIFT);

    // *Note - As in JCTVC-G1102, the JCT-VC uses the Mode Decision forumula where the chroma_sse has been weighted
    //  CostMode = (luma_sse + wchroma * chroma_sse) + lambda_sse * rateMode

    //if (pcs_ptr->parent_pcs_ptr->pred_structure == EB_PRED_RANDOM_ACCESS) {
    //    if (pcs_ptr->temporal_layer_index == 0) {
    //        skip_chroma_sse = (((skip_chroma_sse * chroma_weight_factor_ra[qp]) + CHROMA_WEIGHT_OFFSET) >> CHROMA_WEIGHT_SHIFT);
    //    }
    //    else if (pcs_ptr->temporal_layer_index < 3) {
    //        skip_chroma_sse = (((skip_chroma_sse * chroma_weight_factor_ra_qp_scaling_l1[qp]) + CHROMA_WEIGHT_OFFSET) >> CHROMA_WEIGHT_SHIFT);
    //    }
    //    else {
    //        skip_chroma_sse = (((skip_chroma_sse * chroma_weight_factor_ra_qp_scaling_l3[qp]) + CHROMA_WEIGHT_OFFSET) >> CHROMA_WEIGHT_SHIFT);
    //    }
    //}
    //else {
    //    // Low Delay
    //    if (pcs_ptr->temporal_layer_index == 0) {
    //        skip_chroma_sse = (((skip_chroma_sse * chroma_weight_factor_ld[qp]) + CHROMA_WEIGHT_OFFSET) >> CHROMA_WEIGHT_SHIFT);
    //    }
    //    else {
    //        skip_chroma_sse = (((skip_chroma_sse * chroma_weight_factor_ld_qp_scaling[qp]) + CHROMA_WEIGHT_OFFSET) >> CHROMA_WEIGHT_SHIFT);
    //    }
    //}

    skip_distortion = skip_luma_sse + skip_chroma_sse;
    skip_rate       = skip_mode_rate;
    skip_cost       = RDCOST(lambda, skip_rate, skip_distortion);
    if (pcs_ptr->parent_pcs_ptr->scs_ptr->use_output_stat_file) {
        MvReferenceFrame ref_type[2];
        av1_set_ref_frame(ref_type, candidate_buffer_ptr->candidate_ptr->ref_frame_type);
        if ((candidate_buffer_ptr->candidate_ptr->is_compound &&
             (ref_type[0] != LAST_FRAME || ref_type[1] != BWDREF_FRAME)) ||
            (!candidate_buffer_ptr->candidate_ptr->is_compound &&
             (ref_type[0] != LAST_FRAME && ref_type[0] != BWDREF_FRAME))) {
            skip_cost += skip_cost * FIRST_PASS_COST_PENALTY / 100;
            merge_cost += merge_cost * FIRST_PASS_COST_PENALTY / 100;
        }
        EbReferenceObject *ref_obj_l1 =
            (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[REF_LIST_1][0]->object_ptr;
        if (pcs_ptr->slice_type == B_SLICE &&
            (candidate_buffer_ptr->candidate_ptr->is_compound || ref_type[0] == BWDREF_FRAME) &&
            ref_obj_l1->slice_type == I_SLICE && ref_obj_l1->ref_poc > pcs_ptr->picture_number) {
            skip_cost += skip_cost * 2;
            merge_cost += merge_cost * 2;
        }
    }
    // Assigne full cost
    *candidate_buffer_ptr->full_cost_ptr       = (skip_cost <= merge_cost) ? skip_cost : merge_cost;
    *candidate_buffer_ptr->full_cost_merge_ptr = merge_cost;
    *candidate_buffer_ptr->full_cost_skip_ptr  = skip_cost;
    // Assigne merge flag
    candidate_buffer_ptr->candidate_ptr->merge_flag = EB_TRUE;
    // Assigne skip flag

    candidate_buffer_ptr->candidate_ptr->skip_flag = (skip_cost <= merge_cost) ? EB_TRUE : EB_FALSE;

    //CHKN:  skip_flag context is not accurate as MD does not keep skip info in sync with EncDec.

    return return_error;
}
/*********************************************************************************
* av1_intra_full_cost function is used to estimate the cost of an intra candidate mode
* for full mode decisoion module.
*
*   @param *blk_ptr(input)
*       blk_ptr is the pointer of the target CU.
*   @param *candidate_buffer_ptr(input)
*       chromaBufferPtr is the buffer pointer of the candidate luma mode.
*   @param qp(input)
*       qp is the quantizer parameter.
*   @param luma_distortion (input)
*       luma_distortion is the intra condidate luma distortion.
*   @param lambda(input)
*       lambda is the Lagrange multiplier
**********************************************************************************/
EbErrorType av1_intra_full_cost(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                                struct ModeDecisionCandidateBuffer *candidate_buffer_ptr,
                                BlkStruct *blk_ptr, uint64_t *y_distortion,
                                uint64_t *cb_distortion, uint64_t *cr_distortion, uint64_t lambda,
                                uint64_t *y_coeff_bits, uint64_t *cb_coeff_bits,
                                uint64_t *cr_coeff_bits, BlockSize bsize)

{
    EbErrorType return_error = EB_ErrorNone;

    av1_full_cost(pcs_ptr,
                  context_ptr,
                  candidate_buffer_ptr,
                  blk_ptr,
                  y_distortion,
                  cb_distortion,
                  cr_distortion,
                  lambda,
                  y_coeff_bits,
                  cb_coeff_bits,
                  cr_coeff_bits,
                  bsize);

    return return_error;
}

/*********************************************************************************
* av1_inter_full_cost function is used to estimate the cost of an inter candidate mode
* for full mode decisoion module in inter frames.
*
*   @param *blk_ptr(input)
*       blk_ptr is the pointer of the target CU.
*   @param *candidate_buffer_ptr(input)
*       chromaBufferPtr is the buffer pointer of the candidate luma mode.
*   @param qp(input)
*       qp is the quantizer parameter.
*   @param luma_distortion (input)
*       luma_distortion is the inter condidate luma distortion.
*   @param lambda(input)
*       lambda is the Lagrange multiplier
**********************************************************************************/
EbErrorType av1_inter_full_cost(PictureControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                                struct ModeDecisionCandidateBuffer *candidate_buffer_ptr,
                                BlkStruct *blk_ptr, uint64_t *y_distortion,
                                uint64_t *cb_distortion, uint64_t *cr_distortion, uint64_t lambda,
                                uint64_t *y_coeff_bits, uint64_t *cb_coeff_bits,
                                uint64_t *cr_coeff_bits, BlockSize bsize) {
    EbErrorType return_error = EB_ErrorNone;

    if (candidate_buffer_ptr->candidate_ptr->merge_flag == EB_TRUE) {
        av1_merge_skip_full_cost(pcs_ptr,
                                 context_ptr,
                                 candidate_buffer_ptr,
                                 blk_ptr,
                                 y_distortion,
                                 cb_distortion,
                                 cr_distortion,
                                 lambda,
                                 y_coeff_bits,
                                 cb_coeff_bits,
                                 cr_coeff_bits,
                                 bsize);
    } else {
        av1_full_cost(pcs_ptr,
                      context_ptr,
                      candidate_buffer_ptr,
                      blk_ptr,
                      y_distortion,
                      cb_distortion,
                      cr_distortion,
                      lambda,
                      y_coeff_bits,
                      cb_coeff_bits,
                      cr_coeff_bits,
                      bsize);
    }
    return return_error;
}

/************************************************************
* Coding Loop Context Generation
************************************************************/
void coding_loop_context_generation(ModeDecisionContext *context_ptr, BlkStruct *blk_ptr,
                                    uint32_t blk_origin_x, uint32_t blk_origin_y, uint32_t sb_sz,
                                    NeighborArrayUnit *skip_coeff_neighbor_array,
                                    NeighborArrayUnit *inter_pred_dir_neighbor_array,
                                    NeighborArrayUnit *ref_frame_type_neighbor_array,
                                    NeighborArrayUnit *intra_luma_mode_neighbor_array,
                                    NeighborArrayUnit *skip_flag_neighbor_array,
                                    NeighborArrayUnit *mode_type_neighbor_array,
                                    NeighborArrayUnit *leaf_depth_neighbor_array,
                                    NeighborArrayUnit *leaf_partition_neighbor_array) {
    (void)sb_sz;
    UNUSED(ref_frame_type_neighbor_array);
    uint32_t mode_type_left_neighbor_index =
        get_neighbor_array_unit_left_index(mode_type_neighbor_array, blk_origin_y);
    uint32_t mode_type_top_neighbor_index =
        get_neighbor_array_unit_top_index(mode_type_neighbor_array, blk_origin_x);
    uint32_t leaf_depth_left_neighbor_index =
        get_neighbor_array_unit_left_index(leaf_depth_neighbor_array, blk_origin_y);
    uint32_t leaf_depth_top_neighbor_index =
        get_neighbor_array_unit_top_index(leaf_depth_neighbor_array, blk_origin_x);
    uint32_t skip_flag_left_neighbor_index =
        get_neighbor_array_unit_left_index(skip_flag_neighbor_array, blk_origin_y);
    uint32_t skip_flag_top_neighbor_index =
        get_neighbor_array_unit_top_index(skip_flag_neighbor_array, blk_origin_x);
    uint32_t intra_luma_mode_left_neighbor_index =
        get_neighbor_array_unit_left_index(intra_luma_mode_neighbor_array, blk_origin_y);
    uint32_t intra_luma_mode_top_neighbor_index =
        get_neighbor_array_unit_top_index(intra_luma_mode_neighbor_array, blk_origin_x);

    uint32_t partition_left_neighbor_index =
        get_neighbor_array_unit_left_index(leaf_partition_neighbor_array, blk_origin_y);
    uint32_t partition_above_neighbor_index =
        get_neighbor_array_unit_top_index(leaf_partition_neighbor_array, blk_origin_x);

    // Intra Luma Neighbor Modes

    blk_ptr->prediction_unit_array->intra_luma_left_mode = (uint32_t)(
        (mode_type_neighbor_array->left_array[mode_type_left_neighbor_index] != INTRA_MODE)
            ? (uint32_t)DC_PRED
            : intra_luma_mode_neighbor_array->left_array[intra_luma_mode_left_neighbor_index]);

    blk_ptr->prediction_unit_array->intra_luma_top_mode = (uint32_t)(
        (mode_type_neighbor_array->top_array[mode_type_top_neighbor_index] != INTRA_MODE)
            ? (uint32_t)DC_PRED
            : intra_luma_mode_neighbor_array->top_array[intra_luma_mode_top_neighbor_index]);

    int32_t context_index;
    if (mode_type_neighbor_array->left_array[mode_type_left_neighbor_index] !=
            (uint8_t)INVALID_MODE &&
        mode_type_neighbor_array->top_array[mode_type_top_neighbor_index] !=
            (uint8_t)INVALID_MODE) {
        context_index =
            (mode_type_neighbor_array->left_array[mode_type_left_neighbor_index] ==
                 (uint8_t)INTRA_MODE &&
             mode_type_neighbor_array->top_array[mode_type_top_neighbor_index] ==
                 (uint8_t)INTRA_MODE)
                ? 3
                : (mode_type_neighbor_array->left_array[mode_type_left_neighbor_index] ==
                       (uint8_t)INTRA_MODE ||
                   mode_type_neighbor_array->top_array[mode_type_top_neighbor_index] ==
                       (uint8_t)INTRA_MODE)
                      ? 1
                      : 0;
    } else if (mode_type_neighbor_array->left_array[mode_type_left_neighbor_index] !=
               (uint8_t)INVALID_MODE)
        context_index = (mode_type_neighbor_array->left_array[mode_type_left_neighbor_index] ==
                         (uint8_t)INTRA_MODE)
                            ? 2
                            : 0;
    else if (mode_type_neighbor_array->top_array[mode_type_top_neighbor_index] !=
             (uint8_t)INVALID_MODE)
        context_index = (mode_type_neighbor_array->top_array[mode_type_top_neighbor_index] ==
                         (uint8_t)INTRA_MODE)
                            ? 2
                            : 0;
    else
        context_index = 0;
    blk_ptr->is_inter_ctx = context_index;
    //  if(blk_ptr->is_inter_ctx!=0) //
    //      SVT_LOG("ctx:%i \n",blk_ptr->is_inter_ctx);

    //   Top Intra Mode Neighbor Array instead of a Full
    // Skip Flag Context
    blk_ptr->skip_flag_context =
        (mode_type_neighbor_array->left_array[mode_type_left_neighbor_index] ==
         (uint8_t)INVALID_MODE)
            ? 0
            : (skip_flag_neighbor_array->left_array[skip_flag_left_neighbor_index] == EB_TRUE) ? 1
                                                                                               : 0;
    blk_ptr->skip_flag_context +=
        (mode_type_neighbor_array->top_array[mode_type_top_neighbor_index] == (uint8_t)INVALID_MODE)
            ? 0
            : (skip_flag_neighbor_array->top_array[skip_flag_top_neighbor_index] == EB_TRUE) ? 1
                                                                                             : 0;

    // Split Flag Context (neighbor info)
    context_ptr->md_local_blk_unit[blk_ptr->mds_idx].left_neighbor_mode = (uint32_t)(
        (mode_type_neighbor_array->left_array[mode_type_left_neighbor_index] != INTRA_MODE)
            ? (uint32_t)DC_PRED
            : intra_luma_mode_neighbor_array->left_array[intra_luma_mode_left_neighbor_index]);
    context_ptr->md_local_blk_unit[blk_ptr->mds_idx].left_neighbor_depth =
        leaf_depth_neighbor_array->left_array[leaf_depth_left_neighbor_index];
    context_ptr->md_local_blk_unit[blk_ptr->mds_idx].top_neighbor_mode = (uint32_t)(
        (mode_type_neighbor_array->top_array[mode_type_top_neighbor_index] != INTRA_MODE)
            ? (uint32_t)DC_PRED
            : intra_luma_mode_neighbor_array->top_array[intra_luma_mode_top_neighbor_index]);
    context_ptr->md_local_blk_unit[blk_ptr->mds_idx].top_neighbor_depth =
        leaf_depth_neighbor_array->top_array[leaf_depth_top_neighbor_index];

    // Generate Partition context
    context_ptr->md_local_blk_unit[blk_ptr->mds_idx].above_neighbor_partition =
        (((PartitionContext *)
              leaf_partition_neighbor_array->top_array)[partition_above_neighbor_index]
             .above == (int8_t)INVALID_NEIGHBOR_DATA)
            ? 0
            : ((PartitionContext *)
                   leaf_partition_neighbor_array->top_array)[partition_above_neighbor_index]
                  .above;

    context_ptr->md_local_blk_unit[blk_ptr->mds_idx].left_neighbor_partition =
        (((PartitionContext *)
              leaf_partition_neighbor_array->left_array)[partition_left_neighbor_index]
             .left == (int8_t)INVALID_NEIGHBOR_DATA)
            ? 0
            : ((PartitionContext *)
                   leaf_partition_neighbor_array->left_array)[partition_left_neighbor_index]
                  .left;
    // Skip Coeff AV1 Context
    uint32_t skip_coeff_left_neighbor_index =
        get_neighbor_array_unit_left_index(skip_coeff_neighbor_array, blk_origin_y);
    uint32_t skip_coeff_top_neighbor_index =
        get_neighbor_array_unit_top_index(skip_coeff_neighbor_array, blk_origin_x);

    blk_ptr->skip_coeff_context =
        (skip_coeff_neighbor_array->left_array[skip_coeff_left_neighbor_index] ==
         (uint8_t)INVALID_NEIGHBOR_DATA)
            ? 0
            : (skip_coeff_neighbor_array->left_array[skip_coeff_left_neighbor_index]) ? 1 : 0;

    blk_ptr->skip_coeff_context +=
        (skip_coeff_neighbor_array->top_array[skip_coeff_top_neighbor_index] ==
         (uint8_t)INVALID_NEIGHBOR_DATA)
            ? 0
            : (skip_coeff_neighbor_array->top_array[skip_coeff_top_neighbor_index]) ? 1 : 0;
    // Generate reference mode context

    blk_ptr->reference_mode_context = (uint8_t)eb_av1_get_reference_mode_context(
        blk_origin_x, blk_origin_y, mode_type_neighbor_array, inter_pred_dir_neighbor_array);

    blk_ptr->compoud_reference_type_context = (uint8_t)eb_av1_get_comp_reference_type_context(
        blk_origin_x, blk_origin_y, mode_type_neighbor_array, inter_pred_dir_neighbor_array);

    //Collect Neighbor ref cout
    av1_collect_neighbors_ref_counts_new(blk_ptr->av1xd);

    return;
}

/********************************************
* txb_calc_cost
*   computes TU Cost and generetes TU Cbf
********************************************/
EbErrorType av1_txb_calc_cost(
    ModeDecisionCandidate *candidate_ptr, // input parameter, prediction result Ptr
    int16_t                txb_skip_ctx,
    uint32_t               txb_index, // input parameter, TU index inside the CU
    uint32_t
        y_count_non_zero_coeffs, // input parameter, number of non zero Y quantized coefficients
    uint32_t
        cb_count_non_zero_coeffs, // input parameter, number of non zero cb quantized coefficients
    uint32_t
             cr_count_non_zero_coeffs, // input parameter, number of non zero cr quantized coefficients
    uint64_t y_txb_distortion
        [DIST_CALC_TOTAL], // input parameter, Y distortion for both Normal and Cbf zero modes
    uint64_t cb_txb_distortion
        [DIST_CALC_TOTAL], // input parameter, Cb distortion for both Normal and Cbf zero modes
    uint64_t cr_txb_distortion
        [DIST_CALC_TOTAL], // input parameter, Cr distortion for both Normal and Cbf zero modes
    COMPONENT_TYPE component_type,
    uint64_t *     y_txb_coeff_bits, // input parameter, Y quantized coefficients rate
    uint64_t *     cb_txb_coeff_bits, // input parameter, Cb quantized coefficients rate
    uint64_t *     cr_txb_coeff_bits, // input parameter, Cr quantized coefficients rate
    TxSize         txsize,
    uint64_t       lambda) // input parameter, lambda for Luma

{
    (void)txsize;
    (void)txb_skip_ctx;
    (void)cr_txb_coeff_bits;
    (void)cb_txb_coeff_bits;
    (void)cr_txb_distortion;
    (void)cb_txb_distortion;
    EbErrorType return_error = EB_ErrorNone;
    // Non zero coeff mode variables
    uint64_t y_nonzero_coeff_distortion = y_txb_distortion[DIST_CALC_RESIDUAL];
    uint64_t y_nonzero_coeff_rate;

    uint64_t y_nonzero_coeff_cost = 0;

    // zero Cbf mode variables
    uint64_t y_zero_coeff_cost = 0;

    if (component_type == COMPONENT_LUMA || component_type == COMPONENT_ALL) {
        // Non zero Distortion
        // *Note - As of Oct 2011, the JCT-VC uses the PSNR forumula
        //  PSNR = (LUMA_WEIGHT * PSNRy + PSNRu + PSNRv) / (2+LUMA_WEIGHT)
        y_nonzero_coeff_distortion =
            LUMA_WEIGHT * (y_nonzero_coeff_distortion << AV1_COST_PRECISION);

        // Esimate Cbf's Bits
        y_nonzero_coeff_rate =
            *y_txb_coeff_bits; // yNonZeroCbfLumaFlagBitsNum is already calculated inside y_txb_coeff_bits
        y_zero_coeff_cost = 0xFFFFFFFFFFFFFFFFull;
        // **Compute Cost
        y_nonzero_coeff_cost = RDCOST(lambda, y_nonzero_coeff_rate, y_nonzero_coeff_distortion);
        candidate_ptr->y_has_coeff |=
            (((y_count_non_zero_coeffs != 0) && (y_nonzero_coeff_cost < y_zero_coeff_cost))
             << txb_index);
        *y_txb_coeff_bits = (y_nonzero_coeff_cost < y_zero_coeff_cost) ? *y_txb_coeff_bits : 0;
        y_txb_distortion[DIST_CALC_RESIDUAL] = (y_nonzero_coeff_cost < y_zero_coeff_cost)
                                                   ? y_txb_distortion[DIST_CALC_RESIDUAL]
                                                   : y_txb_distortion[DIST_CALC_PREDICTION];
    }
    if (component_type == COMPONENT_CHROMA_CB || component_type == COMPONENT_CHROMA ||
        component_type == COMPONENT_ALL)
        candidate_ptr->u_has_coeff |= ((cb_count_non_zero_coeffs != 0) << txb_index);
    if (component_type == COMPONENT_CHROMA_CR || component_type == COMPONENT_CHROMA ||
        component_type == COMPONENT_ALL)
        candidate_ptr->v_has_coeff |= ((cr_count_non_zero_coeffs != 0) << txb_index);
    return return_error;
}

/********************************************
* txb_calc_cost
*   computes TU Cost and generetes TU Cbf
********************************************/

EbErrorType av1_txb_calc_cost_luma(
    int16_t                txb_skip_ctx,
    ModeDecisionCandidate *candidate_ptr, // input parameter, prediction result Ptr
    uint32_t               txb_index, // input parameter, TU index inside the CU
    TxSize                 tx_size,
    uint32_t
             y_count_non_zero_coeffs, // input parameter, number of non zero Y quantized coefficients
    uint64_t y_txb_distortion
        [DIST_CALC_TOTAL], // input parameter, Y distortion for both Normal and Cbf zero modes
    uint64_t *y_txb_coeff_bits, // input parameter, Y quantized coefficients rate
    uint64_t *y_full_cost,
    uint64_t  lambda) // input parameter, lambda for Luma

{
    (void)txb_skip_ctx;
    (void)tx_size;
    EbErrorType return_error = EB_ErrorNone;

    // Non zero Cbf mode variables
    uint64_t y_non_zero_cbf_distortion = y_txb_distortion[DIST_CALC_RESIDUAL];
    uint64_t y_non_zero_cbf_rate;
    uint64_t y_non_zero_cbf_cost = 0;

    // zero Cbf mode variables
    uint64_t y_zero_cbf_cost = 0;

    // **Compute distortion
    // Non zero Distortion
    // *Note - As of Oct 2011, the JCT-VC uses the PSNR forumula
    //  PSNR = (LUMA_WEIGHT * PSNRy + PSNRu + PSNRv) / (2+LUMA_WEIGHT)
    y_non_zero_cbf_distortion = LUMA_WEIGHT * (y_non_zero_cbf_distortion << AV1_COST_PRECISION);

    // zero distortion
    y_non_zero_cbf_rate =
        *y_txb_coeff_bits; // yNonZeroCbfLumaFlagBitsNum is already calculated inside y_txb_coeff_bits
    y_zero_cbf_cost = 0xFFFFFFFFFFFFFFFFull;
    // **Compute Cost
    y_non_zero_cbf_cost = RDCOST(lambda, y_non_zero_cbf_rate, y_non_zero_cbf_distortion);
    candidate_ptr->y_has_coeff |= ((y_count_non_zero_coeffs != 0) << txb_index);
    *y_txb_coeff_bits = (y_non_zero_cbf_cost < y_zero_cbf_cost) ? *y_txb_coeff_bits : 0;
    y_txb_distortion[DIST_CALC_RESIDUAL] = (y_non_zero_cbf_cost < y_zero_cbf_cost)
                                               ? y_txb_distortion[DIST_CALC_RESIDUAL]
                                               : y_txb_distortion[DIST_CALC_PREDICTION];
    *y_full_cost = MIN(y_non_zero_cbf_cost, y_zero_cbf_cost);
    return return_error;
}

//static INLINE int32_t partition_plane_context(const MacroBlockD *xd, int32_t mi_row,
//    int32_t mi_col, BlockSize bsize) {
//    const PartitionContextType *above_ctx = xd->above_seg_context + mi_col;
//    const PartitionContextType *left_ctx =
//        xd->left_seg_context + (mi_row & MAX_MIB_MASK);
//    // Minimum partition point is 8x8. Offset the bsl accordingly.
//    const int32_t bsl = mi_size_wide_log2[bsize] - mi_size_wide_log2[BLOCK_8X8];
//    int32_t above = (*above_ctx >> bsl) & 1, left = (*left_ctx >> bsl) & 1;
//
//    assert(mi_size_wide_log2[bsize] == mi_size_high_log2[bsize]);
//    assert(bsl >= 0);
//
//    return (left * 2 + above) + bsl * PARTITION_PLOFFSET;
//}

/*********************************************************************************
* split_flag_rate function is used to generate the Split rate
*
*   @param *blk_ptr(input)
*       blk_ptr is the pointer of the target CU.
*   @param split_flag(input)
*       split_flag is the split flag value.
*   @param split_rate(output)
*       split_rate contains rate.
*   @param lambda(input)
*       lambda is the Lagrange multiplier
*   @param md_rate_estimation_ptr(input)
*       md_rate_estimation_ptr is pointer to MD rate Estimation Tables
**********************************************************************************/
EbErrorType av1_split_flag_rate(PictureParentControlSet *pcs_ptr, ModeDecisionContext *context_ptr,
                                BlkStruct *blk_ptr, uint32_t leaf_index,
                                PartitionType partitionType, uint64_t *split_rate, uint64_t lambda,
                                MdRateEstimationContext *md_rate_estimation_ptr,
                                uint32_t                 tb_max_depth) {
    (void)tb_max_depth;
    (void)leaf_index;

    const BlockGeom *blk_geom     = get_blk_geom_mds(blk_ptr->mds_idx);
    EbErrorType      return_error = EB_ErrorNone;

    uint32_t blk_origin_x = context_ptr->sb_origin_x + blk_geom->origin_x;
    uint32_t blk_origin_y = context_ptr->sb_origin_y + blk_geom->origin_y;

    PartitionType p = partitionType;

    uint32_t cu_depth = blk_geom->depth;
    UNUSED(cu_depth);
    BlockSize bsize = blk_geom->bsize;
    assert(bsize < BlockSizeS_ALL);
    const int32_t is_partition_point = blk_geom->bsize >= BLOCK_8X8;

    if (is_partition_point) {
        const int32_t hbs      = (mi_size_wide[bsize] << 2) >> 1;
        const int32_t has_rows = (blk_origin_y + hbs) < pcs_ptr->aligned_height;
        const int32_t has_cols = (blk_origin_x + hbs) < pcs_ptr->aligned_width;

        uint32_t context_index = 0;

        const PartitionContextType left_ctx =
            context_ptr->md_local_blk_unit[blk_ptr->mds_idx].left_neighbor_partition ==
                    (int8_t)(INVALID_NEIGHBOR_DATA)
                ? 0
                : context_ptr->md_local_blk_unit[blk_ptr->mds_idx].left_neighbor_partition;
        const PartitionContextType above_ctx =
            context_ptr->md_local_blk_unit[blk_ptr->mds_idx].above_neighbor_partition ==
                    (int8_t)(INVALID_NEIGHBOR_DATA)
                ? 0
                : context_ptr->md_local_blk_unit[blk_ptr->mds_idx].above_neighbor_partition;

        const int32_t bsl = mi_size_wide_log2[bsize] - mi_size_wide_log2[BLOCK_8X8];

        int32_t above = (above_ctx >> bsl) & 1, left = (left_ctx >> bsl) & 1;

        assert(mi_size_wide_log2[bsize] == mi_size_high_log2[bsize]);
        assert(bsl >= 0);

        context_index = (left * 2 + above) + bsl * PARTITION_PLOFFSET;

        if (has_rows && has_cols) {
            *split_rate =
                (uint64_t)md_rate_estimation_ptr->partition_fac_bits[context_index][partitionType];

        } else if (!has_rows && has_cols) {
            *split_rate =
                (uint64_t)md_rate_estimation_ptr->partition_fac_bits[2][p == PARTITION_SPLIT];

        } else {
            *split_rate =
                (uint64_t)md_rate_estimation_ptr->partition_fac_bits[2][p == PARTITION_SPLIT];
        }
    } else
        *split_rate = (uint64_t)md_rate_estimation_ptr->partition_fac_bits[0][partitionType];
    *split_rate = RDCOST(lambda, *split_rate, 0);

    return return_error;
}

/********************************************
* txb_calc_cost
*   Computes TU Cost and generetes TU Cbf
*   at the level of the encode pass
********************************************/
EbErrorType av1_encode_txb_calc_cost(EncDecContext *context_ptr, uint32_t *count_non_zero_coeffs,
                                     uint64_t  y_txb_distortion[DIST_CALC_TOTAL],
                                     uint64_t *y_txb_coeff_bits, uint32_t component_mask) {
    BlkStruct *             blk_ptr                  = context_ptr->blk_ptr;
    uint32_t                 txb_index                = context_ptr->txb_itr;
    MdRateEstimationContext *md_rate_estimation_ptr   = context_ptr->md_rate_estimation_ptr;
    uint64_t                 lambda                   = context_ptr->full_lambda;
    uint32_t                 y_count_non_zero_coeffs  = count_non_zero_coeffs[0];
    uint32_t                 cb_count_non_zero_coeffs = count_non_zero_coeffs[1];
    uint32_t                 cr_count_non_zero_coeffs = count_non_zero_coeffs[2];

    EbErrorType return_error = EB_ErrorNone;

    // Non zero Cbf mode variables
    uint64_t y_non_zero_cbf_distortion = y_txb_distortion[DIST_CALC_RESIDUAL];

    uint64_t y_non_zero_cbf_rate;

    uint64_t y_non_zero_cbf_cost = 0;

    // zero Cbf mode variables
    uint64_t y_zero_cbf_distortion = y_txb_distortion[DIST_CALC_PREDICTION];

    uint64_t y_zero_cbf_luma_flag_bits_num = 0;

    uint64_t y_zero_cbf_rate;

    uint64_t y_zero_cbf_cost = 0;
    int16_t  txb_skip_ctx    = context_ptr->md_context->luma_txb_skip_context;
    // **Compute distortion
    if (component_mask == PICTURE_BUFFER_DESC_LUMA_MASK ||
        component_mask == PICTURE_BUFFER_DESC_FULL_MASK) {
        // Non zero Distortion
        // *Note - As of Oct 2011, the JCT-VC uses the PSNR forumula
        //  PSNR = (LUMA_WEIGHT * PSNRy + PSNRu + PSNRv) / (2+LUMA_WEIGHT)
        y_non_zero_cbf_distortion = LUMA_WEIGHT * (y_non_zero_cbf_distortion << AV1_COST_PRECISION);

        // zero distortion
        // *Note - As of Oct 2011, the JCT-VC uses the PSNR forumula
        //  PSNR = (LUMA_WEIGHT * PSNRy + PSNRu + PSNRv) / (2+LUMA_WEIGHT)
        y_zero_cbf_distortion = LUMA_WEIGHT * (y_zero_cbf_distortion << AV1_COST_PRECISION);
        TxSize tx_size = context_ptr->blk_geom->txsize[blk_ptr->tx_depth][context_ptr->txb_itr];
        assert(tx_size < TX_SIZES_ALL);

        const TxSize txs_ctx =
            (TxSize)((txsize_sqr_map[tx_size] + txsize_sqr_up_map[tx_size] + 1) >> 1);
        assert(txs_ctx < TX_SIZES);
        const LvMapCoeffCost *const coeff_costs =
            &md_rate_estimation_ptr->coeff_fac_bits[txs_ctx][0];

        y_zero_cbf_luma_flag_bits_num = coeff_costs->txb_skip_cost[txb_skip_ctx][1];

        y_non_zero_cbf_rate =
            *y_txb_coeff_bits; // yNonZeroCbfLumaFlagBitsNum is already calculated inside y_txb_coeff_bits

        y_zero_cbf_rate        = y_zero_cbf_luma_flag_bits_num;
        TransformUnit *txb_ptr = &blk_ptr->txb_array[context_ptr->txb_itr];
        if (txb_ptr->transform_type[PLANE_TYPE_Y] != DCT_DCT) {
            y_zero_cbf_cost = 0xFFFFFFFFFFFFFFFFull;
        } else
            y_zero_cbf_cost = RDCOST(lambda, y_zero_cbf_rate, y_zero_cbf_distortion);
        // **Compute Cost
        y_non_zero_cbf_cost = RDCOST(lambda, y_non_zero_cbf_rate, y_non_zero_cbf_distortion);
        blk_ptr->txb_array[txb_index].y_has_coeff =
            ((y_count_non_zero_coeffs != 0) && (y_non_zero_cbf_cost < y_zero_cbf_cost)) ? EB_TRUE
                                                                                        : EB_FALSE;
        *y_txb_coeff_bits = (y_non_zero_cbf_cost < y_zero_cbf_cost) ? *y_txb_coeff_bits : 0;
        y_txb_distortion[DIST_CALC_RESIDUAL] = (y_non_zero_cbf_cost < y_zero_cbf_cost)
                                                   ? y_txb_distortion[DIST_CALC_RESIDUAL]
                                                   : y_txb_distortion[DIST_CALC_PREDICTION];
    } else
        blk_ptr->txb_array[txb_index].y_has_coeff = EB_FALSE;
    blk_ptr->txb_array[txb_index].u_has_coeff = cb_count_non_zero_coeffs != 0 ? EB_TRUE : EB_FALSE;
    blk_ptr->txb_array[txb_index].v_has_coeff = cr_count_non_zero_coeffs != 0 ? EB_TRUE : EB_FALSE;

    return return_error;
}
