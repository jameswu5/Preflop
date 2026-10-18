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
    long discountEvery = 10000;   // linear weighting: iterations in the k-th block of N weigh k (docs/solver.md)
    RealizationConfig realization;
};

class Solver {
public:
    // `eq` must outlive the solver; load its HU table first for exact heads-up equity.
    Solver(const Config& cfg, const SolverConfig& sc, const Equity& eq, uint32_t seed);

    // threads > 1 splits the iterations over threads seeded from the solver's RNG. Faster, but
    // results vary run to run.
    void run(long iterations, int threads = 1);
    long iterations() const { return t_; }
    const Tree& tree() const { return tree_; }

    // Normalized strategy sums. Forced-fold nodes fold, locked entries keep their fixed
    // probabilities; nodes never reached stay uniform.
    Strategy average() const;

    // Chart-locked solving: at every (node, bucket) with mask[node * NBUCKETS + bucket] set, play
    // s.probs(node, bucket) (s must share the solver's tree, e.g. from applyCharts) and never update
    // the strategy sum. Regrets there are still accumulated, but only as a diagnostic: they are
    // never played, and averageRegret() reads them. Not stored in checkpoints; lock again on resume.
    void lock(const Strategy& s, const std::vector<uint8_t>& mask);
    // Mean regret of each action at (node, bucket), in bb per hand dealt, linearly weighted like the
    // tables: what always switching to it there would have gained, counting how often the cards and
    // the others bring play there. At a locked entry, max(0, max_a) is how far the fixed play is
    // from a best response.
    std::array<double, 3> averageRegret(int node, int bucket) const;

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
    bool loadCheckpoint(const std::string& path, std::string* err = nullptr);  // err: why it failed

private:
    // Equity depends only on the hole cards and who is still in, so within one iteration it is
    // computed once per live set (and all-in vs flop) and reused by every terminal that shares it.
    struct EquityCache {
        std::array<std::array<double, NPOS>, 64> eq[2];  // [allIn][live mask]
        uint64_t valid[2] = {0, 0};
    };
    // Per-iteration state: the deal, each seat's hand class, and the equity cache.
    struct Deal {
        Holes cards;
        int bucket[NPOS];
        EquityCache cache;
        float weight;  // of this iteration's increments
    };
    // Linear CFR without rescaling the tables: iteration t adds with weight k = its block number, so
    // the tables hold scale(t_) = (blocks done + 1) times the discounted sums. Both regret matching
    // and the average are scale-free; checkpoints store the tables as they are.
    float weight(long t) const { return sc_.discountEvery > 0 ? float((t - 1) / sc_.discountEvery + 1) : 1; }
    float scale(long t) const { return weight(t + 1); }
    void rescale(float factor);
    double traverse(Deal& d, int node, int traverser, std::mt19937& rng, bool shared);
    double terminal(Deal& d, int edge, int traverser, std::mt19937& rng);
    std::array<float, 3> regretMatch(int node, int bucket, bool shared) const;
    void iterate(long t, std::mt19937& rng, bool shared);

    Config cfg_;
    SolverConfig sc_;
    const Equity& eq_;
    Tree tree_;
    std::mt19937 rng_;
    long t_ = 0;
    long loadedT_ = 0;  // iteration count of the last loaded checkpoint, for resetAverage's memory
    // [node * 169 + bucket], scaled by scale(t_): see weight().
    std::vector<std::array<float, 3>> regret_, stratSum_;
    std::vector<Terminal> terminal_;  // [node * 3 + action] where that action ends the hand
    std::vector<uint8_t> locked_;                 // [node * 169 + bucket]; empty = nothing locked
    std::vector<std::array<float, 3>> fixed_;     // the locked probabilities, same indexing
};

}  // namespace pf
