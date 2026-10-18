// Runs MCCFR and writes the average strategy.
// Usage: solve [--seats 2|3|6] [--config file] [--iters N] [--chunk N] [--seed S] [--threads N] [--boards K]
//              [--checkpoint file] [--out file] [--show FFFFR]...
//   --seats      players left after forced folds at the root (2 = SB vs BB, 3 = BTN/SB/BB)
//   --checkpoint resumed if it exists, rewritten after every chunk
//   --warm       start from this checkpoint but never write it (read-only warm start, e.g. tune.py)
//   --seed       RNG seed; when resuming a checkpoint, an explicit --seed replaces its saved RNG state
//   --threads    worker threads (default 1 = bit-reproducible; more is faster but not reproducible)
//   --boards     boards per multiway flop equity estimate (SolverConfig::flopBoards, default 100)
//   --show       print the range at the node reached by these actions (F/C/R), after the last chunk;
//                widths are weighted by how often each class reaches the node, "." = never
//   --r key=val  realization factor (ip oop flush conn set dom low spr init pairLow oopCall callerShrink ipCall domPot, utility.h), repeatable
//   --report     print the validation.md sanity metrics
//   --reset-average  after resuming, restart the strategy average (regrets kept): warm start
//   --memory B   with --reset-average: old regrets fade as B / (B + discount blocks since); default 0.
//                With --reset-average, --iters counts the new iterations after the restart.
//   --lock dir   play the charts in dir (validation.md format) as fixed strategies and solve the rest
//                as a best response to them; the output keeps the chart probabilities. Not stored in
//                the checkpoint: pass it again when resuming. Prints the --top N (default 10) charted
//                spots where the chart gives up the most against the solve (locked-action regret).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "solver/cards.h"
#include "solver/mccfr.h"

using namespace pf;

struct Range {
    int node = -1;
    std::array<double, NBUCKETS> reach;  // how often the actor holds each class here (own earlier actions)
    double width[3] = {};               // reach- and combo-weighted share of fold / call / raise
};

static Range rangeAt(const Strategy& s, const std::vector<Act>& h) {
    Range r;
    if ((r.node = s.nodeFor(h)) < 0) return r;
    r.reach = s.reach(h);
    double total = 0;
    for (int b = 0; b < NBUCKETS; ++b) {
        total += comboCount(b) * r.reach[b];
        for (int a = 0; a < 3; ++a) r.width[a] += comboCount(b) * r.reach[b] * s.probs(r.node, b)[a];
    }
    for (double& x : r.width) x /= total;
    return r;
}

static const char* SEAT[] = {"UTG", "HJ", "CO", "BTN", "SB", "BB"};
static const char* ACT[] = {"fold", "call", "raise"};

static void show(const Strategy& s, const std::string& hist) {
    std::vector<Act> h;
    Range r;
    if (parseHistory(hist, h)) r = rangeAt(s, h);
    if (r.node < 0) return (void)std::printf("\n%s: no such node\n", hist.c_str());
    std::mt19937 rng(0);  // replay the history for the bet sizes
    Hand hand(rng, s.config());
    for (Act a : h) hand.apply(a);
    std::printf("\n%s (%s to act, facing %.1fbb): fold %.1f%%  call %.1f%%  raise %.1f%%", hist.c_str(),
                SEAT[s.actor(r.node)], hand.currentBet(), 100 * r.width[0], 100 * r.width[1], 100 * r.width[2]);
    if (s.tree().legal[r.node] >> int(Act::Raise) & 1) std::printf(" (to %.1fbb)", hand.raiseTo());
    std::printf("\n");
    for (int a : {2, 1}) {
        if (r.width[a] == 0) continue;
        std::printf("  %s %%:\n", a == 2 ? "raise" : "call");
        for (int row = 0; row < 13; ++row) {
            std::printf("   ");
            for (int c = 0; c < 13; ++c)
                if (r.reach[row * 13 + c] < 1e-3) std::printf("   .");  // class (almost) never gets here
                else std::printf(" %3.0f", 100 * s.probs(r.node, row * 13 + c)[a]);
            std::printf("   %c\n", "AKQJT98765432"[row]);
        }
    }
}

// Sanity metrics (validation.md): open % per seat, BB defence vs each open (others folding), the
// opener's response to a BB 3-bet, and whether AA/KK 4-bet there.
static void report(const Strategy& s, int rootFolds) {
    std::printf("\nsanity report (validation.md targets: UTG open mid-teens %%, BTN open in the 40s)\n");
    std::printf("  %-4s %6s | %-24s | %-24s %s\n", "seat", "open", "BB vs open: fold/call/3b", "opener vs BB 3b: f/c/4b", "AA,KK 4b");
    int aa = bucketFromName("AA"), kk = bucketFromName("KK");
    for (int p = rootFolds; p <= SB; ++p) {
        std::vector<Act> h(p, Act::Fold);
        Range open = rangeAt(s, h);
        h.push_back(Act::Raise);
        for (int q = p + 1; q < BB; ++q) h.push_back(Act::Fold);
        Range bb = rangeAt(s, h);
        h.push_back(Act::Raise);
        Range back = rangeAt(s, h);
        std::printf("  %-4s %5.1f%% | %5.1f%% %5.1f%% %5.1f%%    | %5.1f%% %5.1f%% %5.1f%%    %3.0f%% %3.0f%%\n", SEAT[p],
                    100 * open.width[2], 100 * bb.width[0], 100 * bb.width[1], 100 * bb.width[2], 100 * back.width[0],
                    100 * back.width[1], 100 * back.width[2], 100 * s.probs(back.node, aa)[2], 100 * s.probs(back.node, kk)[2]);
    }
    // Bluff check: per-hand raise/call % where reference charts 3-bet or 4-bet light (wheel aces,
    // suited connectors, K6s, KJo) and where we tend to 3-bet linearly (medium pairs, suited broadways).
    auto hands = [&](const char* label, const std::string& hist, std::initializer_list<const char*> names) {
        std::vector<Act> h;
        Range r;
        if (!parseHistory(hist, h) || (r = rangeAt(s, h)).node < 0 || s.actor(r.node) < rootFolds) return;
        std::printf("  %s: raise %.1f%% call %.1f%% |", label, 100 * r.width[2], 100 * r.width[1]);
        for (const char* n : names) {
            auto& p = s.probs(r.node, bucketFromName(n));
            std::printf(" %s %.0f/%.0f", n, 100 * p[2], 100 * p[1]);
        }
        std::printf("\n");
    };
    std::printf("  bluff check (hand raise%%/call%%):\n");
    hands("BTN vs CO open  ", "FFR", {"A5s", "A4s", "K6s", "76s", "65s", "77", "55", "KJs", "KJo", "AJs", "AQo"});
    hands("BB vs BTN open  ", "FFFRF", {"A5s", "A4s", "K6s", "76s", "87s", "77", "55", "KJs", "KJo", "ATs", "QJo"});
    hands("CO vs BTN 3-bet ", "FFRRFF", {"A5s", "A4s", "A3s", "K5s", "AQo", "KJs", "JJ", "TT"});
    hands("BTN vs BB 3-bet ", "FFFRFR", {"A5s", "A4s", "A3s", "A9s", "K5s", "AQo", "JJ", "TT"});
}

// Expected strategy change between a and b: each node's change (share of the actor's range, by
// combos and own reach, whose action moved) weighted by how often b reaches the node. Ranges are
// carried down the tree per seat, ignoring card removal between seats. Forced-fold nodes are skipped.
struct ChangeWalk {
    const Strategy &a, &b;
    int rootFolds;
    std::array<std::array<double, NBUCKETS>, NPOS> range;
    double num = 0, den = 0;

    void walk(int node, double reach) {
        int p = b.actor(node);
        auto& r = range[p];
        double tot = 0, d = 0;
        for (int k = 0; k < NBUCKETS; ++k) {
            tot += comboCount(k) * r[k];
            for (int x = 0; x < 3; ++x) d += comboCount(k) * r[k] * std::fabs(a.probs(node, k)[x] - b.probs(node, k)[x]) / 2;
        }
        if (tot <= 0) return;
        if (p >= rootFolds) num += reach * d / tot, den += reach;
        for (int x = 0; x < 3; ++x) {
            int c = b.child(node, Act(x));
            if (c < 0) continue;
            double px = 0;
            for (int k = 0; k < NBUCKETS; ++k) px += comboCount(k) * r[k] * b.probs(node, k)[x];
            if (px <= 0) continue;
            auto saved = r;
            for (int k = 0; k < NBUCKETS; ++k) r[k] *= b.probs(node, k)[x];
            walk(c, reach * px / tot);
            r = saved;
        }
    }
};

static double strategyChange(const Strategy& a, const Strategy& b, int rootFolds) {
    ChangeWalk w{a, b, rootFolds, {}};
    for (auto& r : w.range) r.fill(1);
    w.walk(b.root(), 1);
    return w.den > 0 ? w.num / w.den : 0;
}

// Charted spots ranked by how much the locked play gives up: per class, the best action's mean
// regret against the chart's mix (bb per hand dealt, so frequent spots and classes weigh more),
// summed over the spot's locked classes.
static void lockReport(const Solver& s, const std::vector<uint8_t>& locked, int top) {
    const Tree& t = s.tree();
    std::vector<std::string> hist(t.size());
    for (int n = 0; n < t.size(); ++n)
        for (int a = 0; a < 3; ++a)
            if (t.next[n][a] > 0) hist[t.next[n][a]] = hist[n] + "FCR"[a];
    struct Spot { double gain; int node; std::vector<std::pair<double, int>> classes; };  // (gain, bucket * 3 + action)
    std::vector<Spot> spots;
    for (int n = 0; n < t.size(); ++n) {
        Spot sp{0, n, {}};
        for (int b = 0; b < NBUCKETS; ++b) {
            if (!locked[size_t(n) * NBUCKETS + b]) continue;
            auto r = s.averageRegret(n, b);
            int best = int(std::max_element(r.begin(), r.end()) - r.begin());
            if (r[best] > 0) sp.gain += r[best], sp.classes.push_back({r[best], b * 3 + best});
        }
        if (!sp.classes.empty()) spots.push_back(std::move(sp));
    }
    std::sort(spots.begin(), spots.end(), [](auto& x, auto& y) { return x.gain > y.gain; });
    std::printf("\ncharted spots where the chart gives up most (mbb per hand dealt; class: better action +mbb)\n");
    for (int i = 0; i < std::min(top, int(spots.size())); ++i) {
        auto& sp = spots[i];
        std::sort(sp.classes.rbegin(), sp.classes.rend());
        std::printf("  %-10s %-3s %7.2f |", sp.node ? hist[sp.node].c_str() : "(root)", SEAT[t.actor[sp.node]], 1000 * sp.gain);
        for (int k = 0; k < std::min(5, int(sp.classes.size())); ++k)
            std::printf(" %s %s +%.2f", bucketName(sp.classes[k].second / 3).c_str(),
                        ACT[sp.classes[k].second % 3], 1000 * sp.classes[k].first);
        std::printf("\n");
    }
}

int main(int argc, char** argv) {
    long memory = 0;
    int seats = 6, threads = 1, boards = SolverConfig{}.flopBoards;
    long iters = 1000000, chunk = 0;
    uint32_t seed = 1;
    std::string cfgPath, ckPath, warmPath, lockDir, outPath = "strategy.bin", table = DATA_DIR "/hu_equity.bin";
    int top = 10;
    std::vector<std::string> shows;
    bool doReport = false, resetAvg = false, seedGiven = false;
    RealizationConfig rc;
    const std::pair<const char*, double*> rFields[] = {
        {"ip", &rc.ip}, {"oop", &rc.oop}, {"flush", &rc.flush}, {"conn", &rc.conn},
        {"set", &rc.set}, {"dom", &rc.dom}, {"low", &rc.low},
        {"spr", &rc.spr}, {"init", &rc.init}, {"pairLow", &rc.pairLow},
        {"oopCall", &rc.oopCall}, {"callerShrink", &rc.callerShrink}, {"ipCall", &rc.ipCall}, {"domPot", &rc.domPot}};
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--report" || a == "--reset-average") {
            (a == "--report" ? doReport : resetAvg) = true;
            continue;
        }
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (!v) return std::fprintf(stderr, "missing value for %s\n", a.c_str()), 2;
        ++i;
        if (a == "--seats") seats = std::atoi(v);
        else if (a == "--config") cfgPath = v;
        else if (a == "--iters") iters = std::atol(v);
        else if (a == "--chunk") chunk = std::atol(v);
        else if (a == "--seed") seed = uint32_t(std::atol(v)), seedGiven = true;
        else if (a == "--threads") threads = std::max(1, std::atoi(v));
        else if (a == "--boards") boards = std::atoi(v);
        else if (a == "--checkpoint") ckPath = v;
        else if (a == "--warm") warmPath = v;
        else if (a == "--memory") memory = std::atol(v);
        else if (a == "--out") outPath = v;
        else if (a == "--lock") lockDir = v;
        else if (a == "--top") top = std::atoi(v);
        else if (a == "--show") shows.push_back(v);
        else if (a == "--r") {
            std::string kv = v;
            auto eqPos = kv.find('=');
            auto f = std::find_if(std::begin(rFields), std::end(rFields),
                                  [&](auto& x) { return kv.substr(0, eqPos) == x.first; });
            if (eqPos == std::string::npos || f == std::end(rFields))
                return std::fprintf(stderr, "bad --r %s (keys: ip oop flush conn set dom low spr init pairLow oopCall callerShrink ipCall domPot)\n", v), 2;
            *f->second = std::atof(v + eqPos + 1);
        }
        else return std::fprintf(stderr, "unknown option %s\n", a.c_str()), 2;
    }
    if (seats < 2 || seats > 6 || iters <= 0 || boards < 1)
        return std::fprintf(stderr, "bad --seats, --iters or --boards\n"), 2;
    if (chunk <= 0) chunk = std::max(1L, iters / 10);

    Config cfg = cfgPath.empty() ? Config{} : loadConfig(cfgPath);
    SolverConfig sc;
    sc.rootFolds = NPOS - seats;
    sc.realization = rc;
    sc.flopBoards = boards;
    Equity eq;
    if (!eq.loadHuTable(table)) std::printf("warning: no %s, heads-up equity will be sampled\n", table.c_str());
    Solver solver(cfg, sc, eq, seed);
    std::vector<uint8_t> locked;
    if (!lockDir.empty()) {
        Strategy chart(cfg, rc);
        std::string err;
        int n = applyCharts(chart, lockDir, &locked, &err);
        if (n < 0) return std::fprintf(stderr, "cannot lock %s: %s\n", lockDir.c_str(), err.c_str()), 2;
        solver.lock(chart, locked);
        std::printf("locked %d charts, %ld (spot, class) entries\n", n, long(std::count(locked.begin(), locked.end(), 1)));
    }
    if (!ckPath.empty() && std::ifstream(ckPath).good()) {  // an existing checkpoint must load
        if (std::string err; !solver.loadCheckpoint(ckPath, &err))
            return std::fprintf(stderr, "cannot resume %s: %s\n", ckPath.c_str(), err.c_str()), 1;
        std::printf("resumed %s at iteration %ld\n", ckPath.c_str(), solver.iterations());
    }
    if (std::string err; !warmPath.empty() && !solver.loadCheckpoint(warmPath, &err))
        return std::fprintf(stderr, "cannot warm-start from %s: %s\n", warmPath.c_str(), err.c_str()), 1;
    if (resetAvg) solver.resetAverage(memory), iters += solver.iterations();  // --iters counts new iterations
    if (seedGiven) solver.reseed(seed);  // an explicit --seed overrides the checkpoint's RNG state

    Strategy prev = solver.average();
    while (solver.iterations() < iters) {
        auto t0 = std::chrono::steady_clock::now();
        long n = std::min(chunk, iters - solver.iterations());
        solver.run(n, threads);
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        Strategy cur = solver.average();
        std::printf("iteration %ld: %.0f it/s, strategy change %.3f%%\n", solver.iterations(), n / sec,
                    100 * strategyChange(prev, cur, sc.rootFolds));
        std::fflush(stdout);
        prev = cur;
        if (!ckPath.empty() && !solver.saveCheckpoint(ckPath))
            return std::fprintf(stderr, "cannot write %s\n", ckPath.c_str()), 1;
    }
    if (!prev.save(outPath)) return std::fprintf(stderr, "cannot write %s\n", outPath.c_str()), 1;
    std::printf("wrote %s\n", outPath.c_str());
    for (auto& h : shows) show(prev, h);
    if (doReport) report(prev, sc.rootFolds);
    if (!locked.empty()) lockReport(solver, locked, top);
}
