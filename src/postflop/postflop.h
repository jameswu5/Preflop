#pragma once
// Heads-up postflop solver (version 2, Phase 0): vector-form discounted CFR from the flop to the
// river over a fixed bet tree. Interface contract shared by the solver, the realization-table tool
// and the trainer; see docs/architecture.md, "Version 2".
#include <array>
#include <vector>

#include "solver/cards.h"

namespace pf::post {

// Bet menu for one street. Bets are fractions of the pot; raises are multiples of the bet faced
// (raise to mult x the opponent's chips in on this street). All-in is added per TreeConfig.
struct StreetSizes {
    std::vector<double> bet, raise;
};

// "Pot" below is every chip in the middle at the decision, including this street's bets.
struct TreeConfig {
    double pot = 0, stack = 0;          // bb at the flop; stack = effective stack behind
    std::array<StreetSizes, 3> sizes;   // flop, turn, river
    int maxRaises = 2;                  // per street, after the first bet
    double addAllinBelowSpr = 1.5;      // all-in is offered when the actor's stack behind <= this x pot
    double forceAllinSpr = 0.15;        // a bet or raise becomes all-in if stack / pot once called <= this
    double mergeFrac = 0.1;             // from the largest down, drop a size within this x pot of a kept one
};

using ComboWeights = std::array<float, NCOMBOS>;  // indexed by comboIndex()

struct Spot {
    std::array<int, 3> flop{};          // cardIndex() values
    int turn = -1, river = -1;          // a card given means that street is already dealt; -1 = by chance
    std::array<ComboWeights, 2> range{};  // [0] = OOP (acts first), [1] = IP; preflop reach weights
    TreeConfig tree;
};

struct Options {
    int maxIters = 1000;
    double targetExpl = 0.01;           // stop when exploitability <= this fraction of the pot
    int threads = 0;                    // 0 = hardware concurrency
    bool compress = false;              // 16-bit regret and strategy storage
};

struct Result {
    // Per combo, 0 where the combo is blocked by the flop or has zero weight.
    std::array<ComboWeights, 2> ev{};      // bb won from the flop on: share of the pot plus postflop
                                           // chips won minus postflop chips put in, so the
                                           // weight-averaged ev of both players sums to the pot
    std::array<ComboWeights, 2> equity{};  // showdown equity against the opponent's range on this flop
    std::array<ComboWeights, 2> weight{};  // reach weights after card removal against the flop and the
                                           // opponent's range, normalised to sum to 1 per player
    double exploitability = 0;             // fraction of the pot
    int iterations = 0;
};

// Realization of a combo or class: sum(weight * ev) / (sum(weight * equity) * pot).
Result solve(const Spot& spot, const Options& opt);

}  // namespace pf::post
