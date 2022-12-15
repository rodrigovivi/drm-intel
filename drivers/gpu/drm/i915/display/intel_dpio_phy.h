/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_DPIO_PHY_H__
#define __INTEL_DPIO_PHY_H__

#include <linux/types.h>
#include "intel_display.h"

enum dpio_channel;
enum dpio_phy;
enum port;
struct drm_i915_private;
struct intel_crtc_state;
struct intel_encoder;

#ifdef I915
void bxt_port_to_phy_channel(struct drm_i915_private *dev_priv, enum port port,
			     enum dpio_phy *phy, enum dpio_channel *ch);
void bxt_ddi_phy_set_signal_levels(struct intel_encoder *encoder,
				   const struct intel_crtc_state *crtc_state);
void bxt_ddi_phy_init(struct drm_i915_private *dev_priv, enum dpio_phy phy);
void bxt_ddi_phy_uninit(struct drm_i915_private *dev_priv, enum dpio_phy phy);
bool bxt_ddi_phy_is_enabled(struct drm_i915_private *dev_priv,
			    enum dpio_phy phy);
bool bxt_ddi_phy_verify_state(struct drm_i915_private *dev_priv,
			      enum dpio_phy phy);
u8 bxt_ddi_phy_calc_lane_lat_optim_mask(u8 lane_count);
void bxt_ddi_phy_set_lane_optim_mask(struct intel_encoder *encoder,
				     u8 lane_lat_optim_mask);
u8 bxt_ddi_phy_get_lane_lat_optim_mask(struct intel_encoder *encoder);

void chv_set_phy_signal_level(struct intel_encoder *encoder,
			      const struct intel_crtc_state *crtc_state,
			      u32 deemph_reg_value, u32 margin_reg_value,
			      bool uniq_trans_scale);
void chv_data_lane_soft_reset(struct intel_encoder *encoder,
			      const struct intel_crtc_state *crtc_state,
			      bool reset);
void chv_phy_pre_pll_enable(struct intel_encoder *encoder,
			    const struct intel_crtc_state *crtc_state);
void chv_phy_pre_encoder_enable(struct intel_encoder *encoder,
				const struct intel_crtc_state *crtc_state);
void chv_phy_release_cl2_override(struct intel_encoder *encoder);
void chv_phy_post_pll_disable(struct intel_encoder *encoder,
			      const struct intel_crtc_state *old_crtc_state);

void vlv_set_phy_signal_level(struct intel_encoder *encoder,
			      const struct intel_crtc_state *crtc_state,
			      u32 demph_reg_value, u32 preemph_reg_value,
			      u32 uniqtranscale_reg_value, u32 tx3_demph);
void vlv_phy_pre_pll_enable(struct intel_encoder *encoder,
			    const struct intel_crtc_state *crtc_state);
void vlv_phy_pre_encoder_enable(struct intel_encoder *encoder,
				const struct intel_crtc_state *crtc_state);
void vlv_phy_reset_lanes(struct intel_encoder *encoder,
			 const struct intel_crtc_state *old_crtc_state);

#else
#define bxt_port_to_phy_channel(xe, port, phy, ch) do { *phy = 0; *ch = 0; } while (xe && port && 0)
static inline void bxt_ddi_phy_set_signal_levels(struct intel_encoder *x,
						 const struct intel_crtc_state *y) {}
#define bxt_ddi_phy_init(xe, phy) do { } while (xe && phy && 0)
#define bxt_ddi_phy_uninit(xe, phy) do { } while (xe && phy && 0)
#define bxt_ddi_phy_is_enabled(xe, phy) (xe && phy && 0)
static inline bool bxt_ddi_phy_verify_state(struct xe_device *xe, enum dpio_phy phy) { return false; }
#define bxt_ddi_phy_calc_lane_lat_optim_mask(x) (x && 0)
#define bxt_ddi_phy_set_lane_optim_mask(x, y) do { } while (x && y && 0)
#define bxt_ddi_phy_get_lane_lat_optim_mask(x) (x && 0)
#endif

#endif /* __INTEL_DPIO_PHY_H__ */
