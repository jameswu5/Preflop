// Runs MCCFR and writes the average strategy.
// Usage: solve [--seats 2|3|6] [--config file] [--iters N] [--chunk N] [--seed S]
//              [--checkpoint file] [--out file] [--show FFFFR]...
//   --seats      players left after forced folds at the root (2 = SB vs BB, 3 = BTN/SB/BB)
//   --checkpoint resumed if it exists, rewritten after every chunk
//   --show       print the range at the node reached by these actions (F/C/R), after the last chunk;
//                widths are weighted by how often each class reaches the node, "." = never
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "solver/cards.h"
#include "solver/mccfr.h"

using namespace pf;

static int weight(int b) { return b / 13 == b % 13 ? 6 : b / 13 < b % 13 ? 4 : 12; }

static void show(const Strategy& s, const std::string& hist) {
    std::vector<Act> h;
    for (char c : hist) h.push_back(c == 'F' ? Act::Fold : c == 'C' ? Act::Call : Act::Raise);
    int n = s.nodeFor(h);
    if (n < 0) return (void)std::printf("\n%s: no such node\n", hist.c_str());
    const char* seat[] = {"UTG", "HJ", "CO", "BTN", "SB", "BB"};
    // reach[b]: how often the actor holds class b here, from its own earlier actions on this path.
    std::array<double, NBUCKETS> reach;
    reach.fill(1);
    for (int i = 0, m = s.root(); i < int(h.size()); m = s.child(m, h[i++]))
        if (s.actor(m) == s.actor(n))
            for (int b = 0; b < NBUCKETS; ++b) reach[b] *= s.probs(m, b)[int(h[i])];
    double w[3] = {}, total = 0;
    for (int b = 0; b < NBUCKETS; ++b) {
        total += weight(b) * reach[b];
        for (int a = 0; a < 3; ++a) w[a] += weight(b) * reach[b] * s.probs(n, b)[a];
    }
    for (double& x : w) x /= total;
    std::mt19937 rng(0);  // replay the history for the raise size
    Hand hand(rng, s.config());
    for (Act a : h) hand.apply(a);
    std::printf("\n%s (%s to act, facing %.1fbb): fold %.1f%%  call %.1f%%  raise %.1f%%", hist.c_str(),
                seat[s.actor(n)], hand.currentBet(), 100 * w[0], 100 * w[1], 100 * w[2]);
    if (s.tree().legal[n] >> int(Act::Raise) & 1) std::printf(" (to %.1fbb)", hand.raiseTo());
    std::printf("\n");
    for (int a : {2, 1}) {
        if (w[a] == 0) continue;
        std::printf("  %s %%:\n", a == 2 ? "raise" : "call");
        for (int r = 0; r < 13; ++r) {
            std::printf("   ");
            for (int c = 0; c < 13; ++c)
                if (reach[r * 13 + c] < 1e-3) std::printf("   .");  // class (almost) never gets here
                else std::printf(" %3.0f", 100 * s.probs(n, r * 13 + c)[a]);
            std::printf("   %c\n", "AKQJT98765432"[r]);
        }
    }
}

// Largest share of any node's range (combo-weighted) whose action changed between a and b.
static double maxDelta(const Strategy& a, const Strategy& b) {
    double worst = 0;
    for (int n = 0; n < a.tree().size(); ++n) {
        double d = 0;
        for (int k = 0; k < NBUCKETS; ++k)
            for (int x = 0; x < 3; ++x) d += weight(k) * std::fabs(a.probs(n, k)[x] - b.probs(n, k)[x]);
        worst = std::max(worst, d / (2 * NCOMBOS));
    }
    return worst;
}

int main(int argc, char** argv) {
    int seats = 6;
    long iters = 1000000, chunk = 0;
    uint32_t seed = 1;
    std::string cfgPath, ckPath, outPath = "strategy.bin", table = DATA_DIR "/hu_equity.bin";
    std::vector<std::string> shows;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (!v) return std::fprintf(stderr, "missing value for %s\n", a.c_str()), 2;
        ++i;
        if (a == "--seats") seats = std::atoi(v);
        else if (a == "--config") cfgPath = v;
        else if (a == "--iters") iters = std::atol(v);
        else if (a == "--chunk") chunk = std::atol(v);
        else if (a == "--seed") seed = uint32_t(std::atol(v));
        else if (a == "--checkpoint") ckPath = v;
        else if (a == "--out") outPath = v;
        else if (a == "--show") shows.push_back(v);
        else return std::fprintf(stderr, "unknown option %s\n", a.c_str()), 2;
    }
    if (seats < 2 || seats > 6 || iters <= 0) return std::fprintf(stderr, "bad --seats or --iters\n"), 2;
    if (chunk <= 0) chunk = std::max(1L, iters / 10);

    Config cfg = cfgPath.empty() ? Config{} : loadConfig(cfgPath);
    SolverConfig sc;
    sc.rootFolds = NPOS - seats;
    Equity eq;
    if (!eq.loadHuTable(table)) std::printf("warning: no %s, heads-up equity will be sampled\n", table.c_str());
    Solver solver(cfg, sc, eq, seed);
    if (!ckPath.empty() && solver.loadCheckpoint(ckPath))
        std::printf("resumed %s at iteration %ld\n", ckPath.c_str(), solver.iterations());

    Strategy prev = solver.average();
    while (solver.iterations() < iters) {
        auto t0 = std::chrono::steady_clock::now();
        long n = std::min(chunk, iters - solver.iterations());
        solver.run(n);
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        Strategy cur = solver.average();
        std::printf("iteration %ld: %.0f it/s, max range change %.2f%%\n", solver.iterations(), n / sec,
                    100 * maxDelta(prev, cur));
        std::fflush(stdout);
        prev = cur;
        if (!ckPath.empty() && !solver.saveCheckpoint(ckPath))
            return std::fprintf(stderr, "cannot write %s\n", ckPath.c_str()), 1;
    }
    if (!prev.save(outPath)) return std::fprintf(stderr, "cannot write %s\n", outPath.c_str()), 1;
    std::printf("wrote %s\n", outPath.c_str());
    for (auto& h : shows) show(prev, h);
}
