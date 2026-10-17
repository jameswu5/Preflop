#pragma once
// Terminal payoffs in bb per seat, won minus contributed (docs/math.md "Terminal payoffs").
#include <array>
#include <random>

#include "solver/equity.h"
#include "trainer/game.h"

namespace pf {

// Equity realization factors, the main tuning knob (validation.md). `ip` goes to the live player
// who acts last postflop, `oop` to every other live player.
// ponytail: position-only R; add per-hand-class adjustments here when tuning calls for them.
struct RealizationConfig {
    double ip = 1.0, oop = 0.85;
};

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
