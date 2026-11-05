#undef NDEBUG  // the build defaults to Release; keep asserts on
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>

#include "solver/board_equity.h"
#include "solver/equity.h"
#include "solver/flops.h"

using namespace pf;

// eq(x, y) on `board` by brute force over the completions.
static double bruteEq(int x, int y, const int* board, int n) {
    bool used[52] = {};
    for (int i = 0; i < n; ++i) used[board[i]] = true;
    auto cx = comboCards(x), cy = comboCards(y);
    for (int c : {cx[0], cx[1], cy[0], cy[1]}) used[c] = true;
    double sum = 0;
    int cnt = 0;
    auto score = [&](const int* b5) {
        Card hx[7], hy[7];
        for (int i = 0; i < 5; ++i) hx[2 + i] = hy[2 + i] = cardFromIndex(b5[i]);
        hx[0] = cardFromIndex(cx[0]), hx[1] = cardFromIndex(cx[1]);
        hy[0] = cardFromIndex(cy[0]), hy[1] = cardFromIndex(cy[1]);
        int a = rank7(hx), b = rank7(hy);
        sum += a > b ? 1 : a == b ? 0.5 : 0, ++cnt;
    };
    int b5[5];
    for (int i = 0; i < n; ++i) b5[i] = board[i];
    if (n == 5) score(b5);
    for (int t = 0; t < 52 && n < 5; ++t) {
        if (used[t]) continue;
        b5[n] = t;
        if (n == 4) { score(b5); continue; }
        for (int r = t + 1; r < 52; ++r)
            if (!used[r]) b5[4] = r, score(b5);
    }
    return sum / cnt;
}

int main() {
    std::mt19937 rng(7);
    for (int n : {5, 4, 3}) {
        for (int trial = 0; trial < 3; ++trial) {
            std::vector<int> deck(52);
            for (int i = 0; i < 52; ++i) deck[i] = i;
            std::shuffle(deck.begin(), deck.end(), rng);
            int board[5] = {deck[0], deck[1], deck[2], deck[3], deck[4]};
            BoardEquity be(board, n);
            // A small random range that includes combos sharing cards with each other and with x.
            ComboWeights w{};
            std::vector<int> ys;
            for (int k = 0; k < 40; ++k) {
                int y = std::uniform_int_distribution<int>(0, NCOMBOS - 1)(rng);
                if (be.blocked(y) || w[y] > 0) continue;
                w[y] = std::uniform_real_distribution<float>(0.1f, 1)(rng), ys.push_back(y);
            }
            std::array<double, NCOMBOS> num, den;
            be.sums(w, num, den);
            for (int x = 0; x < NCOMBOS; x += 7) {
                if (be.blocked(x)) { assert(num[x] == 0 && den[x] == 0); continue; }
                auto cx = comboCards(x);
                double en = 0, ed = 0;
                for (int y : ys) {
                    auto cy = comboCards(y);
                    if (cy[0] == cx[0] || cy[0] == cx[1] || cy[1] == cx[0] || cy[1] == cx[1]) continue;
                    ed += w[y], en += w[y] * bruteEq(x, y, board, n);
                }
                assert(std::fabs(den[x] - ed) < 1e-4 && std::fabs(num[x] - en) < 1e-4);
            }
        }
    }
    // Against the uniform range the flop equities of x and of every y average to 1/2.
    int board[3] = {0, 17, 50};
    BoardEquity be(board, 3);
    ComboWeights w;
    w.fill(1);
    ComboWeights eq;
    be.equity(w, eq);
    double s = 0;
    for (int x = 0; x < NCOMBOS; ++x) s += be.blocked(x) ? 0 : eq[x];
    assert(std::fabs(s / 1176 - 0.5) < 1e-6);
    std::puts("board_equity_test OK");
}
