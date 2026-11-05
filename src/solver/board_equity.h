#pragma once
// Exact equity of every combo against a weighted range on a flop, turn or river (version 2).
// The constructor ranks every combo on every completion of the board once; each query is then
// two prefix passes per completion over the combos in rank order, with running sums per card to
// remove the opponent hands that share a card with ours.
#include <array>
#include <cstdint>
#include <vector>

#include "solver/cards.h"

namespace pf {

using ComboWeights = std::array<float, NCOMBOS>;  // indexed by comboIndex(), as in postflop.h

class BoardEquity {
public:
    BoardEquity(const int* board, int n);  // n = 3, 4 or 5 cardIndex() values

    // For each combo x disjoint from the board, over opponent combos y disjoint from x and the board:
    //   den[x] = sum_y w[y],  num[x] = sum_y w[y] * eq(x, y)   (ties count half)
    // so num / den is x's equity against the range. Both are 0 for combos on the board.
    void sums(const ComboWeights& w, std::array<double, NCOMBOS>& num, std::array<double, NCOMBOS>& den) const;
    // num / den, 0 where den is 0.
    void equity(const ComboWeights& w, ComboWeights& eq) const;

    bool blocked(int combo) const { return blocked_[combo]; }

private:
    static constexpr int PER = 1081;  // combos disjoint from a full 5-card board, C(47, 2)
    std::array<bool, NCOMBOS> blocked_{};
    std::array<uint16_t, NCOMBOS> lo_{}, hi_{};  // the two cards of each combo
    int runouts_ = 0;
    double perPair_ = 1;                // completions of the board disjoint from both hands
    struct Entry {
        uint16_t x;                     // combo
        uint8_t a, b;                   // its cards
    };
    std::vector<Entry> order_;          // [runout][2][PER] combos by rank; ties ascending, then descending
    std::vector<Entry> live_;           // combos disjoint from the board
    std::array<bool, 52> onBoard_{};
    int nBoard_ = 0;
    std::vector<uint16_t> byCard_;      // per card off the board, its combos with the other 51 - n
};

}  // namespace pf
