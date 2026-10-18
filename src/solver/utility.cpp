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
    bool oopCaller = !lastToAct && !raiser;
    double fade = oopCaller ? rc.callerShrink * g + 1 - rc.callerShrink : g;
    double pos = lastToAct ? rc.ip : 1 - (1 - rc.oop) * fade;
    double play = rc.flush * suited + rc.conn * straights(a, b) * g + rc.set * pair * g;
    double h = 1 + play * (oopCaller ? rc.oopCall : 1) - rc.dom * domination(a, b) * !raiser -
               rc.low * (12 - hi) * (!pair && !suited) - rc.pairLow * (12 - hi) * pair;
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

Terminal::Terminal(const Hand& h) : lastRaiser(h.lastRaiser()), allIn(pf::allIn(h)) {
    assert(h.done());
    for (int p : {SB, BB, UTG, HJ, CO, BTN})  // postflop order: SB first, BTN last
        if (!h.folded(p)) live |= 1 << p, last = p;
    for (int p = 0; p < NPOS; ++p) committed[p] = h.committed(p);
    spr = h.stack(last) / h.pot();  // live stacks are equal at a flop
    net = h.pot() - rake(h);
}

std::array<double, NPOS> payoffs(const Terminal& t, const Holes& cards, const std::array<double, NPOS>& eq,
                                 const RealizationConfig& rc) {
    std::array<double, NPOS> share{};
    if (__builtin_popcount(t.live) == 1) {
        share[t.last] = 1;
    } else {
        double sum = 0;
        for (int p = 0; p < NPOS; ++p)
            if (t.live >> p & 1)
                sum += share[p] = eq[p] * (t.allIn ? 1 : realization(rc, p == t.last, p == t.lastRaiser, t.spr,
                                                                     cards[p][0], cards[p][1]));
        assert(sum > 0);
        for (double& s : share) s /= sum;
    }
    std::array<double, NPOS> out;
    for (int p = 0; p < NPOS; ++p) out[p] = share[p] * t.net - t.committed[p];
    return out;
}

std::array<double, NPOS> payoffs(const Hand& h, const std::array<double, NPOS>& eq,
                                 const RealizationConfig& rc) {
    return payoffs(Terminal(h), h.cards, eq, rc);
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
