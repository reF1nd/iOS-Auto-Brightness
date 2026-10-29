// SPDX-License-Identifier: GPL-3.0-only
// Brightness curve of the v6.8 reference daemon: lux -> nits -> panel level.
#pragma once

namespace iab {

// Nits of the reference panel at raw levels i * 8191 / 51.
extern const float kNitsTable[52];

float nits_for_lux(float lux);
// Panel level for nits on a panel whose maximum level is max_level.
int level_for_nits(float nits, int max_level);
// Reference ios_curve(): 1 for lux <= 0, otherwise level_for_nits(nits_for_lux(lux)).
int curve_level(float lux, int max_level);
// Learning reference: system slider 0..255 through manual_gamma to a panel level.
int slider_level(int slider, float gamma, int max_level);

}  // namespace iab
