#undef NDEBUG  // the build defaults to Release; keep asserts on
#include <cassert>
#include <cmath>
#include <cstdio>
#include <numeric>

#include "solver/utility.h"

using namespace pf;
using Eq = std::array<double, NPOS>;
const Act F = Act::Fold, C = Act::Call, R = Act::Raise;

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }
static double total(const Eq& v) { return std::accumulate(v.begin(), v.end(), 0.0); }

static std::mt19937 rng(42);

static void play(Hand& h, std::initializer_list<Act> acts) {
    for (Act a : acts) h.apply(a);
}

int main() {
    const RealizationConfig rc{1.0, 0.8, 1, 1, 1, 0, 0};  // position-only, so payoffs are hand-checkable
    {  // fold-out: UTG opens, everyone folds; UTG wins the blinds
        Hand h(rng);
        play(h, {R, F, F, F, F, F});
        assert(h.done() && !allIn(h));
        auto u = payoffs(h, {}, rc);
        assert(near(u[UTG], 1.5) && near(u[SB], -0.5) && near(u[BB], -1) && near(u[HJ], 0));
        assert(near(total(u), 0));
    }
    {  // all-in called: BTN open, BB 3-bet, BTN 4-bet, BB 5-bet shove, BTN calls. R ignored.
        Hand h(rng);
        play(h, {F, F, F, R, F, R, R, R, C});
        assert(h.done() && allIn(h) && near(h.pot(), 200.5));
        Eq eq{0, 0, 0, 0.6, 0, 0.4};
        auto u = payoffs(h, eq, rc);
        assert(near(u[BTN], 0.6 * 200.5 - 100) && near(u[BB], 0.4 * 200.5 - 100) && near(u[SB], -0.5));
        assert(near(total(u), 0));
    }
    {  // flop HU: CO opens, BTN calls, blinds fold. BTN is IP.
        Hand h(rng);
        play(h, {F, F, R, C, F, F});
        assert(h.done() && !allIn(h) && near(h.pot(), 6.5));
        auto u = payoffs(h, {0, 0, 0.5, 0.5, 0, 0}, rc);
        assert(near(u[BTN], 0.5 / 0.9 * 6.5 - 2.5) && near(u[CO], 0.4 / 0.9 * 6.5 - 2.5));
        assert(near(total(u), 0));
        // equal R reduces to raw equity
        auto v = payoffs(h, {0, 0, 0.3, 0.7, 0, 0}, {0.9, 0.9, 1, 1, 1, 0, 0});
        assert(near(v[CO], 0.3 * 6.5 - 2.5) && near(v[BTN], 0.7 * 6.5 - 2.5));
    }
    {  // flop multiway: UTG opens, HJ and BB call. HJ acts last postflop among UTG, HJ, BB.
        Hand h(rng);
        play(h, {R, C, F, F, F, C});
        assert(h.done() && !allIn(h) && near(h.pot(), 8));
        Eq eq{0.4, 0.3, 0, 0, 0, 0.3};
        auto u = payoffs(h, eq, rc);
        double sum = 0.8 * 0.4 + 1.0 * 0.3 + 0.8 * 0.3;
        assert(near(u[HJ], 0.3 / sum * 8 - 2.5) && near(u[BB], 0.24 / sum * 8 - 2.5));
        assert(near(u[UTG], 0.32 / sum * 8 - 2.5) && near(u[SB], -0.5));
        assert(near(total(u), 0));
        assert(u[HJ] > u[BB]);  // same equity, IP realizes more
    }
    {  // wrapper: random deals through Equity (no HU table), every terminal sums to zero
        Equity e;
        for (int i = 0; i < 200; ++i) {
            Hand h(rng);
            while (!h.done()) {
                auto l = h.legal();
                h.apply(l[rng() % l.size()]);
            }
            auto u = utility(h, e, rng, 20, rc);
            assert(std::fabs(total(u)) < 1e-9);
            for (int p = 0; p < NPOS; ++p) assert(u[p] >= -h.committed(p) - 1e-9);
        }
    }
    {  // hand-class realization: neutral class terms are position-only; each term moves R as documented
        auto c = [](int rank, int suit) { return Card{uint8_t(rank), uint8_t(suit)}; };
        RealizationConfig d{1.0, 0.85, 1, 1, 1, 0, 0};
        assert(near(realization(d, true, c(5, 0), c(0, 1)), 1.0) && near(realization(d, false, c(5, 0), c(0, 1)), 0.85));
        RealizationConfig t{1.0, 0.8, 1.1, 1.05, 0.9, 0.05, 0.02};
        assert(near(realization(t, true, c(12, 0), c(12, 1)), 1.1));                 // AA: pair, no low penalty
        assert(near(realization(t, true, c(12, 0), c(11, 0)), 1.05));                // AKs: connected
        assert(near(realization(t, false, c(12, 0), c(9, 1)), 0.8 * (0.9 - 0.1)));   // AJo: 2 gaps, OOP
        assert(near(realization(t, true, c(5, 0), c(0, 1)), 0.9 - 0.2 - 0.14));      // 72o: gaps capped at 4, 7 below A
        RealizationConfig harsh{1.0, 0.8, 1, 1, 0.1, 0.5, 0.5};
        assert(near(realization(harsh, true, c(5, 0), c(0, 1)), 0.05));             // floored
    }
    std::puts("utility_test: ok");
    return 0;
}
