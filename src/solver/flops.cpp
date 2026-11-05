#include "solver/flops.h"

#include <algorithm>
#include <numeric>

#include "solver/cards.h"

namespace pf {

static int colex(int a, int b, int c) {  // a < b < c -> 0..22099
    return c * (c - 1) * (c - 2) / 6 + b * (b - 1) / 2 + a;
}

namespace {
// Every flop's class, indexed by colex() of its sorted cards. The canonical flop is the smallest
// sorted triple over the 24 suit relabelings.
struct Flops {
    std::vector<CanonFlop> canon;
    std::vector<FlopClass> cls = std::vector<FlopClass>(22100);
    Flops() {
        std::vector<SuitPerm> perms;
        SuitPerm p = {0, 1, 2, 3};
        do perms.push_back(p);
        while (std::next_permutation(p.begin(), p.end()));
        std::vector<int> id(22100, -1);  // canonical colex -> canon index
        for (int c = 2; c < 52; ++c)
            for (int b = 1; b < c; ++b)
                for (int a = 0; a < b; ++a) {
                    std::array<int, 3> best = {52, 52, 52};
                    SuitPerm bestP{};
                    for (auto& q : perms) {
                        std::array<int, 3> t = {permuteCard(a, q), permuteCard(b, q), permuteCard(c, q)};
                        std::sort(t.begin(), t.end());
                        if (t < best) best = t, bestP = q;
                    }
                    int& k = id[colex(best[0], best[1], best[2])];
                    if (k < 0) k = int(canon.size()), canon.push_back({best, 0});
                    ++canon[k].count;
                    cls[colex(a, b, c)] = {k, bestP};
                }
        // Order by cards; renumber.
        std::vector<int> order(canon.size()), rank(canon.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int x, int y) { return canon[x].cards < canon[y].cards; });
        std::vector<CanonFlop> sorted;
        for (int i = 0; i < int(order.size()); ++i) rank[order[i]] = i, sorted.push_back(canon[order[i]]);
        canon = std::move(sorted);
        for (auto& f : cls) f.canon = rank[f.canon];
    }
};
const Flops& flops() {
    static const Flops f;
    return f;
}
}  // namespace

const std::vector<CanonFlop>& canonicalFlops() { return flops().canon; }

FlopClass canonicalFlop(int a, int b, int c) {
    int t[3] = {a, b, c};
    std::sort(t, t + 3);
    return flops().cls[colex(t[0], t[1], t[2])];
}

std::array<int, 2> comboCards(int combo) {
    static const auto table = [] {
        std::array<std::array<int, 2>, NCOMBOS> t;
        for (int j = 1; j < 52; ++j)
            for (int i = 0; i < j; ++i) t[j * (j - 1) / 2 + i] = {i, j};
        return t;
    }();
    return table[combo];
}

int permuteCombo(int combo, const SuitPerm& p) {
    auto [a, b] = comboCards(combo);
    return comboIndex(cardFromIndex(permuteCard(a, p)), cardFromIndex(permuteCard(b, p)));
}

}  // namespace pf
