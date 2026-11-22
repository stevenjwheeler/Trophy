/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

namespace TrophyBackend {
// Exact Steam application IDs, not title/executable substring matches.
// Tool manifests identify additional/new Proton and container-runtime versions.
inline bool knownSteamSupportAppId(unsigned id) {
  switch (id) {
  case 228980:  // Steamworks Common Redistributables
  case 1070560: // Steam Linux Runtime 1 (scout)
  case 1161040: // Proton BattlEye Runtime
  case 1391110: // Steam Linux Runtime 2 (soldier)
  case 1493710: // Proton Experimental
  case 1628350: // Steam Linux Runtime 3 (sniper)
  case 1826330: // Proton EasyAntiCheat Runtime
  case 2180100: // Proton Hotfix
  case 2805730: // Proton 9
  case 4183110: // Steam Linux Runtime 4
  case 4628710: // Proton 11
    return true;
  default:
    return false;
  }
}
} // namespace TrophyBackend
