// End to end through the realization-table pipeline: spots, ranges, postflop solves, aggregation,
// save and load. Linked against the stub solve() (ev = equity * pot), so every class must come out R = 1.
#undef NDEBUG  // the build defaults to Release; keep asserts on
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "postflop/postflop.h"
#include "solver/spots.h"

using namespace pf;

int main() {
    Config cfg;
    Strategy s(cfg);  // uniform: every class reaches every spot
    auto spots = flopSpots(cfg);
    const int flops[][3] = {{0, 5, 50}, {12, 13, 14}};  // 2c 3d Ah and 5c 5d 5h (cardIndex)
    const double mult[] = {24, 4};
    RealizationTable t;
    for (int i : {0, 20, 40}) {  // one spot per pot type
        const FlopSpot& fs = spots[i];
        post::Spot spot;
        spot.range[0] = lineRange(s, fs.history, fs.oop()), spot.range[1] = lineRange(s, fs.history, fs.ip());
        spot.tree.pot = fs.potBB, spot.tree.stack = fs.stackBB;
        RealizationAccum acc(fs);
        for (int f = 0; f < 2; ++f) {
            for (int k = 0; k < 3; ++k) spot.flop[k] = flops[f][k];
            acc.add(post::solve(spot, {}), mult[f]);
        }
        RealizationEntry e = acc.entry();
        assert(e.pot == fs.pot && e.raiser == fs.raiser && e.caller == fs.caller && e.flops == 2 && e.potBB == fs.potBB);
        double nr = 0, nc = 0;
        for (int b = 0; b < NBUCKETS; ++b) {
            assert(e.nRaiser[b] > 0 && e.nCaller[b] > 0);  // uniform ranges: every class present
            assert(std::abs(e.rRaiser[b] - 1) < 1e-5 && std::abs(e.rCaller[b] - 1) < 1e-5);
            nr += e.nRaiser[b], nc += e.nCaller[b];
        }
        assert(std::abs(nr - 28) < 1e-3 && std::abs(nc - 28) < 1e-3);  // total multiplicity
        t.entries.push_back(e);
    }
    std::string err;
    RealizationTable u;
    assert(saveRealizationTable("pipeline_test.bin", t, &err) && loadRealizationTable("pipeline_test.bin", u, &err));
    assert(u.entries.size() == 3 && u.entries[2].rCaller == t.entries[2].rCaller);
    std::filesystem::remove("pipeline_test.bin");
    std::puts("realization_pipeline_test OK");
}
