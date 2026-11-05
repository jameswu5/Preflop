#undef NDEBUG  // the build defaults to Release; keep asserts on
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "postflop/postflop.h"
#include "solver/range.h"

using namespace pf;
using namespace pf::post;

static int card(const char* s) {
    return cardIndex({uint8_t(strchr("23456789TJQKA", s[0]) - "23456789TJQKA"), uint8_t(strchr("cdhs", s[1]) - "cdhs")});
}

static ComboWeights range(const char* text) {
    RangeFreqs f;
    assert(parseRange(text, f));
    ComboWeights w{};
    for (int b = 1; b < 52; ++b)
        for (int a = 0; a < b; ++a) w[comboIndex(cardFromIndex(a), cardFromIndex(b))] = f[bucket(cardFromIndex(a), cardFromIndex(b))];
    return w;
}

static Spot spot(const char* flop, const char* oop, const char* ip, double pot, double stack) {
    Spot s;
    for (int i = 0; i < 3; ++i) s.flop[i] = card(flop + 2 * i);
    s.range = {range(oop), range(ip)};
    s.tree.pot = pot, s.tree.stack = stack;
    s.tree.sizes = {StreetSizes{{0.5}, {2.5}}, StreetSizes{{0.66}, {2.5}}, StreetSizes{{0.66}, {2.5}}};
    return s;
}

static double total(const Result& r) {  // weight-averaged ev of both players; the pot by the header
    double t = 0;
    for (int p = 0; p < 2; ++p)
        for (int c = 0; c < NCOMBOS; ++c) t += double(r.weight[p][c]) * r.ev[p][c];
    return t;
}

static double classEv(const Result& r, int p, const char* name) {
    double w = 0, ev = 0;
    for (int b = 1; b < 52; ++b)
        for (int a = 0; a < b; ++a) {
            int c = comboIndex(cardFromIndex(a), cardFromIndex(b));
            if (bucket(cardFromIndex(a), cardFromIndex(b)) == bucketFromName(name)) w += r.weight[p][c], ev += r.weight[p][c] * r.ev[p][c];
        }
    return ev / w;
}

int main() {
    // Polar river: AA (nuts) and QJ (air) against 88 (bluff-catcher), one pot-sized bet = all-in.
    // Equilibrium: IP calls 1/2, OOP bluffs to make it indifferent, so AA wins 1.5 pots and QJ 0.
    {
        Spot s = spot("KcKd7h", "AA,QJ", "88", 10, 10);
        s.turn = card("4s"), s.river = card("2c");
        s.tree.sizes[2] = {{1.0}, {}};
        Options o;
        o.maxIters = 5000, o.targetExpl = 1e-4, o.threads = 1;
        Result r = solve(s, o);
        std::printf("river: %d iterations, exploitability %.5f, AA %.4f QJ %.4f\n", r.iterations, r.exploitability,
                    classEv(r, 0, "AA"), classEv(r, 0, "QJs"));
        assert(r.exploitability <= 1e-4);
        assert(std::abs(total(r) - 10) < 1e-3);
        assert(std::abs(classEv(r, 0, "AA") - 15) < 0.02 && std::abs(classEv(r, 0, "QJo")) < 0.02);
        assert(std::abs(classEv(r, 1, "88") - (10 - 6.0 / 22 * 15)) < 0.02);  // the rest of the pot
    }

    // No stack: the hand checks down, so ev is equity times the pot over every runout, which checks
    // the chance nodes, suit isomorphism (monotone flop: three interchangeable suits) and showdowns.
    // The same on a fixed turn, whose all-in showdowns take a different path.
    for (int turn : {-1, card("2s")}) {
        Spot s = spot("Ts9s6s", "AA,KK,AKs,T9s,76s,QJo,A5s", "QQ,JJ,KQs,98s,AJo,87s", 6, 0);
        s.turn = turn;
        Options o;
        o.maxIters = 1, o.threads = 1;
        Result r = solve(s, o);
        double eq = 0;
        for (int p = 0; p < 2; ++p)
            for (int c = 0; c < NCOMBOS; ++c) {
                assert(std::abs(r.ev[p][c] - 6 * r.equity[p][c]) < 1e-4);
                eq += r.weight[p][c] * r.equity[p][c];
            }
        assert(std::abs(eq - 1) < 1e-5 && std::abs(total(r) - 6) < 1e-4);
    }

    // A fixed-turn game with bets: its turn all-ins pay out by sweeping each river. (The zero-stack
    // check can't catch reach errors there, since with nothing put in they cancel.)
    {
        Spot s = spot("Td9d6h", "AA,KK,TT,99,AK,KQs,T9s,87s,76s,A5s", "QQ,JJ,AQ,AJs,KJs,QJs,J8s,98s,66", 5.5, 6);
        s.turn = card("2c");
        Options o;
        o.maxIters = 200, o.targetExpl = 0, o.threads = 1;
        Result r = solve(s, o);
        assert(r.exploitability < 0.001 && std::abs(total(r) - 5.5) < 1e-3);
    }

    // A small flop game: exploitability falls, the ev identity holds, threads don't change the
    // answer, and a suit-relabelled flop gives the same answer. That comparison runs few iterations:
    // per-combo ev isn't unique at equilibrium, so float summation order drifts it apart later.
    {
        const char *oop = "AA,KK,TT,99,AK,KQs,T9s,87s,76s,A5s", *ip = "QQ,JJ,AQ,AJs,KJs,QJs,J8s,98s,66";
        Spot s = spot("Td9d6h", oop, ip, 5.5, 6);
        Options o;
        o.targetExpl = 0, o.threads = 1;
        o.maxIters = 20;
        double e20 = solve(s, o).exploitability;
        o.maxIters = 200;
        Result r = solve(s, o);
        std::printf("flop: exploitability %.5f after 20, %.5f after 200\n", e20, r.exploitability);
        assert(r.exploitability < e20 / 10 && r.exploitability < 0.001);
        assert(std::abs(total(r) - 5.5) < 1e-3);

        o.threads = 3;
        Result r3 = solve(s, o);
        assert(r3.exploitability == r.exploitability && r3.ev == r.ev);

        Spot t = spot("Th9h6d", oop, ip, 5.5, 6);  // d <-> h
        o.threads = 1, o.maxIters = 20;
        r = solve(s, o);
        Result rt = solve(t, o);
        auto swap = [](int c) { int s = c % 4; return c / 4 * 4 + (s == 1 ? 2 : s == 2 ? 1 : s); };
        for (int b = 1; b < 52; ++b)
            for (int a = 0; a < b; ++a) {
                int c = comboIndex(cardFromIndex(a), cardFromIndex(b));
                int d = comboIndex(cardFromIndex(swap(a)), cardFromIndex(swap(b)));
                for (int p = 0; p < 2; ++p) assert(std::abs(r.ev[p][c] - rt.ev[p][d]) < 1e-3);
            }
        assert(std::abs(r.exploitability - rt.exploitability) < 1e-4);
    }
    std::printf("postflop_test OK\n");
}
