#undef NDEBUG  // the build defaults to Release; keep asserts on
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <utility>

#include "solver/utility.h"

using namespace pf;
using Eq = std::array<double, NPOS>;
const Act F = Act::Fold, C = Act::Call, R = Act::Raise;

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }
static double total(const Eq& v) { return std::accumulate(v.begin(), v.end(), 0.0); }

static std::mt19937 rng(42);
static const Config NR = [] { Config c; c.rake = 0; return c; }();  // rake off: payoffs sum to 0

static void play(Hand& h, std::initializer_list<Act> acts) {
    for (Act a : acts) h.apply(a);
}

int main() {
    const RealizationConfig rc{1.0, 0.8, 0, 0, 0, 0, 0, 0, 0, 0};  // position-only, so payoffs are hand-checkable
    {  // fold-out: UTG opens, everyone folds; UTG wins the blinds
        Hand h(rng, NR);
        play(h, {R, F, F, F, F, F});
        assert(h.done() && !allIn(h));
        auto u = payoffs(h, {}, rc);
        assert(near(u[UTG], 1.5) && near(u[SB], -0.5) && near(u[BB], -1) && near(u[HJ], 0));
        assert(near(total(u), 0));
    }
    {  // all-in called: BTN open, BB 3-bet, BTN 4-bet, BB 5-bet shove, BTN calls. R ignored.
        Hand h(rng, NR);
        play(h, {F, F, F, R, F, R, R, R, C});
        assert(h.done() && allIn(h) && near(h.pot(), 200.5));
        Eq eq{0, 0, 0, 0.6, 0, 0.4};
        auto u = payoffs(h, eq, rc);
        assert(near(u[BTN], 0.6 * 200.5 - 100) && near(u[BB], 0.4 * 200.5 - 100) && near(u[SB], -0.5));
        assert(near(total(u), 0));
    }
    {  // flop HU: CO opens, BTN calls, blinds fold. BTN is IP.
        Hand h(rng, NR);
        play(h, {F, F, R, C, F, F});
        assert(h.done() && !allIn(h) && near(h.pot(), 6.5));
        auto u = payoffs(h, {0, 0, 0.5, 0.5, 0, 0}, rc);
        assert(near(u[BTN], 0.5 / 0.9 * 6.5 - 2.5) && near(u[CO], 0.4 / 0.9 * 6.5 - 2.5));
        assert(near(total(u), 0));
        // equal R reduces to raw equity
        auto v = payoffs(h, {0, 0, 0.3, 0.7, 0, 0}, {0.9, 0.9, 0, 0, 0, 0, 0, 0, 0, 0});
        assert(near(v[CO], 0.3 * 6.5 - 2.5) && near(v[BTN], 0.7 * 6.5 - 2.5));
    }
    {  // flop multiway: UTG opens, HJ and BB call. HJ acts last postflop among UTG, HJ, BB.
        Hand h(rng, NR);
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
            Hand h(rng, NR);
            while (!h.done()) {
                auto l = h.legal();
                h.apply(l[rng() % l.size()]);
            }
            auto u = utility(h, e, rng, 20, rc);
            assert(std::fabs(total(u)) < 1e-9);
            for (int p = 0; p < NPOS; ++p) assert(u[p] >= -h.committed(p) - 1e-9);
        }
    }
    {  // features, and how each term of the feature-based R behaves
        auto c = [](int rank, int suit) { return Card{uint8_t(rank), uint8_t(suit)}; };
        const int A = 12, K = 11, Q = 10, J = 9, T = 8;
        assert(straights(c(7, 0), c(6, 1)) == 4 && straights(c(K, 0), c(Q, 1)) == 2);   // 98, KQ
        assert(straights(c(A, 0), c(K, 1)) == 1 && straights(c(A, 0), c(3, 1)) == 1);   // AK, A5 (wheel)
        assert(straights(c(A, 0), c(2, 1)) == 1 && straights(c(K, 0), c(2, 1)) == 0);   // A4 (wheel), K4
        assert(straights(c(5, 0), c(0, 1)) == 0 && straights(c(T, 0), c(4, 1)) == 1);   // 72, T6
        assert(straights(c(7, 0), c(7, 1)) == 0);                                        // pairs: none
        assert(domination(c(K, 0), c(Q, 1)) == 0 && domination(c(K, 0), c(J, 1)) == 1);  // KQo, KJo
        assert(domination(c(A, 0), c(J, 1)) == 2 && domination(c(K, 0), c(4, 1)) == 4);  // AJo, K6o (capped)
        assert(domination(c(5, 0), c(4, 1)) == 0 && domination(c(A, 2), c(J, 2)) == 0);  // 76o, AJs
        assert(domination(c(J, 0), c(J, 1)) == 0);                                       // pairs

        // neutral features and spr = 0: position-only, whatever the hand, depth or role
        for (auto [x, y] : {std::pair{c(A, 0), c(A, 1)}, {c(5, 0), c(0, 1)}, {c(3, 2), c(2, 2)}})
            for (bool raiser : {false, true}) {
                assert(near(realization(rc, true, raiser, 1.3, x, y), 1.0));
                assert(near(realization(rc, false, raiser, 18, x, y), 0.8));
            }

        // hand-checked: KJo out of position, calling, SPR 5 (oop .8, conn .03, dom .05, low .03, spr 3)
        const RealizationConfig hc{1.0, 0.8, 0, 0.03, 0, 0.05, 0.03, 3, 0, 0, 1, 1};  // callerShrink 1: fading
        double g = 5 / 8.0, p = 1 - 0.2 * g, h = 1 + 0.03 * 2 * g - 0.05 * 1 - 0.03 * 1;
        assert(near(realization(hc, false, false, 5, c(K, 0), c(J, 1)), p * h));
        RealizationConfig def;
        auto ratio = [&](const RealizationConfig& r, bool raiser, double spr) {  // OOP / IP
            return realization(r, false, raiser, spr, c(K, 0), c(J, 1)) / realization(r, true, raiser, spr, c(K, 0), c(J, 1));
        };
        // position matters less in shallow pots for raisers; OOP callers pay the full penalty by
        // default (callerShrink 0) and fade like raisers only with callerShrink 1
        assert(ratio(def, true, 1.3) > ratio(def, true, 18));
        assert(near(ratio(def, false, 1.3), ratio(def, false, 18)));
        RealizationConfig fade = def;
        fade.callerShrink = 1;
        assert(ratio(fade, false, 1.3) > ratio(fade, false, 18));

        // dom applies only when calling: K6o as caller vs as the last raiser
        RealizationConfig dm = rc;
        dm.dom = 0.05;
        assert(near(realization(dm, true, false, 15, c(K, 0), c(4, 1)), 1 - 0.05 * 4));
        assert(near(realization(dm, true, true, 15, c(K, 0), c(4, 1)), 1.0));

        // set and conn fade with SPR (spr = 3: g = 15/18 in a single-raised pot, 1.3/4.3 in a 4-bet pot)
        RealizationConfig deep = rc;
        deep.set = 0.1, deep.conn = 0.03, deep.spr = 3;
        assert(near(realization(deep, true, false, 15, c(1, 0), c(1, 1)), 1 + 0.1 * 15 / 18));     // 33
        assert(near(realization(deep, true, false, 1.3, c(1, 0), c(1, 1)), 1 + 0.1 * 1.3 / 4.3));
        assert(near(realization(deep, true, false, 15, c(7, 0), c(6, 1)), 1 + 0.03 * 4 * 15 / 18));  // 98o
        assert(near(realization(deep, true, false, 1.3, c(7, 0), c(6, 1)), 1 + 0.03 * 4 * 1.3 / 4.3));
        // flush and low don't depend on depth
        RealizationConfig fl = rc;
        fl.flush = 0.08, fl.low = 0.03, fl.spr = 3;
        assert(near(realization(fl, true, false, 1.3, c(3, 2), c(2, 2)), 1.08) && near(realization(fl, true, false, 18, c(3, 2), c(2, 2)), 1.08));
        assert(near(realization(fl, true, false, 1.3, c(5, 0), c(0, 1)), 1 - 0.03 * 7));  // 72o: 7 ranks below an ace

        // pairLow: per rank below aces, same at any depth (and only for pairs)
        RealizationConfig pl = rc;
        pl.pairLow = 0.02, pl.spr = 3;
        assert(near(realization(pl, true, false, 15, c(A, 0), c(A, 1)), 1.0));                  // AA
        assert(near(realization(pl, true, false, 15, c(0, 0), c(0, 1)), 1 - 12 * 0.02));        // 22
        assert(near(realization(pl, true, false, 1.3, c(0, 0), c(0, 1)), 1 - 12 * 0.02));       // no SPR dependence
        assert(near(realization(pl, true, false, 15, c(5, 0), c(4, 0)), 1.0));                  // 76s: not a pair

        RealizationConfig harsh = rc;
        harsh.low = 0.5;
        assert(near(realization(harsh, true, false, 15, c(5, 0), c(0, 1)), 0.05));  // floored
    }
    {  // pot-dependent R in payoffs: position fades with SPR, the last raiser's initiative bonus
        Eq eq{0, 0, 0.5, 0.5, 0, 0};
        RealizationConfig s5 = rc;
        s5.spr = 5;
        s5.callerShrink = 1;  // this block checks the depth fade, including for the OOP caller below
        Hand srp(rng, NR);  // CO opens, BTN calls: pot 6.5, 97.5 behind, SPR 15
        play(srp, {F, F, R, C, F, F});
        double co = 1 - 0.2 * 15 / 20.0;  // 0.85
        auto u = payoffs(srp, eq, s5);
        assert(near(u[CO], co / (co + 1) * 6.5 - 2.5) && near(u[BTN], 1 / (co + 1) * 6.5 - 2.5) && near(total(u), 0));

        Hand tb(rng, NR);  // CO opens, BTN 3-bets to 8.8, CO calls: pot 19.1, 91.2 behind, SPR 4.77
        play(tb, {F, F, R, R, F, F, C});
        assert(tb.done() && near(tb.pot(), 19.1) && tb.lastRaiser() == BTN);
        double spr = 91.2 / 19.1;
        co = 1 - 0.2 * spr / (spr + 5);  // shrinks more than in the SRP
        assert(co > 0.85);
        u = payoffs(tb, eq, s5);
        assert(near(u[CO], co / (co + 1) * 19.1 - 8.8) && near(total(u), 0));

        RealizationConfig ini = rc;  // initiative alone: CO raised, BTN called
        ini.init = 0.2;
        co = 0.8 * 1.2;
        u = payoffs(srp, eq, ini);
        assert(near(u[CO], co / (co + 1) * 6.5 - 2.5) && near(u[BTN], 1 / (co + 1) * 6.5 - 2.5) && near(total(u), 0));

        ini.spr = 5;  // both: the OOP penalty fades with the SPR, the raiser's bonus doesn't
        ini.callerShrink = 1;  // the OOP player here (CO) is a caller
        double btn = 1.2;
        co = 1 - 0.2 * spr / (spr + 5);
        u = payoffs(tb, eq, ini);
        assert(near(u[BTN], btn / (btn + co) * 19.1 - 8.8) && near(total(u), 0));

        Equity e;  // random terminals, every R term on, default rake: payoffs sum to -rake
        RealizationConfig all;
        all.init = 0.1;
        for (int i = 0; i < 200; ++i) {
            Hand h(rng);
            while (!h.done()) {
                auto l = h.legal();
                h.apply(l[rng() % l.size()]);
            }
            assert(near(total(utility(h, e, rng, 20, all)), -rake(h)) && near(total(utility(h, e, rng, 20, ini)), -rake(h)));
        }
    }
    {  // oopCall / callerShrink: only out-of-position callers change, and the defaults change nothing
        auto c = [](int rank, int suit) { return Card{uint8_t(rank), uint8_t(suit)}; };
        auto before = [](const RealizationConfig& r, bool ip, bool raiser, double spr, Card a, Card b) {  // previous model
            int hi = std::max(a.rank, b.rank);
            bool pair = a.rank == b.rank, suited = a.suit == b.suit;
            double g = spr / (spr + r.spr), pos = ip ? r.ip : 1 - (1 - r.oop) * g;
            double h = 1 + r.flush * suited + r.conn * straights(a, b) * g + r.set * pair * g -
                       r.dom * domination(a, b) * !raiser - r.low * (12 - hi) * (!pair && !suited) - r.pairLow * (12 - hi) * pair;
            return std::max(pos * h * (raiser ? 1 + r.init : 1), 0.05);
        };
        RealizationConfig def;
        def.callerShrink = 1;  // the previous formula: every OOP player's penalty fades with depth
        for (int x = 0; x < 13; ++x)
            for (int y = 0; y <= x; ++y)
                for (int suit : {0, 1})  // suited and offsuit (pairs: offsuit only)
                    for (bool ip : {false, true})
                        for (bool raiser : {false, true})
                            for (double spr : {1.3, 5.0, 18.0}) {
                                if (x == y && suit == 0) continue;
                                Card a = c(x, 0), b = c(y, suit == 0 ? 0 : 1);
                                assert(near(realization(def, ip, raiser, spr, a, b), before(def, ip, raiser, spr, a, b)));
                            }

        const Card a5s[2] = {c(12, 2), c(3, 2)};
        double g = 5 / 8.0;  // SPR 5, spr 3
        double play = def.flush + def.conn * straights(a5s[0], a5s[1]) * g;  // A5s: suited, 1 straight
        RealizationConfig a1 = def;  // A1: OOP callers keep half their playability bonus
        a1.oopCall = 0.5;
        double oopPos = 1 - (1 - def.oop) * g;
        assert(near(realization(a1, false, false, 5, a5s[0], a5s[1]), oopPos * (1 + 0.5 * play)));
        assert(near(realization(a1, false, false, 5, a5s[0], a5s[1]),
                    realization(def, false, false, 5, a5s[0], a5s[1]) - oopPos * 0.5 * play));
        for (auto [ip, raiser] : {std::pair{true, false}, {false, true}, {true, true}})  // everyone else unchanged
            assert(near(realization(a1, ip, raiser, 5, a5s[0], a5s[1]), realization(def, ip, raiser, 5, a5s[0], a5s[1])));

        RealizationConfig a2 = def;  // A2: callerShrink 0 = full OOP penalty at any depth, for callers only
        a2.callerShrink = 0;
        double h = 1 + play;
        assert(near(realization(a2, false, false, 5, a5s[0], a5s[1]), def.oop * h));
        for (double spr : {1.3, 18.0}) {  // 22 as an OOP caller: position factor is oop at any depth
            double h22 = 1 + def.set * spr / (spr + def.spr) - 12 * def.pairLow;
            assert(near(realization(a2, false, false, spr, c(0, 0), c(0, 1)), def.oop * h22));
        }
        a2.callerShrink = 0.5;  // halfway: the fade factor is 0.5 * g + 0.5
        assert(near(realization(a2, false, false, 5, a5s[0], a5s[1]), (1 - (1 - def.oop) * (0.5 * g + 0.5)) * h));
        for (auto [ip, raiser] : {std::pair{true, false}, {false, true}, {true, true}})
            assert(near(realization(a2, ip, raiser, 5, a5s[0], a5s[1]), realization(def, ip, raiser, 5, a5s[0], a5s[1])));

        Equity e;  // both on: payoffs still sum to -rake
        RealizationConfig both = def;
        both.oopCall = 0.3, both.callerShrink = 0.2;
        for (int i = 0; i < 200; ++i) {
            Hand hd(rng);
            while (!hd.done()) {
                auto l = hd.legal();
                hd.apply(l[rng() % l.size()]);
            }
            assert(near(total(utility(hd, e, rng, 20, both)), -rake(hd)));
        }
    }
    {  // ipCall / domPot: only callers in re-raised pots change, and the defaults change nothing
        auto c = [](int rank, int suit) { return Card{uint8_t(rank), uint8_t(suit)}; };
        RealizationConfig def;
        for (int x = 0; x < 13; ++x)
            for (int y = 0; y <= x; ++y)
                for (bool ip : {false, true})
                    for (bool raiser : {false, true})
                        assert(realization(def, ip, raiser, 4, c(x, 0), c(y, 1), true) ==
                               realization(def, ip, raiser, 4, c(x, 0), c(y, 1), false));
        RealizationConfig dp = rc;  // KJo, domination 1: dom 0, domPot 0.1
        dp.domPot = 0.1;
        assert(near(realization(dp, true, false, 4, c(11, 0), c(9, 1), true), 0.9));
        assert(near(realization(dp, false, false, 4, c(11, 0), c(9, 1), true), 0.8 * 0.9));
        assert(near(realization(dp, true, false, 4, c(11, 0), c(9, 1), false), 1));  // single-raised pot
        assert(near(realization(dp, true, true, 4, c(11, 0), c(9, 1), true), 1));    // the raiser

        RealizationConfig ic = rc;
        ic.ipCall = 0.5;
        Eq eq{0, 0, 0.5, 0, 0, 0.5};
        Hand srp(rng, NR);  // CO opens, BB calls: CO is in position but the raiser, and the pot is single-raised
        play(srp, {F, F, R, F, F, C});
        assert(payoffs(srp, eq, ic) == payoffs(srp, eq, rc));
        Hand tb(rng, NR);  // CO opens, BB 3-bets, CO calls: CO is the in-position caller of a 3-bet
        play(tb, {F, F, R, F, F, R, C});
        assert(tb.done() && tb.raises() == 2 && tb.lastRaiser() == BB);
        auto u = payoffs(tb, eq, ic);
        assert(near(u[CO], 0.5 / 1.3 * tb.pot() - tb.committed(CO)) && near(total(u), 0));
        RealizationConfig both = rc;
        both.ipCall = 0.5, both.domPot = 0.1;
        Hand ai(rng, NR);  // all-in called: R is ignored
        play(ai, {F, F, F, R, F, R, R, R, C});
        assert(payoffs(ai, {0, 0, 0, 0.6, 0, 0.4}, both) == payoffs(ai, {0, 0, 0, 0.6, 0, 0.4}, rc));
    }
    {  // rake: 5% capped at 4bb, only when a flop is dealt or an all-in is called
        Hand fo(rng);  // UTG opens, all fold: no flop, no drop
        play(fo, {R, F, F, F, F, F});
        assert(rake(fo) == 0 && near(payoffs(fo, {}, rc)[UTG], 1.5) && near(total(payoffs(fo, {}, rc)), 0));

        Hand fl(rng);  // CO opens, BTN calls: pot 6.5, rake 0.325
        play(fl, {F, F, R, C, F, F});
        assert(near(rake(fl), 0.325));
        auto u = payoffs(fl, {0, 0, 0.5, 0.5, 0, 0}, rc);  // CO OOP 0.8, BTN 1
        assert(near(u[BTN], 1 / 1.8 * (6.5 - 0.325) - 2.5) && near(u[CO], 0.8 / 1.8 * (6.5 - 0.325) - 2.5));
        assert(near(u[SB], -0.5) && near(total(u), -0.325));

        Hand ai(rng);  // 5-bet all-in called: pot 200.5, 5% = 10.0 > cap, so 4
        play(ai, {F, F, F, R, F, R, R, R, C});
        assert(allIn(ai) && near(rake(ai), 4));
        u = payoffs(ai, {0, 0, 0, 0.6, 0, 0.4}, rc);
        assert(near(u[BTN], 0.6 * 196.5 - 100) && near(u[BB], 0.4 * 196.5 - 100) && near(total(u), -4));

        Config low;  // a cap that binds on a flop pot
        low.rake = 0.1, low.rakeCap = 0.5;
        Hand cp(rng, low);
        play(cp, {F, F, R, C, F, F});
        assert(near(rake(cp), 0.5) && near(total(payoffs(cp, {0, 0, 0.5, 0.5, 0, 0}, rc)), -0.5));
    }
    std::puts("utility_test: ok");
    return 0;
}
