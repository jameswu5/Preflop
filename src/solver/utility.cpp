#include "solver/utility.h"

#include <algorithm>
#include <cassert>
#include <cmath>

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

double realization(const RealizationConfig& rc, bool lastToAct, bool raiser, double spr, Card a, Card b,
                   bool reraised) {
    int hi = std::max(a.rank, b.rank);
    bool pair = a.rank == b.rank, suited = a.suit == b.suit;
    double g = rc.spr > 0 ? spr / (spr + rc.spr) : 1;
    bool oopCaller = !lastToAct && !raiser;
    double fade = oopCaller ? rc.callerShrink * g + 1 - rc.callerShrink : g;
    double pos = lastToAct ? rc.ip * (!raiser && reraised ? rc.ipCall : 1) : 1 - (1 - rc.oop) * fade;
    double play = rc.flush * suited + rc.conn * straights(a, b) * g + rc.set * pair * g;
    double h = 1 + play * (oopCaller ? rc.oopCall : 1) - (rc.dom + rc.domPot * reraised) * domination(a, b) * !raiser -
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

bool headsUpFlop(const Hand& h, PotType& pot, Pos& raiser, Pos& caller, bool dead) {
    const Config& cfg = h.config();
    int live = 0, other = -1;
    for (int p = 0; p < NPOS; ++p) {
        if (!h.folded(p)) ++live, other = p != h.lastRaiser() ? p : other;
        else if (!dead && h.committed(p) != (p == SB ? cfg.sb : p == BB ? cfg.bb : 0)) return false;  // dead money
    }
    if (live != 2 || pf::allIn(h) || h.raises() < 1 || h.raises() > 3) return false;
    pot = PotType(h.raises() - 1), raiser = Pos(h.lastRaiser()), caller = Pos(other);
    return true;
}

const RealizationEntry* findRealization(const RealizationTable* t, const Hand& h) {
    PotType pot;
    Pos raiser, caller;
    if (!t || !headsUpFlop(h, pot, raiser, caller, true)) return nullptr;
    for (const RealizationEntry& e : t->entries)
        if (e.pot == pot && e.raiser == raiser && e.caller == caller && std::abs(e.potBB - h.pot()) < 1e-9 &&
            std::abs(e.stackBB - h.stack(caller)) < 1e-9)
            return &e;
    return nullptr;
}

Terminal::Terminal(const Hand& h, const RealizationTable* t)
    : lastRaiser(h.lastRaiser()), allIn(pf::allIn(h)), reraised(h.raises() >= 2), table(findRealization(t, h)) {
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
            if (t.live >> p & 1) {
                double r = t.allIn ? 1 : 0;
                if (t.table) {
                    auto& rs = p == t.lastRaiser ? t.table->rRaiser : t.table->rCaller;
                    r = rs[bucket(cards[p][0], cards[p][1])];
                }
                if (!(r > 0))  // no table, or a class it never saw
                    r = realization(rc, p == t.last, p == t.lastRaiser, t.spr, cards[p][0], cards[p][1], t.reraised);
                sum += share[p] = eq[p] * r;
            }
        assert(sum > 0);
        for (double& s : share) s /= sum;
    }
    std::array<double, NPOS> out;
    for (int p = 0; p < NPOS; ++p) out[p] = share[p] * t.net - t.committed[p];
    return out;
}

std::array<double, NPOS> payoffs(const Hand& h, const std::array<double, NPOS>& eq,
                                 const RealizationConfig& rc, const RealizationTable* table) {
    return payoffs(Terminal(h, table), h.cards, eq, rc);
}

std::array<double, NPOS> utility(const Hand& h, const Equity& e, std::mt19937& rng,
                                 int flopBoards, const RealizationConfig& rc, const RealizationTable* table) {
    auto live = liveSeats(h);
    std::array<double, NPOS> eq{};
    if (std::count(live.begin(), live.end(), true) > 1)
        e.compute(h.cards, live, rng, allIn(h) ? 1 : flopBoards, eq);
    return payoffs(h, eq, rc, table);
}

}  // namespace pf
