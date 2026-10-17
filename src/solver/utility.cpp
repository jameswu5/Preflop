#include "solver/utility.h"

#include <algorithm>
#include <cassert>

namespace pf {

static std::array<bool, NPOS> liveSeats(const Hand& h) {
    std::array<bool, NPOS> live;
    for (int p = 0; p < NPOS; ++p) live[p] = !h.folded(p);
    return live;
}

int straights(Card a, Card b) {
    int hi = std::max(a.rank, b.rank), lo = std::min(a.rank, b.rank), n = 0;
    if (hi == lo) return 0;
    for (int s = -1; s <= 8; ++s) {  // ranks s..s+4; s = -1 is the wheel, where the ace plays low
        auto in = [&](int r) { return (r >= s && r <= s + 4) || (s == -1 && r == 12); };
        n += in(hi) && in(lo);
    }
    return n;
}

int domination(Card a, Card b) {
    int hi = std::max(a.rank, b.rank), lo = std::min(a.rank, b.rank);
    if (hi == lo || a.suit == b.suit || hi < 8) return 0;  // 8 = T
    return std::min(hi - 1 - lo, 4);
}

double realization(const RealizationConfig& rc, bool lastToAct, bool raiser, double spr, Card a, Card b) {
    int hi = std::max(a.rank, b.rank);
    bool pair = a.rank == b.rank, suited = a.suit == b.suit;
    double g = rc.spr > 0 ? spr / (spr + rc.spr) : 1;
    double pos = lastToAct ? rc.ip : 1 - (1 - rc.oop) * g;
    double h = 1 + rc.flush * suited + rc.conn * straights(a, b) * g + rc.set * pair * g -
               rc.dom * domination(a, b) * !raiser - rc.low * (12 - hi) * (!pair && !suited) -
               rc.pairLow * (12 - hi) * pair;
    return std::max(pos * h * (raiser ? 1 + rc.init : 1), 0.05);
}

double rake(const Hand& h) {
    int live = 0;
    for (int p = 0; p < NPOS; ++p) live += !h.folded(p);
    if (live < 2) return 0;  // no flop, no drop
    return std::min(h.config().rake * h.pot(), h.config().rakeCap);
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
        double spr = h.stack(last) / h.pot(), sum = 0;  // live stacks are equal at a flop
        for (int p = 0; p < NPOS; ++p)
            if (live[p])
                sum += share[p] = eq[p] * (ai ? 1 : realization(rc, p == last, p == h.lastRaiser(), spr,
                                                                h.cards[p][0], h.cards[p][1]));
        assert(sum > 0);
        for (double& s : share) s /= sum;
    }
    std::array<double, NPOS> out;
    double net = h.pot() - rake(h);
    for (int p = 0; p < NPOS; ++p) out[p] = share[p] * net - h.committed(p);
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
