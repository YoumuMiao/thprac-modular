#pragma once

// Player-position field inside the player object, one row per title.
//
// This table is the single source of truth: the F8 status HUD and the F10 route
// overlay both read it, so an offset only has to be right once and the two can
// never drift apart. TH06-TH08 have no player object (they read static globals
// or the hitbox centre) and return 0.
//
// Verified in game through the F8 HUD readout, which shows P(x,y) from these
// exact fields:
//   TH10 0x3C0  TH11 0x87C  TH12 0x97C  TH13 0x5B8  TH14 0x5E0
//   TH16 0x610  TH17 0x610  TH18 0x620  TH20 0x614
//
// TH14 is 0x5E0, not the 0x5B0 thprac used before: 0x5B0 is a copy of 0x5E0
// taken for the previous-frame backup, so it lags by one frame.
// TH20 reaches the player through GAME_SIDE0 + 4 and keeps the position pair at
// 0x614.

namespace THPrac {

constexpr int PlayerPositionOffset(int game)
{
    switch (game) {
    case 10: return 0x3C0;
    case 11: return 0x87C;
    case 12: return 0x97C;
    case 13: return 0x5B8;
    case 14: return 0x5E0;
    case 16: return 0x610;
    case 17: return 0x610;
    case 18: return 0x620;
    case 20: return 0x614;
    default: return 0;
    }
}

} // namespace THPrac
