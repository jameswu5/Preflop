#pragma once
// Solved (or dummy) preflop strategy: the betting tree plus action probabilities per
// (node, hand bucket). Written by the solver, read by the trainer bots and the GUI.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "solver/utility.h"
#include "trainer/game.h"

namespace pf {

// Every betting history reachable from the start of a hand (UTG to act), built by walking Hand.
// Node 0 is the root. Terminal states are not nodes: child() returns -1 for them.
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
    const Tree& tree() const { return tree_; }

    int root() const { return 0; }
    int child(int node, Act a) const { return tree_.next[node][int(a)]; }
    int actor(int node) const { return tree_.actor[node]; }
    int nodeFor(const std::vector<Act>& history) const;  // -1 if illegal or past a terminal

    // P(Fold), P(Call), P(Raise) for a hand bucket (solver/cards.h) at a node; illegal actions are 0.
    const std::array<float, 3>& probs(int node, int bucket) const;
    std::array<float, 3>& probs(int node, int bucket);  // for the solver writing results

private:
    Config cfg_;
    RealizationConfig rc_;
    Tree tree_;
    std::vector<std::array<float, 3>> p_;  // [node * 169 + bucket]
};

}  // namespace pf
