#undef NDEBUG  // the build defaults to Release; keep asserts on
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <set>
#include <tuple>

#include "solver/range.h"
#include "solver/spots.h"

using namespace pf;

static float combo(const post::ComboWeights& w, const char* cls) {  // weight of one combo of a class
    int b = bucketFromName(cls), r1 = 12 - b / 13, r2 = 12 - b % 13;
    bool suited = b / 13 < b % 13;
    return w[comboIndex({uint8_t(r1), 0}, {uint8_t(r2), uint8_t(suited ? 0 : 1)})];
}

int main() {
    Config cfg;
    auto spots = flopSpots(cfg);
    // 15 (raiser, caller) seat pairs per pot type, each once.
    std::set<std::tuple<PotType, Pos, Pos>> keys;
    int count[3] = {};
    for (auto& s : spots) {
        assert(keys.insert({s.pot, s.raiser, s.caller}).second);
        ++count[int(s.pot)];
        assert(s.potBB > 0 && s.stackBB > 0 && s.oop() != s.ip());
    }
    std::printf("spots: %d single, %d 3-bet, %d 4-bet\n", count[0], count[1], count[2]);
    assert(count[0] == 15 && count[1] == 15 && count[2] == 15);
    for (size_t i = 1; i < spots.size(); ++i) assert(spots[i - 1].pot <= spots[i].pot);

    auto find = [&](PotType t, Pos r, Pos c) -> const FlopSpot& {
        for (auto& s : spots)
            if (s.pot == t && s.raiser == r && s.caller == c) return s;
        std::abort();
    };
    const FlopSpot& utgBB = find(PotType::Single, UTG, BB);  // 2.5 open + call + dead SB
    assert(std::abs(utgBB.potBB - 5.5) < 1e-9 && std::abs(utgBB.stackBB - 97.5) < 1e-9);
    assert(utgBB.oop() == BB && utgBB.ip() == UTG);
    const FlopSpot& bbSB = find(PotType::ThreeBet, BB, SB);  // SB opens, BB 3-bets, SB calls
    assert(bbSB.oop() == SB && bbSB.history.size() == 7);
    assert(find(PotType::FourBet, CO, BTN).oop() == CO);

    // Uniform strategy: each own decision is 1/k over the legal actions.
    Strategy uni(cfg);
    auto opener = lineRange(uni, utgBB.history, UTG), caller = lineRange(uni, utgBB.history, BB);
    assert(combo(opener, "AA") == 0.5f && combo(opener, "72o") == 0.5f);
    assert(std::abs(combo(caller, "KQs") - 1.0f / 3) < 1e-6);

    // Chart ranges: UTG's open and BB's call over it come straight from ref/.
    std::string err;
    Strategy charts(cfg);
    assert(applyCharts(charts, REF_DIR, nullptr, &err) > 0);
    Chart open, vsOpen;
    assert(readChart(REF_DIR "/gto_open_lj.txt", open, err) && readChart(REF_DIR "/gto_vs_open_bb_vs_lj.txt", vsOpen, err));
    opener = lineRange(charts, utgBB.history, UTG), caller = lineRange(charts, utgBB.history, BB);
    for (const char* c : {"AA", "K7s", "QJo", "72o"}) assert(combo(opener, c) == open.act[2][bucketFromName(c)]);
    for (const char* c : {"AQs", "A5s", "KJs", "72o"}) assert(combo(caller, c) == vsOpen.act[1][bucketFromName(c)]);
    std::puts("spots_test OK");
}
