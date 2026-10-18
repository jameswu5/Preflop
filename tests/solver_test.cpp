#undef NDEBUG  // the build defaults to Release; keep asserts on
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "solver/cards.h"
#include "solver/mccfr.h"

using namespace pf;
const Act F = Act::Fold, C = Act::Call, R = Act::Raise;

static std::vector<std::array<Card, 2>> combos() {  // by comboIndex
    std::vector<std::array<Card, 2>> c(NCOMBOS);
    for (int i = 0; i < 52; ++i)
        for (int j = i + 1; j < 52; ++j) c[comboIndex(cardFromIndex(i), cardFromIndex(j))] = {cardFromIndex(i), cardFromIndex(j)};
    return c;
}
static bool disjoint(const std::array<Card, 2>& x, const std::array<Card, 2>& y) {
    for (Card a : x)
        for (Card b : y)
            if (cardIndex(a) == cardIndex(b)) return false;
    return true;
}

// Exact exploitability (bb/hand) of a heads-up push/fold strategy at stack S: the average of what
// a best response gains as SB and as BB. 0 at Nash. Uses the exact HU table, as the solver does.
static double pushFoldExploitability(const Strategy& s, const std::vector<float>& hu, double S) {
    int sbNode = s.nodeFor({F, F, F, F}), bbNode = s.child(sbNode, R);
    auto combo = combos();
    std::vector<double> push(NCOMBOS), call(NCOMBOS);
    for (int c = 0; c < NCOMBOS; ++c) {
        int b = bucket(combo[c][0], combo[c][1]);
        push[c] = s.probs(sbNode, b)[int(R)], call[c] = s.probs(bbNode, b)[int(C)];
    }
    double brSB = 0, brBB = 0;
    for (int x = 0; x < NCOMBOS; ++x) {  // x = SB combo for brSB, BB combo for brBB
        double pushEV = 0, sbFoldGain = 0, callEV = 0, foldEV = 0;
        for (int y = 0; y < NCOMBOS; ++y) {
            if (!disjoint(combo[x], combo[y])) continue;
            double e = hu[size_t(x) * NCOMBOS + y];  // equity of x vs y
            pushEV += call[y] * (e * 2 * S - S) + (1 - call[y]) * 1;  // SB x pushes into BB y
            sbFoldGain += (1 - push[y]) * 0.5;                        // SB y folds to BB x
            callEV += push[y] * (e * 2 * S - S);
            foldEV += push[y] * -1;
        }
        brSB += std::max(pushEV, -0.5 * 1225) / 1225;
        brBB += (sbFoldGain + std::max(callEV, foldEV)) / 1225;
    }
    return (brSB + brBB) / 2 / NCOMBOS;
}

int main() {
    {  // tree matches the engine along random action sequences
        Tree t;
        std::mt19937 rng(3);
        for (int trial = 0; trial < 2000; ++trial) {
            Hand h(rng);
            int n = 0;
            while (!h.done()) {
                assert(n >= 0 && t.actor[n] == h.toAct());
                uint8_t mask = 0;
                for (Act a : h.legal()) mask |= 1 << int(a);
                assert(t.legal[n] == mask);
                auto l = h.legal();
                Act a = l[rng() % l.size()];
                h.apply(a);
                n = t.next[n][int(a)];
                assert((n < 0) == h.done());
            }
        }
    }
    {  // strategy: uniform default, save/load round trip, rejects bad files
        Strategy s;
        int n = s.nodeFor({F, F, R});  // BTN facing a CO open
        assert(s.actor(n) == BTN && s.probs(n, 0)[0] == 1.0f / 3);
        assert(s.probs(s.nodeFor({F, F, F, F}), 0)[int(C)] == 0);  // SB can't complete
        assert(s.nodeFor({C}) == -1);                                // no limping
        RealizationConfig rc;
        rc.oop += 0.1;  // not the default, so the load must really restore it
        Strategy s2(Config{}, rc);
        s2.probs(n, 0) = {0, 0, 1};
        assert(s2.save("solver_test.strat"));
        Strategy t;
        assert(t.load("solver_test.strat") && t.probs(n, 0)[int(R)] == 1 && t.realization().oop == rc.oop);
        std::ofstream("solver_test.strat", std::ios::binary) << "junk";
        assert(!t.load("solver_test.strat") && t.probs(n, 0)[int(R)] == 1);  // unchanged on failure
        std::remove("solver_test.strat");
    }

    Equity eq;
    std::vector<float> hu(size_t(NCOMBOS) * NCOMBOS);
    if (!eq.loadHuTable(DATA_DIR "/hu_equity.bin")) {
        std::puts("solver_test: no data/hu_equity.bin, skipping solver checks (run gen_equity)");
        return 0;
    }
    std::ifstream(DATA_DIR "/hu_equity.bin", std::ios::binary).read(reinterpret_cast<char*>(hu.data()), hu.size() * 4);

    Config pf10;  // heads-up 10bb push/fold: every open is all-in
    pf10.stack = pf10.open = pf10.sbOpen = 10;
    pf10.rake = 0;  // the exact exploitability formula below is rake-free
    SolverConfig sc;
    sc.rootFolds = 4;
    {  // deterministic for a fixed seed
        Solver a(pf10, sc, eq, 9), b(pf10, sc, eq, 9);
        a.run(20000), b.run(20000);
        Strategy sa = a.average(), sb = b.average();
        for (int n = 0; n < sa.tree().size(); ++n)
            for (int k = 0; k < NBUCKETS; ++k) assert(sa.probs(n, k) == sb.probs(n, k));
    }
    {  // checkpoint: save, resume in a fresh solver, continue == one uninterrupted run
        Solver a(pf10, sc, eq, 5), b(pf10, sc, eq, 5);
        a.run(15000);
        assert(a.saveCheckpoint("solver_test.ck"));
        Solver c(pf10, sc, eq, 123);  // different seed: the RNG state comes from the file
        assert(c.loadCheckpoint("solver_test.ck") && c.iterations() == 15000);
        a.run(15000), c.run(15000), b.run(30000);
        Strategy sa = a.average(), sb = b.average(), scs = c.average();
        for (int n = 0; n < sa.tree().size(); ++n)
            for (int k = 0; k < NBUCKETS; ++k) assert(sa.probs(n, k) == sb.probs(n, k) && scs.probs(n, k) == sb.probs(n, k));
        SolverConfig warm = sc;
        warm.realization.oop += 0.1;
        Solver w(pf10, warm, eq, 5);
        assert(w.loadCheckpoint("solver_test.ck") && w.iterations() == 0);  // only R differs: warm start, fresh average
        w.resetAverage(1);  // keep one discount block of memory: count restarts at discountEvery
        assert(w.iterations() == warm.discountEvery);
        SolverConfig boards = sc;
        boards.flopBoards = 50;
        Solver k(pf10, boards, eq, 5);
        assert(k.loadCheckpoint("solver_test.ck") && k.iterations() == 15000);  // board count is precision only
        Config other = pf10;
        other.stack = other.open = other.sbOpen = 12;
        Solver d(other, sc, eq, 5);
        assert(!d.loadCheckpoint("solver_test.ck") && d.iterations() == 0);  // different game: refused
        std::ofstream("solver_test.ck", std::ios::binary) << "junk";
        assert(!c.loadCheckpoint("solver_test.ck") && c.iterations() == 30000);
        std::remove("solver_test.ck");
    }
    if (std::ifstream(DATA_DIR "/strategy_6max.bin").good()) {  // older strategy files load, new R fields default
        Strategy s;
        assert(s.load(DATA_DIR "/strategy_6max.bin") && s.realization().ipCall == 1 && s.realization().domPot == 0);
        assert(s.save("solver_test.strat"));
        Strategy t;
        assert(t.load("solver_test.strat") && t.probs(5, 7) == s.probs(5, 7) &&
               std::memcmp(&t.realization(), &s.realization(), sizeof(RealizationConfig)) == 0);
        std::remove("solver_test.strat");
    }
    if (Config ck; std::ifstream(DATA_DIR "/6max.ck", std::ios::binary).seekg(8).read(reinterpret_cast<char*>(&ck), sizeof ck)) {
        // older checkpoint versions still load, new R fields default; under the game they were made for
        // (the raw Config after "PFCK" + version), which may predate the current default sizes
        std::string err;
        Config sizes = ck;
        sizes.threeBetIP += 0.5;
        assert(!Solver(sizes, SolverConfig{}, eq, 1).loadCheckpoint(DATA_DIR "/6max.ck", &err) && err.find("Config") != std::string::npos);
        Solver a(ck, SolverConfig{}, eq, 1), b(ck, SolverConfig{}, eq, 1);
        SolverConfig other;
        other.realization.ipCall = 0.9;
        Solver c(ck, other, eq, 1);
        assert(c.loadCheckpoint(DATA_DIR "/6max.ck") && c.iterations() == 0);  // R differs only in ipCall: warm start
        assert(a.loadCheckpoint(DATA_DIR "/6max.ck") && a.saveCheckpoint("solver_test.ck"));
        assert(b.loadCheckpoint("solver_test.ck") && b.iterations() == a.iterations());
        a.run(2000), b.run(2000);
        Strategy sa = a.average(), sb = b.average();
        for (int n = 0; n < sa.tree().size(); ++n)
            for (int k = 0; k < NBUCKETS; ++k) assert(sa.probs(n, k) == sb.probs(n, k));
        std::remove("solver_test.ck");
    }
    {  // multithreaded: not bit-reproducible, but converges just as well
        Solver s(pf10, sc, eq, 2);
        s.run(1500000, 4);
        assert(s.iterations() == 1500000);
        double expl = pushFoldExploitability(s.average(), hu, 10);
        std::printf("  push/fold 10bb, 4 threads: exploitability %.4f bb/hand\n", expl);
        assert(expl < 0.003);
    }
    {  // chart lock: SB shoves any two; that stays fixed, and BB best-responds to it
        Strategy chart(pf10);
        int sbNode = chart.nodeFor({F, F, F, F}), bbNode = chart.child(sbNode, R);
        std::vector<uint8_t> mask(size_t(chart.tree().size()) * NBUCKETS);
        for (int b = 0; b < NBUCKETS; ++b) chart.probs(sbNode, b) = {0, 0, 1}, mask[size_t(sbNode) * NBUCKETS + b] = 1;
        Solver s(pf10, sc, eq, 4);
        s.lock(chart, mask);
        s.run(400000);
        Strategy avg = s.average();
        auto combo = combos();
        std::vector<double> eqSum(NBUCKETS), n(NBUCKETS);  // BB's mean equity vs any two, per class
        for (int x = 0; x < NCOMBOS; ++x)
            for (int y = 0; y < NCOMBOS; ++y)
                if (disjoint(combo[x], combo[y])) {
                    int b = bucket(combo[x][0], combo[x][1]);
                    eqSum[b] += hu[size_t(x) * NCOMBOS + y], ++n[b];
                }
        int decisive = 0;
        for (int b = 0; b < NBUCKETS; ++b) {
            assert(avg.probs(sbNode, b) == (std::array<float, 3>{0, 0, 1}));
            double e = eqSum[b] / n[b], call = avg.probs(bbNode, b)[int(C)];  // calling 9 to win 20: needs 45%
            if (std::fabs(e - 0.45) > 0.03) assert(e > 0.45 ? call > 0.9 : call < 0.1), ++decisive;
        }
        assert(decisive > 100);

        Strategy aaFolds(pf10);  // lock only AA, to fold: the regret report flags it
        int aa = bucketFromName("AA"), kk = bucketFromName("KK");
        std::fill(mask.begin(), mask.end(), 0);
        aaFolds.probs(sbNode, aa) = {1, 0, 0}, mask[size_t(sbNode) * NBUCKETS + aa] = 1;
        Solver t(pf10, sc, eq, 4);
        t.lock(aaFolds, mask);
        t.run(200000);
        auto ra = t.averageRegret(sbNode, aa), rk = t.averageRegret(sbNode, kk);  // AA shove: ~3.8bb x 6/1326
        std::printf("  locked AA fold: shove regret %.4f bb/hand (KK, solved: %.4f)\n", ra[int(R)], std::max(rk[0], rk[2]));
        assert(t.average().probs(sbNode, aa)[int(F)] == 1 && t.average().probs(sbNode, kk)[int(R)] > 0.99);
        assert(ra[int(R)] > 0.01 && ra[int(R)] > 20 * std::max({rk[0], rk[2], 0.0}));
    }
    {  // converges toward Nash
        Solver s(pf10, sc, eq, 1);
        double uniform = pushFoldExploitability(s.average(), hu, 10);
        long done = 0;
        double expl = 0;
        for (long n : {100000L, 400000L, 1500000L}) {
            s.run(n - done), done = n;
            expl = pushFoldExploitability(s.average(), hu, 10);
            std::printf("  push/fold 10bb: %ld iterations, exploitability %.4f bb/hand (uniform %.3f)\n", n, expl, uniform);
        }
        assert(uniform > 0.4 && expl < 0.003);
        Strategy avg = s.average();
        int sbNode = avg.nodeFor({F, F, F, F}), bbNode = avg.child(sbNode, R);
        double pushPct = 0, callPct = 0;  // combo-weighted range widths
        for (int b = 0; b < NBUCKETS; ++b) {
            int w = b / 13 == b % 13 ? 6 : b / 13 < b % 13 ? 4 : 12;
            pushPct += w * avg.probs(sbNode, b)[int(R)] / NCOMBOS, callPct += w * avg.probs(bbNode, b)[int(C)] / NCOMBOS;
        }
        std::printf("  SB pushes %.1f%%, BB calls %.1f%%\n", 100 * pushPct, 100 * callPct);
        assert(avg.probs(sbNode, bucketFromName("AA"))[int(R)] > 0.99);
        assert(avg.probs(bbNode, bucketFromName("AA"))[int(C)] > 0.99);
        assert(avg.probs(bbNode, bucketFromName("72o"))[int(C)] < 0.01);
        assert(avg.probs(avg.root(), 0)[int(F)] == 1);  // forced fold
    }
    std::puts("solver_test: ok");
    return 0;
}
