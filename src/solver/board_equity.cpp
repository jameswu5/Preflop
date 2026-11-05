#include "solver/board_equity.h"

#include <algorithm>

#include "solver/equity.h"
#include "solver/flops.h"

namespace pf {

BoardEquity::BoardEquity(const int* board, int n) {
    bool* used = onBoard_.data();
    nBoard_ = n;
    for (int i = 0; i < n; ++i) used[board[i]] = true;
    for (int x = 0; x < NCOMBOS; ++x) {
        auto [a, b] = comboCards(x);
        lo_[x] = uint16_t(a), hi_[x] = uint16_t(b);
        blocked_[x] = used[a] || used[b];
        if (!blocked_[x]) live_.push_back({uint16_t(x), uint8_t(a), uint8_t(b)});
    }
    for (int c = 0; c < 52; ++c)  // each card off the board with every other card off the board
        for (int d = 0; d < 52; ++d)
            if (!used[c] && !used[d] && d != c) byCard_.push_back(uint16_t(comboIndex(cardFromIndex(c), cardFromIndex(d))));
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

    order_.resize(size_t(runouts_) * 2 * PER);
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
        // By rank, ties by combo ascending; then again with ties descending.
        Entry* o = &order_[size_t(r) * 2 * PER];
        for (int pass = 0; pass < 2; ++pass, o += PER) {
            std::sort(keyed.begin(), keyed.end());
            for (int i = 0; i < PER; ++i) {
                int x = int(keyed[i] & 2047) ^ (pass ? 2047 : 0);
                o[i] = {uint16_t(x), uint8_t(lo_[x]), uint8_t(hi_[x])};
                keyed[i] ^= 2047;  // flips the tie order for the second pass
            }
        }
    }
}

void BoardEquity::sums(const ComboWeights& w, std::array<double, NCOMBOS>& num,
                       std::array<double, NCOMBOS>& den) const {
    num.fill(0), den.fill(0);
    // Opponents disjoint from x: all - those holding x's low card - x's high card + x (counted twice).
    // Each card's total in its own loop: summing combo by combo stalls on neighbours sharing a card.
    double all = 0, card[52] = {};
    const int k = 51 - nBoard_;
    for (int c = 0, i = 0; c < 52; ++c) {
        if (onBoard_[c]) continue;
        const uint16_t* l = &byCard_[size_t(i++) * k];
        double t = 0;
        for (int j = 0; j < k; ++j) t += w[l[j]];
        card[c] = t, all += t;
    }
    all /= 2;
    for (const Entry& q : live_) den[q.x] = all - card[q.a] - card[q.b] + w[q.x];

    // One prefix pass per order: an opponent below x counts in both passes, a tied one in exactly one
    // (it comes before x in one order and after it in the other), so the two passes sum to
    // 2 win + tie. D = the weight so far minus what shares a card with x (x itself is never counted
    // before itself).
    // The two passes are independent, so they run interleaved: two dependency chains at once.
    for (int r = 0; r < runouts_; ++r) {
        const Entry *e = &order_[size_t(r) * 2 * PER], *f = e + PER;
        double below = 0, bc[52] = {}, below2 = 0, bc2[52] = {};
        for (int i = 0; i < PER; ++i) {
            const Entry &q = e[i], &u = f[i];
            float v = w[q.x], v2 = w[u.x];
            double d = below - bc[q.a] - bc[q.b], d2 = below2 - bc2[u.a] - bc2[u.b];
            below += v, bc[q.a] += v, bc[q.b] += v;
            below2 += v2, bc2[u.a] += v2, bc2[u.b] += v2;
            num[q.x] += d, num[u.x] += d2;
        }
    }
    double inv = 0.5 / perPair_;
    for (int x = 0; x < NCOMBOS; ++x) num[x] *= inv;
}

void BoardEquity::equity(const ComboWeights& w, ComboWeights& eq) const {
    std::array<double, NCOMBOS> num, den;
    sums(w, num, den);
    for (int x = 0; x < NCOMBOS; ++x) eq[x] = den[x] > 0 ? float(num[x] / den[x]) : 0;
}

}  // namespace pf
