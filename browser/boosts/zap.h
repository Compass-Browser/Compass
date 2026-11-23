// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_BOOSTS_ZAP_H_
#define COMPASS_BROWSER_BOOSTS_ZAP_H_

#include <string>
#include <string_view>
#include <vector>

#include "compass/browser/boosts/boost_store.h"

// Zap (docs/design/boosts.md, "Zap"): elements the user clicked away on a
// site, kept as a Boost of that site whose CSS hides them. One per site,
// told apart from the user's own Boosts by the first line of its CSS.
namespace compass::boosts {

// The first line of a zap Boost's CSS.
inline constexpr char kZapMarker[] = "/* Zapped with Compass */";

// Whether |boost| is a zap Boost.
bool IsZapBoost(const Boost& boost);

// |store|'s zap Boost for exactly |match| (a BoostMatch pattern), or null.
const Boost* FindZapBoost(const BoostStore& store, std::string_view match);

// The selectors |boost|'s CSS hides, in order.
std::vector<std::string> ZappedSelectors(const Boost& boost);

// |existing| (a zap Boost) with |selectors| hidden too, or a new zap Boost
// for |match| when |existing| is null; selectors already there aren't
// added twice. Empty selectors and ones with "{", "}" or a comment are
// dropped: each must stay one rule. Enabled again if it was off.
Boost AddZappedSelectors(const Boost* existing,
                         std::string_view match,
                         const std::vector<std::string>& selectors);

}  // namespace compass::boosts

#endif  // COMPASS_BROWSER_BOOSTS_ZAP_H_
