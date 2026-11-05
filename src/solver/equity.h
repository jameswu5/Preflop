#pragma once
// Hand evaluation (PokerHandEvaluator wrapper) and preflop equity, docs/math.md.
#include <array>
#include <random>
#include <string>
#include <vector>

#include "trainer/game.h"

namespace pf {

using Holes = std::array<std::array<Card, 2>, NPOS>;

// Strength of the best 5-card hand among 7 cards; higher is better, equal means a split.
int rank7(const Card* c);

class Equity {
public:
    // data/hu_equity.bin from gen_equity. Without it, heads-up spots are sampled too.
    bool loadHuTable(const std::string& path);

    // Pot share of each live player over `boards` random boards dealt from the deck minus all
    // 12 hole cards (folded hands are dead). Ties split. Folded players get 0; live shares sum to 1.
    // Heads-up with the table loaded: exact table lookup, `boards` and `rng` unused.
    // ponytail: the table ignores the 8 folded cards. Fixed dead cards can move equity 2-4 points;
    // random ones average out, but folders skew to weak cards, so this drops that bias. If it
    // matters, sample instead (skip loadHuTable).
    // boards == 1 is the single sampled board for an all-in showdown (math.md).
    void compute(const Holes& cards, const std::array<bool, NPOS>& live, std::mt19937& rng,
                 int boards, std::array<double, NPOS>& out) const;

    // Exact heads-up equity of combo a against combo b (comboIndex()); needs loadHuTable().
    float hu(int a, int b) const { return hu_[size_t(a) * 1326 + b]; }
    bool hasHuTable() const { return !hu_.empty(); }

private:
    std::vector<float> hu_;  // [comboIndex(a)][comboIndex(b)] = equity of a vs b
};

}  // namespace pf
