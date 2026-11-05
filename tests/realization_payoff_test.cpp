// Measured realization in the payoffs: covered heads-up flops take R from the table, everything else
// (and every solve without a table) is bit-identical to the formula.
#undef NDEBUG  // the build defaults to Release; keep asserts on
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "solver/mccfr.h"
#include "solver/spots.h"

using namespace pf;
const Act F = Act::Fold, C = Act::Call, R = Act::Raise;

static bool near(double a, double b) { return std::abs(a - b) < 1e-9; }
static bool same(const std::array<double, NPOS>& a, const std::array<double, NPOS>& b) {
    return std::memcmp(a.data(), b.data(), sizeof a) == 0;
}
// payoffs() as it was before realization tables (utility.cpp at v2), to check the no-table path against.
static std::array<double, NPOS> formula(const Hand& h, const std::array<double, NPOS>& eq, const RealizationConfig& rc) {
    Terminal t(h);
    std::array<double, NPOS> share{};
    if (__builtin_popcount(t.live) == 1) {
        share[t.last] = 1;
    } else {
        double sum = 0;
        for (int p = 0; p < NPOS; ++p)
            if (t.live >> p & 1)
                sum += share[p] = eq[p] * (t.allIn ? 1 : realization(rc, p == t.last, p == t.lastRaiser, t.spr,
                                                                     h.cards[p][0], h.cards[p][1], t.reraised));
        for (double& s : share) s /= sum;
    }
    std::array<double, NPOS> out;
    for (int p = 0; p < NPOS; ++p) out[p] = share[p] * t.net - t.committed[p];
    return out;
}

static Hand play(std::mt19937& rng, std::initializer_list<Act> acts, const Config& cfg = {}) {
    Hand h(rng, cfg);
    for (Act a : acts) h.apply(a);
    assert(h.done());
    return h;
}
static bool sameStrategy(const Strategy& a, const Strategy& b) {
    for (int n = 0; n < a.tree().size(); ++n)
        for (int k = 0; k < NBUCKETS; ++k)
            if (a.probs(n, k) != b.probs(n, k)) return false;
    return true;
}

int main() {
    const RealizationConfig rc;  // today's defaults
    std::mt19937 rng(7);

    // Table: every covered spot, raiser R = 2, caller R = 1, except AA as the raiser (absent: R = 0).
    RealizationTable table;
    table.source = "test";
    for (const FlopSpot& s : flopSpots(Config{})) {
        RealizationEntry e;
        e.pot = s.pot, e.raiser = s.raiser, e.caller = s.caller, e.potBB = s.potBB, e.stackBB = s.stackBB;
        e.rRaiser.fill(2), e.rCaller.fill(1), e.rRaiser[bucketFromName("AA")] = 0;
        table.entries.push_back(e);
    }
    const RealizationTable empty;

    {  // no table, or an empty one: the same bits at every kind of terminal, over many deals
        for (int i = 0; i < 2000; ++i) {
            Hand h(rng);
            while (!h.done()) {
                auto l = h.legal();
                h.apply(l[rng() % l.size()]);
            }
            std::array<double, NPOS> eq;
            for (double& x : eq) x = std::uniform_real_distribution<>(0.05, 1)(rng);
            auto base = payoffs(h, eq, rc);
            assert(same(base, formula(h, eq, rc)));
            assert(same(base, payoffs(h, eq, rc, nullptr)) && same(base, payoffs(h, eq, rc, &empty)));
            if (!findRealization(&table, h)) assert(same(base, payoffs(h, eq, rc, &table)));  // uncovered: formula
        }
    }
    {  // covered: UTG opens, BB calls. Shares are R * eq renormalised, rake off the pot.
        Hand h = play(rng, {R, F, F, F, F, C});
        const RealizationEntry* e = findRealization(&table, h);
        assert(e && e->pot == PotType::Single && e->raiser == UTG && e->caller == BB);
        h.cards[UTG] = {Card{11, 0}, Card{10, 1}};  // KQo
        auto u = payoffs(h, {0.6, 0, 0, 0, 0, 0.4}, rc, &table);
        double net = h.pot() - rake(h);
        assert(near(u[UTG], 2 * 0.6 / (2 * 0.6 + 0.4) * net - 2.5) && near(u[BB], 0.4 / (2 * 0.6 + 0.4) * net - 2.5));

        h.cards[UTG] = {Card{12, 0}, Card{12, 1}};  // AA: absent from the table, so the formula's R
        u = payoffs(h, {0.6, 0, 0, 0, 0, 0.4}, rc, &table);
        double ra = realization(rc, true, true, h.stack(UTG) / h.pot(), h.cards[UTG][0], h.cards[UTG][1]);
        assert(near(u[UTG], ra * 0.6 / (ra * 0.6 + 0.4) * net - 2.5));
    }
    {  // uncovered: squeeze (dead money), multiway, all-in, and a table made for other bet sizes
        assert(!findRealization(&table, play(rng, {R, C, R, F, F, F, C, F})));  // UTG opens, HJ calls, CO squeezes
        assert(!findRealization(&table, play(rng, {R, C, F, F, F, C})));     // three-way
        assert(!findRealization(&table, play(rng, {F, F, F, R, F, R, R, R, C})));  // 5-bet all-in
        Config big;
        big.open = 3;
        assert(!findRealization(&table, play(rng, {R, F, F, F, F, C}, big)));
    }
    {  // solver: an empty table is bit-identical; a table changes the solve and is stored in the strategy
        Equity eq;
        assert(eq.loadHuTable(DATA_DIR "/hu_equity.bin"));
        SolverConfig sc;
        sc.rootFolds = 3, sc.flopBoards = 10;  // BTN, SB, BB
        Solver a(Config{}, sc, eq, 11), b(Config{}, sc, eq, 11), c(Config{}, sc, eq, 11);
        b.setRealizationTable(empty), c.setRealizationTable(table);
        a.run(3000), b.run(3000), c.run(3000);
        Strategy sa = a.average(), sb = b.average(), sc2 = c.average();
        assert(sameStrategy(sa, sb) && !sameStrategy(sa, sc2));
        assert(!sa.realizationTable() && sc2.realizationTable() && sc2.realizationTable()->entries.size() == table.entries.size());

        assert(sa.save("rp_test_a.strat") && sc2.save("rp_test_c.strat"));
        Strategy la, lc;
        assert(la.load("rp_test_a.strat") && lc.load("rp_test_c.strat"));
        assert(!la.realizationTable() && sameStrategy(la, sa));
        const RealizationTable* lt = lc.realizationTable();
        assert(lt && lt->source == "test" && lt->entries.size() == table.entries.size() &&
               lt->entries[5].rRaiser == table.entries[5].rRaiser && lt->entries[5].potBB == table.entries[5].potBB);
        std::filesystem::remove("rp_test_a.strat"), std::filesystem::remove("rp_test_c.strat");
    }
    std::puts("realization_payoff_test OK");
}
