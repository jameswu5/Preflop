#pragma once
// Terminal payoffs in bb per seat, won minus contributed (docs/math.md "Terminal payoffs").
#include <array>
#include <random>

#include "solver/equity.h"
#include "trainer/game.h"

namespace pf {

// Equity realization factors, the main tuning knob (validation.md). R = position * hand class:
// `ip` for the live player who acts last postflop, `oop` for the others, times the hand's class
// multiplier, minus penalties for unconnected and low cards. Defaults were fit by
// src/tools/tune.py to public RFI and BTN-vs-CO charts with squeeze calls on (docs/validation.md).
struct RealizationConfig {
    double ip = 1.0, oop = 0.85;
    double pair = 1.0, suited = 1.075, offsuit = 0.8;  // class multipliers
    double gap = 0.08;  // per rank between unpaired cards (KQ 0, KJ 1, ..., 4+ counts as 4)
    double low = 0.03;  // offsuit only: per rank the higher card is below an ace (A-x 0, 32o 11)
    // Pot dependence (0 = off). init: R *= 1 + init for the last raiser. spr: the deviation from 1
    // shrinks in low-SPR pots, R = 1 - (1 - R) * SPR / (SPR + spr), applied after init.
    // Keep this struct all doubles: it is compared bytewise and written raw into strategy files.
    double spr = 2, init = 0;
};

// R for one player's hand at a flop with stack-to-pot ratio `spr`, floored at 0.05 so no hand
// realizes nothing. `raiser`: this player made the last raise.
double realization(const RealizationConfig& rc, bool lastToAct, bool raiser, double spr, Card a, Card b);

// All-in called: done() with every live player at currentBet() == stack.
bool allIn(const Hand& h);

// Payoffs at a done() Hand given each seat's equity (ignored if all but one folded).
// All-in: eq is the showdown pot share. Flop: share_i = R_i*eq_i / sum_j R_j*eq_j.
std::array<double, NPOS> payoffs(const Hand& h, const std::array<double, NPOS>& eq,
                                 const RealizationConfig& rc = {});

// payoffs() with eq from Equity::compute: 1 board for an all-in, `flopBoards` otherwise.
std::array<double, NPOS> utility(const Hand& h, const Equity& e, std::mt19937& rng,
                                 int flopBoards = 100, const RealizationConfig& rc = {});

}  // namespace pf
