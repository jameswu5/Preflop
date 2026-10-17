#pragma once
// Terminal payoffs in bb per seat, won minus contributed (docs/math.md "Terminal payoffs"). They sum
// to -rake(h): zero when no flop is dealt or rake is off.
#include <array>
#include <random>

#include "solver/equity.h"
#include "trainer/game.h"

namespace pf {

// Equity realization factor R: how much of its raw equity a hand turns into pot share at a flop
// terminal (docs/math.md). Built from features of the hand, its role (position, caller or raiser)
// and the pot depth. Fold equity and blockers are exact in the solver already (real cards are dealt),
// so there are deliberately no blocker terms. With g = SPR / (SPR + spr) (1 if spr = 0):
//   P = lastToAct ? ip : 1 - (1 - oop) * g                    position matters less shallow
//   H = 1 + flush * suited + conn * straights * g + set * pair * g
//         - dom * domination * (caller) - low * (12 - hi) * (offsuit non-pair) - pairLow * (12 - hi) * pair
//   R = max(P * H * (1 + init * raiser), 0.05)
// Starting values, to be fit by src/tools/tune.py. All zero but ip/oop (and spr = 0) = position-only.
// Keep this struct all doubles: it is compared bytewise and written raw into strategy files.
struct RealizationConfig {
    double ip = 1, oop = 0.7;  // position factors (ip = last to act postflop among the live players)
    double flush = 0.15;       // suited hands
    double conn = 0.05;        // per straight using both cards (straights()), scaled by g
    double set = 0.1;          // pairs (set mining), scaled by g
    double dom = 0.1;          // per point of domination() when calling, not when the last raiser
    double low = 0.03;         // offsuit non-pairs: per rank the top card is below an ace
    double spr = 3;            // pot depth: SPR at which g = 1/2; 0 = depth-independent (g = 1)
    double init = 0.25;        // R *= 1 + init for the last raiser
    double pairLow = 0.02;     // pairs: per rank below aces (overcards), at any depth
};

// Number of 5-card straights (A-2-3-4-5 through T-J-Q-K-A) containing both ranks; 0 for pairs.
// 98 = 4, KQ = 2, AK = 1, A5 = 1, T6 = 1, K4 = 0.
int straights(Card a, Card b);
// Offsuit non-pairs with a T+ top card: min(hi - 1 - lo, 4) (KQo 0, KJo 1, AJo 2, K6o 4); else 0.
int domination(Card a, Card b);

// R for one player's hand at a flop with stack-to-pot ratio `spr`. `raiser`: made the last raise.
double realization(const RealizationConfig& rc, bool lastToAct, bool raiser, double spr, Card a, Card b);

// Rake taken at a done() Hand: min(rake * pot, rakeCap) if 2+ players are left (a flop is dealt or an
// all-in is called), else 0 (no flop, no drop).
double rake(const Hand& h);

// All-in called: done() with every live player at currentBet() == stack.
bool allIn(const Hand& h);

// Payoffs at a done() Hand given each seat's equity (ignored if all but one folded):
// share_i * (pot - rake) - contributed_i. All-in: share = eq, the showdown pot share.
// Flop: share_i = R_i*eq_i / sum_j R_j*eq_j.
std::array<double, NPOS> payoffs(const Hand& h, const std::array<double, NPOS>& eq,
                                 const RealizationConfig& rc = {});

// payoffs() with eq from Equity::compute: 1 board for an all-in, `flopBoards` otherwise.
std::array<double, NPOS> utility(const Hand& h, const Equity& e, std::mt19937& rng,
                                 int flopBoards = 100, const RealizationConfig& rc = {});

}  // namespace pf
