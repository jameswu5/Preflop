#pragma once
// External-sampling MCCFR over the preflop tree (docs/solver.md), single-threaded and deterministic.
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

    void run(long iterations);
    long iterations() const { return t_; }
    const Tree& tree() const { return tree_; }

    // Normalized strategy sums. Forced-fold nodes fold; nodes never reached stay uniform.
    Strategy average() const;

    // Full solver state (tables, iteration, RNG), so resuming continues exactly where it stopped.
    // load fails (state unchanged) if the file is malformed or was made with a different Config /
    // SolverConfig. A different RealizationConfig is allowed: warm-starting a tuning run.
    bool saveCheckpoint(const std::string& path) const;
    bool loadCheckpoint(const std::string& path);

private:
    double traverse(const Hand& h, int node, int traverser);
    std::array<float, 3> regretMatch(int node, int bucket) const;

    Config cfg_;
    SolverConfig sc_;
    const Equity& eq_;
    Tree tree_;
    std::mt19937 rng_;
    long t_ = 0;
    std::vector<std::array<float, 3>> regret_, stratSum_;  // [node * 169 + bucket]
};

}  // namespace pf
