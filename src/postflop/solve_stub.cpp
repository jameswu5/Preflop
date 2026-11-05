// Stand-in for the postflop solver until it lands: every combo realizes its equity exactly
// (ev = equity * pot, so R = 1), with equity a flat 0.5. Weights are real: the range after card
// removal against the flop and the opponent's range. Lets the realization-table pipeline run end to end.
// ponytail: delete once src/postflop's solve() is merged; link that instead.
#include "postflop/postflop.h"

namespace pf::post {

Result solve(const Spot& spot, const Options&) {
    struct Combo { int c, a, b; };
    std::vector<Combo> live;  // combos not blocked by the flop
    for (int a = 0; a < 52; ++a)
        for (int b = a + 1; b < 52; ++b)
            if (a != spot.flop[0] && a != spot.flop[1] && a != spot.flop[2] && b != spot.flop[0] &&
                b != spot.flop[1] && b != spot.flop[2])
                live.push_back({comboIndex(cardFromIndex(a), cardFromIndex(b)), a, b});
    Result r;
    for (int p = 0; p < 2; ++p) {
        const ComboWeights &me = spot.range[p], &opp = spot.range[1 - p];
        double total = 0;
        for (auto& x : live) {
            if (me[x.c] == 0) continue;
            double vs = 0;
            for (auto& y : live)
                if (y.a != x.a && y.a != x.b && y.b != x.a && y.b != x.b) vs += opp[y.c];
            total += r.weight[p][x.c] = float(me[x.c] * vs);
        }
        for (auto& x : live)
            if (r.weight[p][x.c] > 0) {
                r.weight[p][x.c] = float(r.weight[p][x.c] / total);
                r.equity[p][x.c] = 0.5f, r.ev[p][x.c] = float(0.5 * spot.tree.pot);
            }
    }
    return r;
}

}  // namespace pf::post
