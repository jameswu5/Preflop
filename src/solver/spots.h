#pragma once
// Heads-up flop spots of the preflop tree and the ranges that reach them (version 2, Phase 1):
// the inputs to the postflop solves behind the realization table.
#include <vector>

#include "postflop/postflop.h"
#include "solver/realization_table.h"
#include "solver/strategy.h"

namespace pf {

// Postflop order is SB, BB, UTG, ..., BTN: the lower rank acts first.
inline int postflopOrder(int seat) { return (seat + 2) % NPOS; }

struct FlopSpot {
    PotType pot = PotType::Single;
    Pos raiser = UTG, caller = BB;
    std::vector<Act> history;  // from UTG to the call that closes the action
    double potBB = 0, stackBB = 0;  // at the flop, before rake; stackBB = effective stack behind
    Pos oop() const { return postflopOrder(raiser) < postflopOrder(caller) ? raiser : caller; }
    Pos ip() const { return oop() == raiser ? caller : raiser; }
};

// Every flop terminal of the tree that is heads-up, not all-in, and 1-3 raises deep with no dead
// money beyond the blinds: the open, 3-bet and 4-bet lines, one per (pot type, raiser, caller).
// Squeezes, cold calls of a 3-bet and cold 4-bets leave a folder's chips in the pot, so they are not
// listed; realization() falls back to the formula there. Ordered by pot type, then history.
std::vector<FlopSpot> flopSpots(const Config& cfg);

// `seat`'s range at the end of `history`: per combo, the probability that its own actions along the
// line were taken (Strategy::reach). For chart ranges, applyCharts() the strategy first.
post::ComboWeights lineRange(const Strategy& s, const std::vector<Act>& history, int seat);

// Sums one spot's postflop results over flops into a table entry. Per role and class,
// R = sum(m * w * ev) / (sum(m * w * eq) * pot), with m the flop's multiplicity and w the combo's
// weight (post::Result::weight); n = sum(m * w), so n sums to the total multiplicity over the classes.
struct RealizationAccum {
    explicit RealizationAccum(const FlopSpot& s) : spot(s) {}
    void add(const post::Result& r, double multiplicity);  // r from a solve with range[0] = spot.oop()
    RealizationEntry entry() const;                        // R = 0 for classes with no weight

    FlopSpot spot;
    int flops = 0;
    std::array<std::array<double, NBUCKETS>, 2> ev{}, eq{}, n{};  // [0] raiser, [1] caller
};

}  // namespace pf
