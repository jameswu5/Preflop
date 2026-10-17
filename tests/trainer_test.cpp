#undef NDEBUG  // the build defaults to Release; keep asserts on
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>

#include "solver/cards.h"
#include "trainer/trainer.h"

using namespace pf;
const Act F = Act::Fold, C = Act::Call, R = Act::Raise;

static bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) < tol; }

int main() {
    std::mt19937 rng(7);
    {  // spot filters on the uniform strategy
        Strategy s;
        for (int t = 0; t < NSPOT; ++t)
            for (int pos : {CO, BTN}) {
                auto sp = deal(s, {SpotType(t), pos}, rng);
                assert(sp && sp->user == pos && sp->hand.toAct() == pos && sp->type == SpotType(t));
                assert(classify(s, sp->path, pos) == sp->type && s.actor(sp->node) == pos);
                std::vector<Act> hist;
                for (auto& st : sp->path) hist.push_back(st.act);
                assert(s.nodeFor(hist) == sp->node);
            }
        assert(!deal(s, {SpotType::Squeeze, UTG}, rng, 1000));  // nobody can have opened yet
        assert(!deal(s, {SpotType::VsSqueeze, BB}, rng, 1000));  // calling closes the action at BB

        // classify on hand-built paths: nodes from the tree, so actors are known
        auto pathOf = [&](std::vector<Act> acts) {
            std::vector<Step> p;
            for (int n = s.root(); Act a : acts) p.push_back({n, a}), n = s.child(n, a);
            return p;
        };
        auto sqz = pathOf({F, R, F, C, R, F, F});  // HJ opens, BTN calls, SB squeezes, BB and HJ fold
        assert(s.actor(s.nodeFor({F, R, F, C, R, F, F})) == BTN);
        assert(classify(s, sqz, BTN) == SpotType::VsSqueeze && classify(s, sqz, HJ) == SpotType::FourBet);  // HJ opened
        auto tb = pathOf({F, F, R, R, F, F});  // CO opens, BTN 3-bets, blinds fold: CO faces the 3-bet
        assert(s.actor(s.nodeFor({F, F, R, R, F, F})) == CO && classify(s, tb, CO) == SpotType::FourBet);
        auto vs = deal(s, {SpotType::VsSqueeze, BTN}, rng);  // default Config: the cold caller may call
        auto legal = vs->hand.legal();
        assert(std::find(legal.begin(), legal.end(), C) != legal.end());
        assert(!deal(s, {SpotType::Open, BB}, rng, 1000));      // no limps, so BB never sees it unopened
        auto any = deal(s, {}, rng);
        assert(any && any->type == classify(s, any->path, any->user));

        // uniform weight = product of 1/|legal| over the bots' steps
        auto sq = deal(s, {SpotType::Squeeze, BTN}, rng);
        double w = 1;
        for (auto& st : sq->path)
            if (s.actor(st.node) != BTN) w /= __builtin_popcount(s.tree().legal[st.node]);
        assert(near(historyWeight(s, *sq, sq->hand.cards), w, 1e-6));  // probs are float
    }
    {  // known EV under weights: UTG opens only pairs (AA always .. 22 1/13), everyone else folds
        Strategy s;
        for (int n = 0; n < s.tree().size(); ++n)
            for (int b = 0; b < NBUCKETS; ++b) s.probs(n, b) = {1, 0, 0};
        for (int r = 0; r < 13; ++r) {
            float p = (13 - r) / 13.0f;
            s.probs(s.root(), r * 13 + r) = {1 - p, 0, p};
        }
        auto sp = deal(s, {SpotType::ThreeBet, HJ}, rng);
        assert(sp && sp->path.size() == 1 && sp->path[0].act == R);

        Holes h = sp->hand.cards;
        auto setUtg = [&](const char* name) {
            for (int a = 0; a < 52; ++a)
                for (int b = a + 1; b < 52; ++b)
                    if (bucket(cardFromIndex(a), cardFromIndex(b)) == bucketFromName(name))
                        return void(h[UTG] = {cardFromIndex(a), cardFromIndex(b)});
        };
        setUtg("AA"), assert(near(historyWeight(s, *sp, h), 1));
        setUtg("22"), assert(near(historyWeight(s, *sp, h), 1 / 13.0, 1e-6));
        setUtg("AKs"), assert(historyWeight(s, *sp, h) == 0);

        Equity e;
        RolloutConfig rc;
        rc.deals = 50000;
        auto ev = rollout(s, e, *sp, rng, rc);
        assert(near(ev[int(F)], 0) && near(ev[int(R)], 4));  // 3-bet to 8.8 takes the 2.5 open + blinds

        if (e.loadHuTable(DATA_DIR "/hu_equity.bin")) {
            // Call: HU flop vs UTG's pair range, by exact enumeration over UTG combos.
            Hand end = sp->hand;
            for (Act a : {C, F, F, F, F}) end.apply(a);
            assert(end.done() && near(end.pot(), 6.5));
            auto mine = sp->hand.cards[HJ];
            double num = 0, den = 0;
            for (int a = 0; a < 52; ++a)
                for (int b = a + 1; b < 52; ++b) {
                    Card ca = cardFromIndex(a), cb = cardFromIndex(b);
                    if (a == cardIndex(mine[0]) || a == cardIndex(mine[1]) || b == cardIndex(mine[0]) || b == cardIndex(mine[1]))
                        continue;
                    double w = s.probs(s.root(), bucket(ca, cb))[int(R)];
                    if (w == 0) continue;
                    end.cards[UTG] = {ca, cb};
                    std::array<double, NPOS> eq;
                    e.compute(end.cards, {true, true, false, false, false, false}, rng, 1, eq);
                    num += w * payoffs(end, eq)[HJ], den += w;
                }
            ev = rollout(s, e, *sp, rng, rc);
            std::printf("call EV: rollout %.3f, exact %.3f\n", ev[int(C)], num / den);
            assert(near(ev[int(C)], num / den, 0.2));
        } else {
            std::puts("no hu_equity.bin: skipping the call-EV enumeration");
        }
    }
    {  // same seed, same spot and EVs
        Strategy s;
        Equity e;
        RolloutConfig rc;
        rc.deals = 2000;
        std::array<double, 3> ev[2];
        int node[2];
        for (int i = 0; i < 2; ++i) {
            std::mt19937 r(123);
            auto sp = deal(s, {SpotType::FourBet, -1}, r);
            node[i] = sp->node;
            ev[i] = rollout(s, e, *sp, r, rc);
        }
        assert(node[0] == node[1]);
        for (int a = 0; a < 3; ++a) assert(ev[0][a] == ev[1][a] || (std::isnan(ev[0][a]) && std::isnan(ev[1][a])));
    }
    {  // grading and stats
        Strategy s;
        auto sp = deal(s, {SpotType::ThreeBet, BTN}, rng);  // uniform: F/C/R each 1/3
        std::array<double, 3> ev{0, -1, 0.5};
        Feedback f = grade(s, *sp, ev, C, {0.5, 0.1});
        assert(near(f.evLoss, 1.5) && !f.correct && near(f.mix[int(C)], 1 / 3.0, 1e-6));
        assert(grade(s, *sp, ev, C, {0.3, 0.1, 2.0}).correct);  // mixed often enough, loss under the cap
        assert(!grade(s, *sp, ev, C, {0.3, 0.1}).correct);      // mixed, but 1.5bb > the default 1bb cap
        assert(!grade(s, *sp, {0, -20.8, 0}, C, {}).correct);   // the reported case: a 20.8bb loss in the mix
        assert(grade(s, *sp, ev, R, {0.5, 0.1}).correct);  // best action

        Stats st;
        st.add(*sp, f);
        auto sp2 = deal(s, {SpotType::Open, UTG}, rng);
        st.add(*sp2, grade(s, *sp2, {0, -1, -0.1}, R, {}));
        auto l = st.leaks();
        assert(l.size() == 2 && l[0] == std::pair(SpotType::ThreeBet, int(BTN)));
        auto& c = st.cell[int(SpotType::Open)][UTG];
        assert(c.n == 1 && c.correct == 1 && near(c.evLoss, 0.1));
    }
    std::puts("trainer_test: ok");
    return 0;
}
