#undef NDEBUG  // the build defaults to Release; keep asserts on
#include <cassert>
#include <cmath>
#include <cstdio>
#include <set>
#include <stdexcept>

#include "trainer/game.h"

using namespace pf;
using A = std::vector<Act>;
const Act F = Act::Fold, C = Act::Call, R = Act::Raise;

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

static std::mt19937 rng(42);

// Asserts `pos` is to act with exactly `legal` available, then applies `a`.
static void act(Hand& h, int pos, A legal, Act a) {
    assert(!h.done());
    assert(h.toAct() == pos);
    assert(h.legal() == legal);
    h.apply(a);
}

static void foldTo(Hand& h, int pos) {
    for (int p = h.toAct(); p != pos; p = h.toAct()) h.apply(F);
}

int main() {
    {  // deal + blinds
        Hand h(rng);
        std::set<int> seen;
        for (auto& hc : h.cards)
            for (auto c : hc) seen.insert(c.rank * 4 + c.suit), assert(c.rank < 13 && c.suit < 4);
        assert(seen.size() == 12);
        assert(near(h.committed(SB), 0.5) && near(h.committed(BB), 1) && near(h.pot(), 1.5));
        assert(near(h.stack(BB), 99) && near(h.currentBet(), 1));
    }
    {  // fold-out: everyone folds, BB wins without acting; no SB complete
        Hand h(rng);
        for (int p = UTG; p < SB; ++p) act(h, p, {F, R}, F);
        act(h, SB, {F, R}, F);
        assert(h.done() && h.toAct() == -1 && h.legal().empty() && !h.folded(BB));
    }
    {  // open, all fold: done right after BB folds
        Hand h(rng);
        assert(near(h.raiseTo(), 2.5));
        act(h, UTG, {F, R}, R);
        assert(near(h.committed(UTG), 2.5) && near(h.currentBet(), 2.5));
        for (int p = HJ; p < BB; ++p) act(h, p, {F, C, R}, F);
        assert(!h.done());
        act(h, BB, {F, C, R}, F);
        assert(h.done() && near(h.pot(), 4));
    }
    {  // open + overcalls, closes when BB calls
        Hand h(rng);
        act(h, UTG, {F, R}, R);
        act(h, HJ, {F, C, R}, C);
        act(h, CO, {F, C, R}, C);
        act(h, BTN, {F, C, R}, F);
        act(h, SB, {F, C, R}, F);
        assert(!h.done());
        act(h, BB, {F, C, R}, C);
        assert(h.done() && near(h.pot(), 4 * 2.5 + 0.5));
    }
    {  // ex 1: BTN 3-bets CO open (IP) = 3 * 2.5 = 7.5; opener may call, blinds fold/raise only
        Hand h(rng);
        foldTo(h, CO);
        act(h, CO, {F, R}, R);
        assert(h.raiseTo() == 7.5);
        act(h, BTN, {F, C, R}, R);
        assert(h.stack(BTN) == 92.5);
        act(h, SB, {F, R}, F);
        act(h, BB, {F, R}, F);
        act(h, CO, {F, C, R}, C);
        assert(h.done() && h.pot() == 16.5);
    }
    {  // ex 2: BB 3-bets BTN open (OOP) = 5 * 2.5 = 12.5; 4-bet IP 2x; 5-bet all-in; call/fold vs all-in
        Hand h(rng);
        foldTo(h, BTN);
        act(h, BTN, {F, R}, R);
        act(h, SB, {F, C, R}, F);
        assert(h.raiseTo() == 12.5);
        act(h, BB, {F, C, R}, R);
        assert(h.raiseTo() == 25);
        act(h, BTN, {F, C, R}, R);
        assert(near(h.raiseTo(), 100));
        act(h, BB, {F, C, R}, R);
        assert(near(h.stack(BB), 0));
        act(h, BTN, {F, C}, C);
        assert(h.done() && near(h.pot(), 200.5));
    }
    for (double sq : {1.0, 0.0}) {  // ex 3: BTN squeezes HJ open + CO call (IP) = 3 * 2.5 + 2.5 = 10
        Config cfg;                  // squeezeCall: the cold caller may call the squeeze, or only fold/4-bet
        cfg.squeezeCall = sq;
        A coldVsSqueeze = sq ? A{F, C, R} : A{F, R};
        Hand h(rng, cfg);
        foldTo(h, HJ);
        act(h, HJ, {F, R}, R);
        act(h, CO, {F, C, R}, C);
        assert(h.raiseTo() == 10);
        act(h, BTN, {F, C, R}, R);
        act(h, SB, {F, R}, F);
        act(h, BB, {F, R}, F);
        act(h, HJ, {F, C, R}, F);
        assert(h.raiseTo() == 25.5);  // CO is OOP to BTN: 2.55 * 10
        act(h, CO, coldVsSqueeze, R);
        act(h, BTN, {F, C, R}, C);
        assert(h.done());
    }
    {  // squeeze call allowed (default): callers flat the squeeze, but face a 4-bet with fold/raise only
        Hand h(rng);
        foldTo(h, CO);
        act(h, CO, {F, R}, R);
        act(h, BTN, {F, C, R}, C);
        act(h, SB, {F, C, R}, F);
        act(h, BB, {F, C, R}, R);  // squeeze
        act(h, CO, {F, C, R}, F);
        act(h, BTN, {F, C, R}, C);  // flats the squeeze
        assert(h.done() && h.pot() == 2 * 15 + 2.5 + 0.5);  // squeeze 5 * 2.5 + 2.5
        Hand g(rng);
        foldTo(g, CO);
        act(g, CO, {F, R}, R);
        act(g, BTN, {F, C, R}, C);
        act(g, SB, {F, C, R}, F);
        act(g, BB, {F, C, R}, R);
        act(g, CO, {F, C, R}, R);   // 4-bet
        act(g, BTN, {F, R}, F);     // the caller faces a 4-bet: no flat
        act(g, BB, {F, C, R}, F);
        assert(g.done());
    }
    {  // a player who hasn't acted still can't cold-call a 3-bet
        Hand h(rng);
        foldTo(h, CO);
        act(h, CO, {F, R}, R);
        act(h, BTN, {F, C, R}, R);
        act(h, SB, {F, R}, F);
    }
    {  // ex 4: BB squeezes CO open + BTN call + SB overcall (OOP) = 5 * 2.5 + 2 * 2.5 = 17.5
        Hand h(rng);
        foldTo(h, CO);
        act(h, CO, {F, R}, R);
        act(h, BTN, {F, C, R}, C);
        act(h, SB, {F, C, R}, C);
        assert(h.raiseTo() == 17.5);
        act(h, BB, {F, C, R}, R);
        act(h, CO, {F, C, R}, F);
        act(h, BTN, {F, C, R}, F);  // callers may flat the squeeze (squeezeCall default on)
        assert(!h.done());
        act(h, SB, {F, C, R}, F);
        assert(h.done() && h.pot() == 25);
    }
    {  // ex 5: BB 3-bets SB open (3bb, IP) = 9; SB 4-bets OOP 2.55x
        Hand h(rng);
        foldTo(h, SB);
        assert(near(h.raiseTo(), 3));
        act(h, SB, {F, R}, R);
        assert(h.raiseTo() == 9);
        act(h, BB, {F, C, R}, R);
        assert(h.raiseTo() == 23);  // 2.55 * 9 = 22.95
        act(h, SB, {F, C, R}, R);
        act(h, BB, {F, C, R}, F);
        assert(h.done());
    }
    {  // reference sizes (pokertrainer GTO range viewer, docs/game-rules.md), straight from the defaults
        auto at = [&](std::initializer_list<Act> acts) {
            Hand h(rng);
            for (Act a : acts) h.apply(a);
            return h.raiseTo();
        };
        assert(at({}) == 2.5 && at({F, F, F, F}) == 3);         // opens
        assert(at({F, F, R}) == 7.5);                           // BTN 3-bets a CO open
        assert(at({F, F, R, F}) == 12.5 && at({F, F, R, F, F}) == 12.5);  // SB, BB 3-bet it
        assert(at({F, F, F, F, R}) == 9);                       // BB 3-bets an SB open
        assert(at({F, F, R, R, F, F}) == 19.1);                 // CO 4-bets BTN's 7.5 (site: 19; 2.55 can't hit 19 and 23)
        assert(at({F, F, R, F, F, R}) == 25);                   // CO 4-bets BB's 12.5
        assert(at({F, F, F, F, R, R}) == 23);                   // SB 4-bets BB's 9
    }
    {  // cold 4-bet, cold 5-bet shove, multiway call/fold vs all-in
        Hand h(rng);
        act(h, UTG, {F, R}, R);
        assert(h.raiseTo() == 7.5);
        act(h, HJ, {F, C, R}, R);
        assert(h.raiseTo() == 15);  // CO is IP to HJ: 2 * 7.5
        act(h, CO, {F, R}, R);
        assert(h.committed(CO) == 15 && h.stack(CO) == 85);
        act(h, BTN, {F, R}, R);
        assert(near(h.committed(BTN), 100));
        act(h, SB, {F, C}, F);
        act(h, BB, {F, C}, C);
        act(h, UTG, {F, C}, F);
        act(h, HJ, {F, C}, F);
        assert(!h.done());
        act(h, CO, {F, C}, C);
        assert(h.done() && h.pot() == 310.5);
    }
    {  // config file
        const char* path = "game_test.cfg";
        FILE* f = std::fopen(path, "w");
        std::fputs("# reference setup\nopen = 2.2  # min-ish\nthreeBetOOP=5\ntick = 0.05\nrake = 0.045\nrakeCap = 3\n\n", f);
        std::fclose(f);
        Config c = loadConfig(path);
        assert(near(c.open, 2.2) && near(c.threeBetOOP, 5) && near(c.tick, 0.05) && near(c.stack, 100));
        assert(near(c.rake, 0.045) && near(c.rakeCap, 3));
        f = std::fopen(path, "w");
        std::fputs("opne = 2\n", f);
        std::fclose(f);
        bool threw = false;
        try { loadConfig(path); } catch (const std::runtime_error&) { threw = true; }
        assert(threw);
        std::remove(path);
    }
    std::puts("game_test: ok");
    return 0;
}
