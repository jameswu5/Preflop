#pragma once
// Solved (or dummy) preflop strategy: the betting tree plus action probabilities per
// (node, hand bucket). Written by the solver, read by the trainer bots and the GUI.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "solver/cards.h"
#include "solver/utility.h"
#include "trainer/game.h"

namespace pf {

// Every betting history reachable from the start of a hand (UTG to act), built by walking Hand.
// Node 0 is the root. Terminal states are not nodes: child() returns -1 for them.
// "FFR" -> {Fold, Fold, Raise}; false on any letter other than F/C/R.
bool parseHistory(const std::string& letters, std::vector<Act>& out);

struct Tree {
    std::vector<int8_t> actor;                // seat to act at each node
    std::vector<uint8_t> legal;               // bit (1 << int(Act)) set if the action is legal
    std::vector<std::array<int32_t, 3>> next; // child node per Act, -1 if terminal or illegal
    explicit Tree(const Config& cfg = {});
    int size() const { return int(actor.size()); }
};

class Strategy {
public:
    // Uniform over the legal actions at every node: a stand-in until a solve exists.
    explicit Strategy(const Config& cfg = {}, const RealizationConfig& rc = {});

    bool load(const std::string& path);  // false (and unchanged) on a missing or malformed file
    bool save(const std::string& path) const;

    const Config& config() const { return cfg_; }  // build Hands with this so the tree matches
    // The payoff model the strategy was solved with; use it for rollouts so EVs match the solve.
    const RealizationConfig& realization() const { return rc_; }
    // The measured realization table it was solved with (Solver::setRealizationTable), nullptr if none.
    // Pass it to utility()/payoffs() with realization().
    const RealizationTable* realizationTable() const { return table_.entries.empty() ? nullptr : &table_; }
    void setRealizationTable(const RealizationTable& t) { table_ = t; }
    const Tree& tree() const { return tree_; }

    int root() const { return 0; }
    int child(int node, Act a) const { return tree_.next[node][int(a)]; }
    int actor(int node) const { return tree_.actor[node]; }
    int nodeFor(const std::vector<Act>& history) const;  // -1 if illegal or past a terminal
    // For the player to act after `history`: per hand class, the probability that its own
    // earlier actions on this path were taken (1 where it hasn't acted yet). Call only when
    // nodeFor(history) >= 0, unless `seat` names the player: then any legal history, terminal included.
    std::array<double, NBUCKETS> reach(const std::vector<Act>& history, int seat = -1) const;

    // P(Fold), P(Call), P(Raise) for a hand bucket (solver/cards.h) at a node; illegal actions are 0.
    const std::array<float, 3>& probs(int node, int bucket) const;
    std::array<float, 3>& probs(int node, int bucket);  // for the solver writing results

private:
    Config cfg_;
    RealizationConfig rc_;
    RealizationTable table_;
    Tree tree_;
    std::vector<std::array<float, 3>> p_;  // [node * 169 + bucket]
};

// Overwrites probs() at every spot with a chart in `dir` (top-level *.txt only; *.off and subdirs
// ignored). With `scope: listed` only the named classes are overwritten. Returns the number of charts
// applied, or -1 with `err` set on a malformed file or a spot that isn't a node of s.tree(). If `locked`
// is non-null it is resized to tree().size() * NBUCKETS and set to 1 at [node * NBUCKETS + bucket] for
// every entry taken from a chart, 0 elsewhere.
// Charts are read in file-name order (a later chart for the same spot wins); on -1, s and `locked`
// are unchanged. An unreadable `dir` is -1 too.
int applyCharts(Strategy& s, const std::string& dir, std::vector<uint8_t>* locked = nullptr, std::string* err = nullptr);

}  // namespace pf
