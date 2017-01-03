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

#include <limits.h>

#include "av1/encoder/encoder.h"
#include "av1/encoder/speed_features.h"
#include "av1/encoder/rdopt.h"

#include "aom_dsp/aom_dsp_common.h"

// Mesh search patters for various speed settings
static MESH_PATTERN best_quality_mesh_pattern[MAX_MESH_STEP] = {
  { 64, 4 }, { 28, 2 }, { 15, 1 }, { 7, 1 }
};

#define MAX_MESH_SPEED 5  // Max speed setting for mesh motion method
static MESH_PATTERN
    good_quality_mesh_patterns[MAX_MESH_SPEED + 1][MAX_MESH_STEP] = {
      { { 64, 8 }, { 28, 4 }, { 15, 1 }, { 7, 1 } },
      { { 64, 8 }, { 28, 4 }, { 15, 1 }, { 7, 1 } },
      { { 64, 8 }, { 14, 2 }, { 7, 1 }, { 7, 1 } },
      { { 64, 16 }, { 24, 8 }, { 12, 4 }, { 7, 1 } },
      { { 64, 16 }, { 24, 8 }, { 12, 4 }, { 7, 1 } },
      { { 64, 16 }, { 24, 8 }, { 12, 4 }, { 7, 1 } },
    };
static unsigned char good_quality_max_mesh_pct[MAX_MESH_SPEED + 1] = {
  50, 25, 15, 5, 1, 1
};

// Intra only frames, golden frames (except alt ref overlays) and
// alt ref frames tend to be coded at a higher than ambient quality
static int frame_is_boosted(const AV1_COMP *cpi) {
  return frame_is_kf_gf_arf(cpi);
}

// Sets a partition size down to which the auto partition code will always
// search (can go lower), based on the image dimensions. The logic here
// is that the extent to which ringing artefacts are offensive, depends
// partly on the screen area that over which they propogate. Propogation is
// limited by transform block size but the screen area take up by a given block
// size will be larger for a small image format stretched to full screen.
static BLOCK_SIZE set_partition_min_limit(AV1_COMMON *const cm) {
  unsigned int screen_area = (cm->width * cm->height);

  // Select block size based on image format size.
  if (screen_area < 1280 * 720) {
    // Formats smaller in area than 720P
    return BLOCK_4X4;
  } else if (screen_area < 1920 * 1080) {
    // Format >= 720P and < 1080P
    return BLOCK_8X8;
  } else {
    // Formats 1080P and up
    return BLOCK_16X16;
  }
}

static void set_good_speed_feature_framesize_dependent(AV1_COMP *cpi,
                                                       SPEED_FEATURES *sf,
                                                       int speed) {
  AV1_COMMON *const cm = &cpi->common;

  if (speed >= 1) {
    if (AOMMIN(cm->width, cm->height) >= 720) {
      sf->disable_split_mask =
          cm->show_frame ? DISABLE_ALL_SPLIT : DISABLE_ALL_INTER_SPLIT;
      sf->partition_search_breakout_dist_thr = (1 << 23);
    } else {
      sf->disable_split_mask = DISABLE_COMPOUND_SPLIT;
      sf->partition_search_breakout_dist_thr = (1 << 21);
    }
  }

  if (speed >= 2) {
    if (AOMMIN(cm->width, cm->height) >= 720) {
      sf->disable_split_mask =
          cm->show_frame ? DISABLE_ALL_SPLIT : DISABLE_ALL_INTER_SPLIT;
      sf->adaptive_pred_interp_filter = 0;
      sf->partition_search_breakout_dist_thr = (1 << 24);
      sf->partition_search_breakout_rate_thr = 120;
    } else {
      sf->disable_split_mask = LAST_AND_INTRA_SPLIT_ONLY;
      sf->partition_search_breakout_dist_thr = (1 << 22);
      sf->partition_search_breakout_rate_thr = 100;
    }
    sf->rd_auto_partition_min_limit = set_partition_min_limit(cm);
  }

  if (speed >= 3) {
    if (AOMMIN(cm->width, cm->height) >= 720) {
      sf->disable_split_mask = DISABLE_ALL_SPLIT;
      sf->schedule_mode_search = cm->base_qindex < 220 ? 1 : 0;
      sf->partition_search_breakout_dist_thr = (1 << 25);
      sf->partition_search_breakout_rate_thr = 200;
    } else {
      sf->max_intra_bsize = BLOCK_32X32;
      sf->disable_split_mask = DISABLE_ALL_INTER_SPLIT;
      sf->schedule_mode_search = cm->base_qindex < 175 ? 1 : 0;
      sf->partition_search_breakout_dist_thr = (1 << 23);
      sf->partition_search_breakout_rate_thr = 120;
    }
  }

  // If this is a two pass clip that fits the criteria for animated or
  // graphics content then reset disable_split_mask for speeds 1-4.
  // Also if the image edge is internal to the coded area.
  if ((speed >= 1) && (cpi->oxcf.pass == 2) &&
      ((cpi->twopass.fr_content_type == FC_GRAPHICS_ANIMATION) ||
       (av1_internal_image_edge(cpi)))) {
    sf->disable_split_mask = DISABLE_COMPOUND_SPLIT;
  }

  if (speed >= 4) {
    if (AOMMIN(cm->width, cm->height) >= 720) {
      sf->partition_search_breakout_dist_thr = (1 << 26);
    } else {
      sf->partition_search_breakout_dist_thr = (1 << 24);
    }
    sf->disable_split_mask = DISABLE_ALL_SPLIT;
  }
}

static void set_good_speed_feature(AV1_COMP *cpi, AV1_COMMON *cm,
                                   SPEED_FEATURES *sf, int speed) {
  const int boosted = frame_is_boosted(cpi);

  if (speed >= 1) {
    sf->tx_type_search.fast_intra_tx_type_search = 1;
    sf->tx_type_search.fast_inter_tx_type_search = 1;
  }

  if (speed >= 2) {
    if ((cpi->twopass.fr_content_type == FC_GRAPHICS_ANIMATION) ||
        av1_internal_image_edge(cpi)) {
      sf->use_square_partition_only = !frame_is_boosted(cpi);
    } else {
      sf->use_square_partition_only = !frame_is_intra_only(cm);
    }

    sf->less_rectangular_check = 1;

    sf->use_rd_breakout = 1;
    sf->adaptive_motion_search = 1;
    sf->mv.auto_mv_step_size = 1;
    sf->adaptive_rd_thresh = 1;
    sf->mv.subpel_iters_per_step = 1;
    sf->mode_skip_start = 10;
    sf->adaptive_pred_interp_filter = 1;

    sf->recode_loop = ALLOW_RECODE_KFARFGF;
#if CONFIG_TX64X64
    sf->intra_y_mode_mask[TX_64X64] = INTRA_DC_H_V;
    sf->intra_uv_mode_mask[TX_64X64] = INTRA_DC_H_V;
#endif  // CONFIG_TX64X64
    sf->intra_y_mode_mask[TX_32X32] = INTRA_DC_H_V;
    sf->intra_uv_mode_mask[TX_32X32] = INTRA_DC_H_V;
    sf->intra_y_mode_mask[TX_16X16] = INTRA_DC_H_V;
    sf->intra_uv_mode_mask[TX_16X16] = INTRA_DC_H_V;

    sf->tx_size_search_breakout = 1;
    sf->partition_search_breakout_rate_thr = 80;
    sf->tx_type_search.prune_mode = PRUNE_ONE;
    // Use transform domain distortion.
    // Note var-tx expt always uses pixel domain distortion.
    sf->use_transform_domain_distortion = 1;
#if CONFIG_EXT_INTER
    sf->disable_wedge_search_var_thresh = 100;
    sf->fast_wedge_sign_estimate = 1;
#endif  // CONFIG_EXT_INTER
  }

  if (speed >= 3) {
    sf->tx_size_search_method =
        frame_is_boosted(cpi) ? USE_FULL_RD : USE_LARGESTALL;
    sf->mode_search_skip_flags =
        (cm->frame_type == KEY_FRAME) ? 0 : FLAG_SKIP_INTRA_DIRMISMATCH |
                                                FLAG_SKIP_INTRA_BESTINTER |
                                                FLAG_SKIP_COMP_BESTINTRA |
                                                FLAG_SKIP_INTRA_LOWVAR;
    sf->disable_filter_search_var_thresh = 100;
    sf->comp_inter_joint_search_thresh = BLOCK_SIZES;
    sf->auto_min_max_partition_size = RELAXED_NEIGHBORING_MIN_MAX;
    sf->allow_partition_search_skip = 1;
    sf->use_upsampled_references = 0;
    sf->adaptive_rd_thresh = 2;
#if CONFIG_EXT_TX
    sf->tx_type_search.prune_mode = PRUNE_TWO;
#endif
  }

  if (speed >= 4) {
    sf->use_square_partition_only = !frame_is_intra_only(cm);
    sf->tx_size_search_method =
        frame_is_intra_only(cm) ? USE_FULL_RD : USE_LARGESTALL;
    sf->mv.subpel_search_method = SUBPEL_TREE_PRUNED;
    sf->adaptive_pred_interp_filter = 0;
    sf->adaptive_mode_search = 1;
    sf->cb_partition_search = !boosted;
    sf->cb_pred_filter_search = 1;
    sf->alt_ref_search_fp = 1;
    sf->recode_loop = ALLOW_RECODE_KFMAXBW;
    sf->adaptive_rd_thresh = 3;
    sf->mode_skip_start = 6;
#if CONFIG_TX64X64
    sf->intra_y_mode_mask[TX_64X64] = INTRA_DC;
    sf->intra_uv_mode_mask[TX_64X64] = INTRA_DC;
#endif  // CONFIG_TX64X64
    sf->intra_y_mode_mask[TX_32X32] = INTRA_DC;
    sf->intra_uv_mode_mask[TX_32X32] = INTRA_DC;
    sf->adaptive_interp_filter_search = 1;
  }

  if (speed >= 5) {
    sf->use_square_partition_only = 1;
    sf->tx_size_search_method = USE_LARGESTALL;
    sf->mv.search_method = BIGDIA;
    sf->mv.subpel_search_method = SUBPEL_TREE_PRUNED_MORE;
    sf->adaptive_rd_thresh = 4;
    if (cm->frame_type != KEY_FRAME)
      sf->mode_search_skip_flags |= FLAG_EARLY_TERMINATE;
    sf->disable_filter_search_var_thresh = 200;
    sf->use_fast_coef_updates = ONE_LOOP_REDUCED;
    sf->use_fast_coef_costing = 1;
    sf->partition_search_breakout_rate_thr = 300;
  }

  if (speed >= 6) {
    int i;
    sf->optimize_coefficients = 0;
    sf->mv.search_method = HEX;
    sf->disable_filter_search_var_thresh = 500;
    for (i = 0; i < TX_SIZES; ++i) {
      sf->intra_y_mode_mask[i] = INTRA_DC;
      sf->intra_uv_mode_mask[i] = INTRA_DC;
    }
    sf->partition_search_breakout_rate_thr = 500;
    sf->mv.reduce_first_step_size = 1;
    sf->simple_model_rd_from_var = 1;
  }
}

static void set_rt_speed_feature_framesize_dependent(AV1_COMP *cpi,
                                                     SPEED_FEATURES *sf,
                                                     int speed) {
  AV1_COMMON *const cm = &cpi->common;
  if (speed >= 1) {
    if (AOMMIN(cm->width, cm->height) >= 720) {
      sf->disable_split_mask =
          cm->show_frame ? DISABLE_ALL_SPLIT : DISABLE_ALL_INTER_SPLIT;
    } else {
      sf->disable_split_mask = DISABLE_COMPOUND_SPLIT;
    }
  }

  if (speed >= 2) {
    if (AOMMIN(cm->width, cm->height) >= 720) {
      sf->disable_split_mask =
          cm->show_frame ? DISABLE_ALL_SPLIT : DISABLE_ALL_INTER_SPLIT;
    } else {
      sf->disable_split_mask = LAST_AND_INTRA_SPLIT_ONLY;
    }
  }

  if (speed >= 5) {
    if (AOMMIN(cm->width, cm->height) >= 720) {
      sf->partition_search_breakout_dist_thr = (1 << 25);
    } else {
      sf->partition_search_breakout_dist_thr = (1 << 23);
    }
  }
}

static void set_rt_speed_feature(AV1_COMP *cpi, SPEED_FEATURES *sf, int speed,
                                 aom_tune_content content) {
  AV1_COMMON *const cm = &cpi->common;
  const int is_keyframe = cm->frame_type == KEY_FRAME;
  const int frames_since_key = is_keyframe ? 0 : cpi->rc.frames_since_key;
  sf->static_segmentation = 0;
  sf->adaptive_rd_thresh = 1;
  sf->use_fast_coef_costing = 1;
  sf->allow_exhaustive_searches = 0;
  sf->exhaustive_searches_thresh = INT_MAX;
  sf->use_upsampled_references = 0;
#if CONFIG_EXT_INTER
  sf->disable_wedge_search_var_thresh = 100;
  sf->fast_wedge_sign_estimate = 1;
#endif  // CONFIG_EXT_INTER

  // Use transform domain distortion computation
  // Note var-tx expt always uses pixel domain distortion.
  sf->use_transform_domain_distortion = 1;

  if (speed >= 1) {
    sf->use_square_partition_only = !frame_is_intra_only(cm);
    sf->less_rectangular_check = 1;
    sf->tx_size_search_method =
        frame_is_intra_only(cm) ? USE_FULL_RD : USE_LARGESTALL;

    sf->use_rd_breakout = 1;

    sf->adaptive_motion_search = 1;
    sf->adaptive_pred_interp_filter = 1;
    sf->mv.auto_mv_step_size = 1;
    sf->adaptive_rd_thresh = 2;
#if CONFIG_TX64X64
    sf->intra_y_mode_mask[TX_64X64] = INTRA_DC_H_V;
    sf->intra_uv_mode_mask[TX_64X64] = INTRA_DC_H_V;
#endif  // CONFIG_TX64X64
    sf->intra_y_mode_mask[TX_32X32] = INTRA_DC_H_V;
    sf->intra_uv_mode_mask[TX_32X32] = INTRA_DC_H_V;
    sf->intra_uv_mode_mask[TX_16X16] = INTRA_DC_H_V;
  }

  if (speed >= 2) {
    sf->mode_search_skip_flags =
        (cm->frame_type == KEY_FRAME) ? 0 : FLAG_SKIP_INTRA_DIRMISMATCH |
                                                FLAG_SKIP_INTRA_BESTINTER |
                                                FLAG_SKIP_COMP_BESTINTRA |
                                                FLAG_SKIP_INTRA_LOWVAR;
    sf->adaptive_pred_interp_filter = 2;
    sf->disable_filter_search_var_thresh = 50;
    sf->comp_inter_joint_search_thresh = BLOCK_SIZES;
    sf->auto_min_max_partition_size = RELAXED_NEIGHBORING_MIN_MAX;
    sf->lf_motion_threshold = LOW_MOTION_THRESHOLD;
    sf->adjust_partitioning_from_last_frame = 1;
    sf->last_partitioning_redo_frequency = 3;
    sf->mode_skip_start = 11;
    sf->intra_y_mode_mask[TX_16X16] = INTRA_DC_H_V;
  }

  if (speed >= 3) {
    sf->use_square_partition_only = 1;
    sf->disable_filter_search_var_thresh = 100;
    sf->mv.subpel_iters_per_step = 1;
    sf->adaptive_rd_thresh = 4;
    sf->mode_skip_start = 6;
    sf->optimize_coefficients = 0;
    sf->disable_split_mask = DISABLE_ALL_SPLIT;
    sf->lpf_pick = LPF_PICK_FROM_Q;
  }

  if (speed >= 4) {
    int i;
    sf->last_partitioning_redo_frequency = 4;
    sf->adaptive_rd_thresh = 5;
    sf->use_fast_coef_costing = 0;
    sf->auto_min_max_partition_size = STRICT_NEIGHBORING_MIN_MAX;
    sf->adjust_partitioning_from_last_frame =
        cm->last_frame_type != cm->frame_type ||
        (0 == (frames_since_key + 1) % sf->last_partitioning_redo_frequency);
    sf->mv.subpel_force_stop = 1;
    for (i = 0; i < TX_SIZES; i++) {
      sf->intra_y_mode_mask[i] = INTRA_DC_H_V;
      sf->intra_uv_mode_mask[i] = INTRA_DC;
    }
#if CONFIG_TX64X64
    sf->intra_y_mode_mask[TX_64X64] = INTRA_DC;
#endif  // CONFIG_TX64X64
    sf->intra_y_mode_mask[TX_32X32] = INTRA_DC;
    sf->frame_parameter_update = 0;
    sf->mv.search_method = FAST_HEX;

    sf->inter_mode_mask[BLOCK_32X32] = INTER_NEAREST_NEAR_NEW;
    sf->inter_mode_mask[BLOCK_32X64] = INTER_NEAREST;
    sf->inter_mode_mask[BLOCK_64X32] = INTER_NEAREST;
    sf->inter_mode_mask[BLOCK_64X64] = INTER_NEAREST;
#if CONFIG_EXT_PARTITION
    sf->inter_mode_mask[BLOCK_64X128] = INTER_NEAREST;
    sf->inter_mode_mask[BLOCK_128X64] = INTER_NEAREST;
    sf->inter_mode_mask[BLOCK_128X128] = INTER_NEAREST;
#endif  // CONFIG_EXT_PARTITION
    sf->max_intra_bsize = BLOCK_32X32;
  }

  if (speed >= 5) {
    sf->auto_min_max_partition_size =
        is_keyframe ? RELAXED_NEIGHBORING_MIN_MAX : STRICT_NEIGHBORING_MIN_MAX;
    sf->default_max_partition_size = BLOCK_32X32;
    sf->default_min_partition_size = BLOCK_8X8;
    sf->force_frame_boost =
        is_keyframe ||
        (frames_since_key % (sf->last_partitioning_redo_frequency << 1) == 1);
    sf->max_delta_qindex = is_keyframe ? 20 : 15;
    sf->partition_search_type = REFERENCE_PARTITION;
    sf->inter_mode_mask[BLOCK_32X32] = INTER_NEAREST_NEW_ZERO;
    sf->inter_mode_mask[BLOCK_32X64] = INTER_NEAREST_NEW_ZERO;
    sf->inter_mode_mask[BLOCK_64X32] = INTER_NEAREST_NEW_ZERO;
    sf->inter_mode_mask[BLOCK_64X64] = INTER_NEAREST_NEW_ZERO;
#if CONFIG_EXT_PARTITION
    sf->inter_mode_mask[BLOCK_64X128] = INTER_NEAREST_NEW_ZERO;
    sf->inter_mode_mask[BLOCK_128X64] = INTER_NEAREST_NEW_ZERO;
    sf->inter_mode_mask[BLOCK_128X128] = INTER_NEAREST_NEW_ZERO;
#endif  // CONFIG_EXT_PARTITION
    sf->adaptive_rd_thresh = 2;
    // This feature is only enabled when partition search is disabled.
    sf->reuse_inter_pred_sby = 1;
    sf->partition_search_breakout_rate_thr = 200;
    sf->coeff_prob_appx_step = 4;
    sf->use_fast_coef_updates = is_keyframe ? TWO_LOOP : ONE_LOOP_REDUCED;
    sf->mode_search_skip_flags = FLAG_SKIP_INTRA_DIRMISMATCH;
    sf->tx_size_search_method = is_keyframe ? USE_LARGESTALL : USE_TX_8X8;
    sf->simple_model_rd_from_var = 1;

    if (!is_keyframe) {
      int i;
      if (content == AOM_CONTENT_SCREEN) {
        for (i = 0; i < BLOCK_SIZES; ++i)
          sf->intra_y_mode_bsize_mask[i] = INTRA_DC_TM_H_V;
      } else {
        for (i = 0; i < BLOCK_SIZES; ++i)
          if (i >= BLOCK_16X16)
            sf->intra_y_mode_bsize_mask[i] = INTRA_DC;
          else
            // Use H and V intra mode for block sizes <= 16X16.
            sf->intra_y_mode_bsize_mask[i] = INTRA_DC_H_V;
      }
    }
  }

  if (speed >= 6) {
    // Adaptively switch between SOURCE_VAR_BASED_PARTITION and FIXED_PARTITION.
    sf->partition_search_type = VAR_BASED_PARTITION;
    // Turn on this to use non-RD key frame coding mode.
    sf->mv.search_method = NSTEP;
    sf->mv.reduce_first_step_size = 1;
  }

  if (speed >= 7) {
    sf->adaptive_rd_thresh = 3;
    sf->mv.search_method = FAST_DIAMOND;
    sf->mv.fullpel_search_step_param = 10;
  }
  if (speed >= 8) {
    sf->adaptive_rd_thresh = 4;
    sf->mv.subpel_force_stop = 2;
    sf->lpf_pick = LPF_PICK_MINIMAL_LPF;
  }
}

void av1_set_speed_features_framesize_dependent(AV1_COMP *cpi) {
  SPEED_FEATURES *const sf = &cpi->sf;
  const AV1EncoderConfig *const oxcf = &cpi->oxcf;
  AV1_COMMON *const cm = &cpi->common;
  RD_OPT *const rd = &cpi->rd;
  int i;

  // Limit memory usage for high resolutions
  if (AOMMIN(cm->width, cm->height) > 1080) {
    sf->use_upsampled_references = 0;
  }

  if (oxcf->mode == REALTIME) {
    set_rt_speed_feature_framesize_dependent(cpi, sf, oxcf->speed);
  } else if (oxcf->mode == GOOD) {
    set_good_speed_feature_framesize_dependent(cpi, sf, oxcf->speed);
  }

  if (sf->disable_split_mask == DISABLE_ALL_SPLIT) {
    sf->adaptive_pred_interp_filter = 0;
  }

  // Check for masked out split cases.
  for (i = 0; i < MAX_REFS; ++i) {
    if (sf->disable_split_mask & (1 << i)) {
      rd->thresh_mult_sub8x8[i] = INT_MAX;
    }
  }
}

void av1_set_speed_features_framesize_independent(AV1_COMP *cpi) {
  SPEED_FEATURES *const sf = &cpi->sf;
  AV1_COMMON *const cm = &cpi->common;
  MACROBLOCK *const x = &cpi->td.mb;
  const AV1EncoderConfig *const oxcf = &cpi->oxcf;
  int i;

  // best quality defaults
  sf->frame_parameter_update = 1;
  sf->mv.search_method = NSTEP;
  sf->recode_loop = ALLOW_RECODE;
  sf->mv.subpel_search_method = SUBPEL_TREE;
  sf->mv.subpel_iters_per_step = 2;
  sf->mv.subpel_force_stop = 0;
  sf->optimize_coefficients = !is_lossless_requested(&cpi->oxcf);
  sf->mv.reduce_first_step_size = 0;
  sf->coeff_prob_appx_step = 1;
  sf->mv.auto_mv_step_size = 0;
  sf->mv.fullpel_search_step_param = 6;
  sf->comp_inter_joint_search_thresh = BLOCK_4X4;
  sf->adaptive_rd_thresh = 0;
  sf->tx_size_search_method = USE_FULL_RD;
  sf->adaptive_motion_search = 0;
  sf->adaptive_pred_interp_filter = 0;
  sf->adaptive_mode_search = 0;
  sf->cb_pred_filter_search = 0;
  sf->cb_partition_search = 0;
  sf->alt_ref_search_fp = 0;
  sf->partition_search_type = SEARCH_PARTITION;
  sf->tx_type_search.prune_mode = NO_PRUNE;
  sf->tx_type_search.fast_intra_tx_type_search = 0;
  sf->tx_type_search.fast_inter_tx_type_search = 0;
  sf->less_rectangular_check = 0;
  sf->use_square_partition_only = 0;
  sf->auto_min_max_partition_size = NOT_IN_USE;
  sf->rd_auto_partition_min_limit = BLOCK_4X4;
  sf->default_max_partition_size = BLOCK_LARGEST;
  sf->default_min_partition_size = BLOCK_4X4;
  sf->adjust_partitioning_from_last_frame = 0;
  sf->last_partitioning_redo_frequency = 4;
  sf->disable_split_mask = 0;
  sf->mode_search_skip_flags = 0;
  sf->force_frame_boost = 0;
  sf->max_delta_qindex = 0;
  sf->disable_filter_search_var_thresh = 0;
  sf->adaptive_interp_filter_search = 0;
  sf->allow_partition_search_skip = 0;
#if CONFIG_EXT_TILE
  sf->use_upsampled_references = 0;
#else
  sf->use_upsampled_references = 1;
#endif  // CONFIG_EXT_TILE
#if CONFIG_EXT_INTER
  sf->disable_wedge_search_var_thresh = 0;
  sf->fast_wedge_sign_estimate = 0;
#endif  // CONFIG_EXT_INTER

  for (i = 0; i < TX_SIZES; i++) {
    sf->intra_y_mode_mask[i] = INTRA_ALL;
    sf->intra_uv_mode_mask[i] = INTRA_ALL;
  }
  sf->use_rd_breakout = 0;
  sf->lpf_pick = LPF_PICK_FROM_FULL_IMAGE;
  sf->use_fast_coef_updates = TWO_LOOP;
  sf->use_fast_coef_costing = 0;
  sf->mode_skip_start = MAX_MODES;  // Mode index at which mode skip mask set
  sf->schedule_mode_search = 0;
  for (i = 0; i < BLOCK_SIZES; ++i) sf->inter_mode_mask[i] = INTER_ALL;
  sf->max_intra_bsize = BLOCK_LARGEST;
  sf->reuse_inter_pred_sby = 0;
  // This setting only takes effect when partition_search_type is set
  // to FIXED_PARTITION.
  sf->always_this_block_size = BLOCK_16X16;
  sf->search_type_check_frequency = 50;
  // Recode loop tolerance %.
  sf->recode_tolerance = 25;
  sf->default_interp_filter = SWITCHABLE;
  sf->tx_size_search_breakout = 0;
  sf->partition_search_breakout_dist_thr = 0;
  sf->partition_search_breakout_rate_thr = 0;
  sf->simple_model_rd_from_var = 0;

// Set this at the appropriate speed levels
#if CONFIG_EXT_TILE
  sf->use_transform_domain_distortion = 1;
#else
  sf->use_transform_domain_distortion = 0;
#endif  // CONFIG_EXT_TILE

  if (oxcf->mode == REALTIME)
    set_rt_speed_feature(cpi, sf, oxcf->speed, oxcf->content);
  else if (oxcf->mode == GOOD)
    set_good_speed_feature(cpi, cm, sf, oxcf->speed);

  // sf->partition_search_breakout_dist_thr is set assuming max 64x64
  // blocks. Normalise this if the blocks are bigger.
  if (MAX_SB_SIZE_LOG2 > 6) {
    sf->partition_search_breakout_dist_thr <<= 2 * (MAX_SB_SIZE_LOG2 - 6);
  }

  cpi->full_search_sad = av1_full_search_sad;
  cpi->diamond_search_sad = av1_diamond_search_sad;

  sf->allow_exhaustive_searches = 1;
  if (oxcf->mode == BEST) {
    if (cpi->twopass.fr_content_type == FC_GRAPHICS_ANIMATION)
      sf->exhaustive_searches_thresh = (1 << 20);
    else
      sf->exhaustive_searches_thresh = (1 << 21);
    sf->max_exaustive_pct = 100;
    for (i = 0; i < MAX_MESH_STEP; ++i) {
      sf->mesh_patterns[i].range = best_quality_mesh_pattern[i].range;
      sf->mesh_patterns[i].interval = best_quality_mesh_pattern[i].interval;
    }
  } else {
    int speed = (oxcf->speed > MAX_MESH_SPEED) ? MAX_MESH_SPEED : oxcf->speed;
    if (cpi->twopass.fr_content_type == FC_GRAPHICS_ANIMATION)
      sf->exhaustive_searches_thresh = (1 << 22);
    else
      sf->exhaustive_searches_thresh = (1 << 23);
    sf->max_exaustive_pct = good_quality_max_mesh_pct[speed];
    if (speed > 0)
      sf->exhaustive_searches_thresh = sf->exhaustive_searches_thresh << 1;

    for (i = 0; i < MAX_MESH_STEP; ++i) {
      sf->mesh_patterns[i].range = good_quality_mesh_patterns[speed][i].range;
      sf->mesh_patterns[i].interval =
          good_quality_mesh_patterns[speed][i].interval;
    }
  }

  // Slow quant, dct and trellis not worthwhile for first pass
  // so make sure they are always turned off.
  if (oxcf->pass == 1) sf->optimize_coefficients = 0;

  // No recode for 1 pass.
  if (oxcf->pass == 0) {
    sf->recode_loop = DISALLOW_RECODE;
    sf->optimize_coefficients = 0;
  }

  if (sf->mv.subpel_search_method == SUBPEL_TREE) {
    cpi->find_fractional_mv_step = av1_find_best_sub_pixel_tree;
  } else if (sf->mv.subpel_search_method == SUBPEL_TREE_PRUNED) {
    cpi->find_fractional_mv_step = av1_find_best_sub_pixel_tree_pruned;
  } else if (sf->mv.subpel_search_method == SUBPEL_TREE_PRUNED_MORE) {
    cpi->find_fractional_mv_step = av1_find_best_sub_pixel_tree_pruned_more;
  } else if (sf->mv.subpel_search_method == SUBPEL_TREE_PRUNED_EVENMORE) {
    cpi->find_fractional_mv_step = av1_find_best_sub_pixel_tree_pruned_evenmore;
  }

#if !CONFIG_AOM_QM
  x->optimize = sf->optimize_coefficients == 1 && oxcf->pass != 1;
#else
  // FIXME: trellis not very efficient for quantisation matrices
  x->optimize = 0;
#endif

  x->min_partition_size = sf->default_min_partition_size;
  x->max_partition_size = sf->default_max_partition_size;

  if (!cpi->oxcf.frame_periodic_boost) {
    sf->max_delta_qindex = 0;
  }
}
