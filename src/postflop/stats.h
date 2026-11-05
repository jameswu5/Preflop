#pragma once
// Solver internals for tools and tests; not part of the postflop.h contract.
#include <cstddef>

#include "postflop/postflop.h"

namespace pf::post {

struct Stats {
    size_t bytes = 0;            // tree and regret/strategy storage
    size_t actionNodes = 0, nodes = 0;
    int hands[2] = {};           // combos in each range after the board
    double buildSec = 0, solveSec = 0;
};

Result solve(const Spot& spot, const Options& opt, Stats* stats);
Stats measure(const Spot& spot, const Options& opt);  // builds the tree only: sizes without allocating the storage

}  // namespace pf::post
