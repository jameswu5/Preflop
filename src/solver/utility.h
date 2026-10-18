#pragma once
// Terminal payoffs in bb per seat, won minus contributed (docs/math.md "Terminal payoffs"). They sum
// to -rake(h): zero when no flop is dealt or rake is off.
#include <array>
#include <cstddef>
#include <random>

#include "solver/equity.h"
#include "trainer/game.h"

namespace pf {

// Equity realization factor R: how much of its raw equity a hand turns into pot share at a flop
// terminal (docs/math.md). Built from features of the hand, its role (position, caller or raiser)
// and the pot depth. Fold equity and blockers are exact in the solver already (real cards are dealt),
// so there are deliberately no blocker terms. With g = SPR / (SPR + spr) (1 if spr = 0):
//   P = lastToAct ? ip : 1 - (1 - oop) * g                    position matters less shallow
//       (OOP callers: g is replaced by callerShrink * g + 1 - callerShrink;
//        IP callers in a re-raised pot (3-bet or more): P = ip * ipCall)
//   H = 1 + (flush * suited + conn * straights * g + set * pair * g) * (oopCall for OOP callers, else 1)
//         - (dom + domPot * reraised) * domination * (caller) - low * (12 - hi) * (offsuit non-pair)
//         - pairLow * (12 - hi) * pair
//   R = max(P * H * (1 + init * raiser), 0.05)
// Defaults are the values fitted by src/tools/tune.py (docs/math.md §4). Neutral, position-only: every
// term 0 but ip/oop, spr = 0, and oopCall = callerShrink = ipCall = 1.
// Keep this struct all doubles: it is compared bytewise and written raw into strategy files.
struct RealizationConfig {
    double ip = 1, oop = 0.7;  // position factors (ip = last to act postflop among the live players)
    double flush = 0.16;       // suited hands
    double conn = 0.05;        // per straight using both cards (straights()), scaled by g
    double set = 0.1;          // pairs (set mining), scaled by g
    double dom = 0.1;          // per point of domination() when calling, not when the last raiser
    double low = 0.03;         // offsuit non-pairs: per rank the top card is below an ace
    double spr = 3;            // pot depth: SPR at which g = 1/2; 0 = depth-independent (g = 1)
    double init = 0.2;         // R *= 1 + init for the last raiser
    double pairLow = 0.01;     // pairs: per rank below aces (overcards), at any depth
    // Out-of-position callers (not last to act, not the last raiser); 1 = off, the same as everyone else.
    double oopCall = 1;        // scales their playability bonuses (flush, conn, set): no position, no initiative
    double callerShrink = 0;   // their position penalty fades with depth as 1 - (1 - oop) * (callerShrink * g + 1 - callerShrink)
    // Callers in re-raised (3-bet+) pots. Appended after the fields above: files written before
    // these existed load with them neutral, ipCall 1 and domPot 0 (Strategy::load, Solver::loadCheckpoint).
    double ipCall = 0.8;       // scales P for an in-position caller: calling a 3-bet in position is no free ride
    double domPot = 0;         // extra domination penalty per point for any caller
};
// Size of RealizationConfig before ipCall/domPot, as stored by older files.
constexpr size_t REALIZATION_V1_SIZE = offsetof(RealizationConfig, ipCall);

// Number of 5-card straights (A-2-3-4-5 through T-J-Q-K-A) containing both ranks; 0 for pairs.
// 98 = 4, KQ = 2, AK = 1, A5 = 1, T6 = 1, K4 = 0.
int straights(Card a, Card b);
// Offsuit non-pairs with a T+ top card: min(hi - 1 - lo, 4) (KQo 0, KJo 1, AJo 2, K6o 4); else 0.
int domination(Card a, Card b);

// R for one player's hand at a flop with stack-to-pot ratio `spr`. `raiser`: made the last raise.
// `reraised`: the pot was 3-bet or more (Hand::raises() >= 2).
double realization(const RealizationConfig& rc, bool lastToAct, bool raiser, double spr, Card a, Card b,
                   bool reraised = false);

// Rake taken at a done() Hand: min(rake * pot, rakeCap) if 2+ players are left (a flop is dealt or an
// all-in is called), else 0 (no flop, no drop).
double rake(const Hand& h);

// All-in called: done() with every live player at currentBet() == stack.
bool allIn(const Hand& h);

// Everything payoffs() needs from a done() Hand except the cards; fixed by the betting history, so
// the solver builds one per terminal up front.
struct Terminal {
    std::array<double, NPOS> committed{};
    double net = 0, spr = 0;  // pot minus rake; last live player's stack / pot
    int live = 0, last = -1, lastRaiser = -1;  // live seat bitmask, live seat acting last postflop
    bool allIn = false, reraised = false;
    Terminal() = default;
    explicit Terminal(const Hand& h);
};

// Payoffs at a done() Hand given each seat's equity (ignored if all but one folded):
// share_i * (pot - rake) - contributed_i. All-in: share = eq, the showdown pot share.
// Flop: share_i = R_i*eq_i / sum_j R_j*eq_j.
std::array<double, NPOS> payoffs(const Hand& h, const std::array<double, NPOS>& eq,
                                 const RealizationConfig& rc = {});
std::array<double, NPOS> payoffs(const Terminal& t, const Holes& cards, const std::array<double, NPOS>& eq,
                                 const RealizationConfig& rc);

// payoffs() with eq from Equity::compute: 1 board for an all-in, `flopBoards` otherwise.
std::array<double, NPOS> utility(const Hand& h, const Equity& e, std::mt19937& rng,
                                 int flopBoards = 100, const RealizationConfig& rc = {});

}  // namespace pf
