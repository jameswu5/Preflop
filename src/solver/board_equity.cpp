#include "solver/board_equity.h"

#include <algorithm>

#include "solver/equity.h"
#include "solver/flops.h"

namespace pf {

BoardEquity::BoardEquity(const int* board, int n) {
    bool used[52] = {};
    for (int i = 0; i < n; ++i) used[board[i]] = true;
    for (int x = 0; x < NCOMBOS; ++x) {
        auto [a, b] = comboCards(x);
        lo_[x] = uint16_t(a), hi_[x] = uint16_t(b);
        blocked_[x] = used[a] || used[b];
    }
    std::vector<int> rest;
    for (int c = 0; c < 52; ++c)
        if (!used[c]) rest.push_back(c);
    // Completions: every (5 - n)-subset of the remaining cards.
    std::vector<std::array<int, 2>> runs;
    int m = int(rest.size());
    if (n == 5) runs.push_back({-1, -1});
    else if (n == 4) for (int c : rest) runs.push_back({c, -1});
    else for (int i = 0; i < m; ++i) for (int j = i + 1; j < m; ++j) runs.push_back({rest[i], rest[j]});
    runouts_ = int(runs.size());
    perPair_ = n == 5 ? 1 : n == 4 ? 44 : 990;  // C(52 - n - 4, 5 - n)

    order_.resize(size_t(runouts_) * PER);
    tieNext_.resize(order_.size());
    std::vector<uint32_t> keyed;
    keyed.reserve(PER);
    Card c[7];
    for (int i = 0; i < n; ++i) c[2 + i] = cardFromIndex(board[i]);
    for (int r = 0; r < runouts_; ++r) {
        for (int k = 0; k < 5 - n; ++k) c[2 + n + k] = cardFromIndex(runs[r][k]);
        keyed.clear();
        for (int x = 0; x < NCOMBOS; ++x) {
            if (blocked_[x] || lo_[x] == runs[r][0] || lo_[x] == runs[r][1] || hi_[x] == runs[r][0] ||
                hi_[x] == runs[r][1])
                continue;
            c[0] = cardFromIndex(lo_[x]), c[1] = cardFromIndex(hi_[x]);
            keyed.push_back(uint32_t(rank7(c)) << 11 | uint32_t(x));
        }
        std::sort(keyed.begin(), keyed.end());
        uint16_t* o = &order_[size_t(r) * PER];
        uint8_t* t = &tieNext_[size_t(r) * PER];
        for (int i = 0; i < PER; ++i) {
            o[i] = uint16_t(keyed[i] & 2047);
            t[i] = i + 1 < PER && keyed[i] >> 11 == keyed[i + 1] >> 11;
        }
    }
}

void BoardEquity::sums(const ComboWeights& w, std::array<double, NCOMBOS>& num,
                       std::array<double, NCOMBOS>& den) const {
    num.fill(0), den.fill(0);
    // Opponents disjoint from x: all - those holding x's low card - x's high card + x (counted twice).
    double all = 0, card[52] = {};
    for (int y = 0; y < NCOMBOS; ++y)
        if (!blocked_[y]) all += w[y], card[lo_[y]] += w[y], card[hi_[y]] += w[y];
    for (int x = 0; x < NCOMBOS; ++x)
        if (!blocked_[x]) den[x] = all - card[lo_[x]] - card[hi_[x]] + w[x];

    for (int r = 0; r < runouts_; ++r) {
        const uint16_t* o = &order_[size_t(r) * PER];
        const uint8_t* t = &tieNext_[size_t(r) * PER];
        double below = 0, belowCard[52] = {};
        for (int i = 0; i < PER;) {
            int j = i;
            while (t[j]) ++j;  // [i, j] share a rank
            double grp = 0, grpCard[52];
            for (int k = i; k <= j; ++k) grpCard[lo_[o[k]]] = grpCard[hi_[o[k]]] = 0;
            for (int k = i; k <= j; ++k) {
                int y = o[k];
                grp += w[y], grpCard[lo_[y]] += w[y], grpCard[hi_[y]] += w[y];
            }
            for (int k = i; k <= j; ++k) {
                int x = o[k], a = lo_[x], b = hi_[x];
                double win = below - belowCard[a] - belowCard[b];
                double tie = grp - grpCard[a] - grpCard[b] + w[x];  // excludes x: subtracted twice, added once
                num[x] += win + 0.5 * tie;
            }
            for (int k = i; k <= j; ++k) {
                int y = o[k];
                below += w[y], belowCard[lo_[y]] += w[y], belowCard[hi_[y]] += w[y];
            }
            i = j + 1;
        }
    }
    for (int x = 0; x < NCOMBOS; ++x) num[x] /= perPair_;
}

void BoardEquity::equity(const ComboWeights& w, ComboWeights& eq) const {
    std::array<double, NCOMBOS> num, den;
    sums(w, num, den);
    for (int x = 0; x < NCOMBOS; ++x) eq[x] = den[x] > 0 ? float(num[x] / den[x]) : 0;
}

}  // namespace pf
