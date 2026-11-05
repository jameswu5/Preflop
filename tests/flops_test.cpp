#undef NDEBUG  // the build defaults to Release; keep asserts on
#include <algorithm>
#include <cassert>
#include <cstdio>

#include "solver/cards.h"
#include "solver/flops.h"

using namespace pf;

int main() {
    auto& canon = canonicalFlops();
    assert(canon.size() == 1755);
    int total = 0;
    for (size_t i = 0; i < canon.size(); ++i) {
        auto& f = canon[i];
        total += f.count;
        assert(f.cards[0] < f.cards[1] && f.cards[1] < f.cards[2]);
        assert(i == 0 || canon[i - 1].cards < f.cards);
        assert(canonicalFlop(f.cards[2], f.cards[0], f.cards[1]).canon == int(i));  // fixed point
        // Trips 4; paired 12 (kicker suit in the pair or not); unpaired: monotone 4, two-tone 12, rainbow 24.
        assert(f.count == 4 || f.count == 12 || f.count == 24);
    }
    assert(total == 22100);

    // Every flop under every suit relabeling: same class, and perm maps it onto the canonical cards.
    SuitPerm q = {0, 1, 2, 3};
    std::vector<int> seen(canon.size());
    for (int c = 2; c < 52; ++c)
        for (int b = 1; b < c; ++b)
            for (int a = 0; a < b; ++a) {
                FlopClass k = canonicalFlop(a, b, c);
                ++seen[k.canon];
                int t[3] = {permuteCard(a, k.perm), permuteCard(b, k.perm), permuteCard(c, k.perm)};
                std::sort(t, t + 3);
                for (int i = 0; i < 3; ++i) assert(t[i] == canon[k.canon].cards[i]);
                do assert(canonicalFlop(permuteCard(c, q), permuteCard(a, q), permuteCard(b, q)).canon == k.canon);
                while (std::next_permutation(q.begin(), q.end()));
            }
    for (size_t i = 0; i < canon.size(); ++i) assert(seen[i] == canon[i].count);

    // Two known classes: AsAhAd stands for 4 flops, a monotone K72 for 4, a rainbow K72 for 24.
    auto ci = [](int rank, int suit) { return rank * 4 + suit; };
    assert(canon[canonicalFlop(ci(12, 3), ci(12, 2), ci(12, 1)).canon].count == 4);
    assert(canon[canonicalFlop(ci(11, 0), ci(5, 0), ci(0, 0)).canon].count == 4);
    assert(canon[canonicalFlop(ci(11, 0), ci(5, 1), ci(0, 2)).canon].count == 24);

    // The sample: every k-th flop, all of them for n <= 0.
    assert(sampleCanonicalFlops(0).size() == 1755 && sampleCanonicalFlops(50).size() == 50);
    auto s50 = sampleCanonicalFlops(50);
    assert(s50[0].cards == canon[0].cards && s50[1].cards == canon[1755 / 50].cards);

    // Combo decoding and relabeling.
    for (int x = 0; x < NCOMBOS; ++x) {
        auto [a, b] = comboCards(x);
        assert(a < b && comboIndex(cardFromIndex(a), cardFromIndex(b)) == x);
        SuitPerm p = {2, 0, 3, 1}, inv = {1, 3, 0, 2};
        int y = permuteCombo(x, p);
        assert(bucket(cardFromIndex(a), cardFromIndex(b)) == bucket(cardFromIndex(comboCards(y)[0]), cardFromIndex(comboCards(y)[1])));
        assert(permuteCombo(y, inv) == x);
    }
    std::puts("flops_test OK");
}
