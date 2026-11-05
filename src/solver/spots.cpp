#include "solver/spots.h"

#include <algorithm>
#include <random>

namespace pf {

static void walk(const Hand& h, std::vector<Act>& hist, std::vector<FlopSpot>& out, bool dead) {
    if (!h.done()) {
        for (Act a : h.legal()) {
            Hand c = h;
            c.apply(a);
            hist.push_back(a);
            walk(c, hist, out, dead);
            hist.pop_back();
        }
        return;
    }
    FlopSpot s;
    if (!headsUpFlop(h, s.pot, s.raiser, s.caller, dead)) return;
    s.history = hist, s.potBB = h.pot(), s.stackBB = h.stack(s.caller);
    out.push_back(s);
}

std::vector<FlopSpot> flopSpots(const Config& cfg, bool dead) {
    std::mt19937 rng(0);  // cards don't affect the betting
    std::vector<FlopSpot> out;
    std::vector<Act> hist;
    walk(Hand(rng, cfg), hist, out, dead);
    std::stable_sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.pot < b.pot; });
    return out;
}

post::ComboWeights lineRange(const Strategy& s, const std::vector<Act>& history, int seat) {
    auto reach = s.reach(history, seat);
    post::ComboWeights w{};
    for (int i = 0; i < 52; ++i)
        for (int j = i + 1; j < 52; ++j) {
            Card a = cardFromIndex(i), b = cardFromIndex(j);
            w[comboIndex(a, b)] = float(reach[bucket(a, b)]);
        }
    return w;
}

void RealizationAccum::add(const post::Result& r, double m) {
    for (int a = 0; a < 52; ++a)
        for (int b = a + 1; b < 52; ++b) {
            Card x = cardFromIndex(a), y = cardFromIndex(b);
            int c = comboIndex(x, y), k = bucket(x, y);
            for (int side = 0; side < 2; ++side) {
                double w = m * r.weight[side][c];
                if (w == 0) continue;
                int role = (side == 0 ? spot.oop() : spot.ip()) == spot.raiser ? 0 : 1;
                ev[role][k] += w * r.ev[side][c], eq[role][k] += w * r.equity[side][c], n[role][k] += w;
            }
        }
    ++flops;
}

RealizationEntry RealizationAccum::entry() const {
    RealizationEntry e;
    e.pot = spot.pot, e.raiser = spot.raiser, e.caller = spot.caller;
    e.potBB = spot.potBB, e.stackBB = spot.stackBB, e.flops = flops;
    for (int k = 0; k < NBUCKETS; ++k) {
        auto r = [&](int role) { return eq[role][k] > 0 ? float(ev[role][k] / (eq[role][k] * spot.potBB)) : 0.0f; };
        e.rRaiser[k] = r(0), e.rCaller[k] = r(1), e.nRaiser[k] = float(n[0][k]), e.nCaller[k] = float(n[1][k]);
    }
    return e;
}

}  // namespace pf
