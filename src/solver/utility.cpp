#include "solver/utility.h"

#include <algorithm>
#include <cassert>

namespace pf {

static std::array<bool, NPOS> liveSeats(const Hand& h) {
    std::array<bool, NPOS> live;
    for (int p = 0; p < NPOS; ++p) live[p] = !h.folded(p);
    return live;
}

bool allIn(const Hand& h) {
    for (int p = 0; p < NPOS; ++p)
        if (!h.folded(p) && h.stack(p) <= 0) return true;
    return false;
}

std::array<double, NPOS> payoffs(const Hand& h, const std::array<double, NPOS>& eq,
                                 const RealizationConfig& rc) {
    assert(h.done());
    auto live = liveSeats(h);
    int nLive = 0, last = -1;  // last = live seat acting last postflop (SB first, BTN last)
    for (int p : {SB, BB, UTG, HJ, CO, BTN})
        if (live[p]) ++nLive, last = p;

    std::array<double, NPOS> share{};
    if (nLive == 1) {
        share[last] = 1;
    } else {
        bool ai = allIn(h);
        double sum = 0;
        for (int p = 0; p < NPOS; ++p)
            if (live[p]) sum += share[p] = eq[p] * (ai ? 1 : p == last ? rc.ip : rc.oop);
        assert(sum > 0);
        for (double& s : share) s /= sum;
    }
    std::array<double, NPOS> out;
    for (int p = 0; p < NPOS; ++p) out[p] = share[p] * h.pot() - h.committed(p);
    return out;
}

std::array<double, NPOS> utility(const Hand& h, const Equity& e, std::mt19937& rng,
                                 int flopBoards, const RealizationConfig& rc) {
    auto live = liveSeats(h);
    std::array<double, NPOS> eq{};
    if (std::count(live.begin(), live.end(), true) > 1)
        e.compute(h.cards, live, rng, allIn(h) ? 1 : flopBoards, eq);
    return payoffs(h, eq, rc);
}

}  // namespace pf
