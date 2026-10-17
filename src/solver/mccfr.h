#pragma once
// External-sampling MCCFR over the preflop tree (docs/solver.md). Deterministic for a fixed seed
// with one thread; with more, threads share the tables Hogwild-style (relaxed atomics).
#include <random>
#include <string>
#include <vector>

#include "solver/equity.h"
#include "solver/strategy.h"
#include "solver/utility.h"

namespace pf {

struct SolverConfig {
    int rootFolds = 0;            // seats UTG.. forced to fold at the root: 4 = SB vs BB, 3 = BTN/SB/BB
    int flopBoards = 100;         // boards per multiway flop-terminal equity estimate
    long discountEvery = 10000;   // linear weighting: every N iterations scale regrets and strategy sums by k/(k+1)
    RealizationConfig realization;
};

class Solver {
public:
    // `eq` must outlive the solver; load its HU table first for exact heads-up equity.
    Solver(const Config& cfg, const SolverConfig& sc, const Equity& eq, uint32_t seed);

    // threads > 1 runs iterations in parallel blocks that end on discount boundaries; each block's
    // threads are seeded from the solver's RNG. Faster, but results vary run to run.
    void run(long iterations, int threads = 1);
    long iterations() const { return t_; }
    const Tree& tree() const { return tree_; }

    // Normalized strategy sums. Forced-fold nodes fold; nodes never reached stay uniform.
    Strategy average() const;

    // Full solver state (tables, iteration, RNG), so resuming continues exactly where it stopped.
    // load fails (state unchanged) if the file is malformed or was made with a different Config /
    // SolverConfig (flopBoards may differ: it is estimation precision, not the game). A different
    // RealizationConfig is allowed for warm-starting a tuning run: the
    // regrets are kept, but the strategy sums and iteration count restart, so the average reflects
    // only the new R.
    bool saveCheckpoint(const std::string& path) const;
    // Forget the strategy sums; regrets are kept (warm start). The iteration count restarts at
    // `memoryBlocks` discount blocks, so after K more blocks the old regrets keep weight
    // memoryBlocks / (memoryBlocks + K): 0 forgets them fastest, larger values keep them longer.
    void resetAverage(long memoryBlocks = 0);
    void reseed(uint32_t seed) { rng_.seed(seed); }  // e.g. independent runs from one checkpoint
    bool loadCheckpoint(const std::string& path);

private:
    double traverse(const Hand& h, int node, int traverser, std::mt19937& rng, bool shared);
    std::array<float, 3> regretMatch(int node, int bucket, bool shared) const;
    void iterate(long t, std::mt19937& rng, bool shared);
    void discount(int threads);

    Config cfg_;
    SolverConfig sc_;
    const Equity& eq_;
    Tree tree_;
    std::mt19937 rng_;
    long t_ = 0;
    long loadedT_ = 0;  // iteration count of the last loaded checkpoint, for resetAverage's memory
    std::vector<std::array<float, 3>> regret_, stratSum_;  // [node * 169 + bucket]
};

}  // namespace pf
