// The fitted realization model on synthetic tables: it recovers a known model, and filling a table
// with holes puts back the values that model gives, on the measured lines and the unmeasured ones.
#undef NDEBUG  // the build defaults to Release; keep asserts on
#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>

#include "solver/realization_model.h"
#include "solver/spots.h"

using namespace pf;

int main() {
    std::mt19937 rng(5);
    std::uniform_real_distribution<> u(0, 1);
    RealizationModel truth;
    for (double& a : truth.base) a = u(rng) - 0.5;
    truth.theta = {0.2, 0.25, -0.1, -0.3, -0.15, 0.8};

    {  // exact data: the fit recovers the model; a class with no data sits at the mean base
        std::vector<ModelCell> cells;
        for (int i = 0; i < 20000; ++i) {
            int k = 1 + int(rng() % (NBUCKETS - 1));  // class 0 (AA) never seen
            bool ip = rng() & 1, raiser = rng() & 1;
            double spr = 0.5 + 15 * u(rng), eqr = 0.2 + 0.6 * u(rng);
            cells.push_back({k, ip, raiser, spr, eqr, truth.predict(k, ip, raiser, spr, eqr), 0.1 + u(rng)});
        }
        RealizationModel m = fitRealizationModel(cells, 1e-9);
        for (int j = 0; j < RealizationModel::NF; ++j) assert(std::abs(m.theta[j] - truth.theta[j]) < 1e-6);
        for (int k = 1; k < NBUCKETS; ++k) assert(std::abs(m.base[k] - truth.base[k]) < 1e-6);
        double mean = 0;
        for (int k = 1; k < NBUCKETS; ++k) mean += m.base[k];
        m = fitRealizationModel(cells, 1);  // shrinkage: unseen AA at the (weighted) mean of the bases
        assert(std::abs(m.base[0] - mean / (NBUCKETS - 1)) < 0.05);
    }
    {  // fill: a measured table from `truth` with holes; every line of the tree comes back at `truth`
        Equity e;
        assert(e.loadHuTable(DATA_DIR "/hu_equity.bin"));
        Strategy s;  // uniform: every class in every range
        auto eqr = [&](const FlopSpot& fs, int role) {
            return classEquity(e, lineRange(s, fs.history, role ? fs.raiser : fs.caller));
        };
        RealizationTable measured;
        for (const FlopSpot& fs : flopSpots(s.config())) {
            RealizationEntry x;
            x.pot = fs.pot, x.raiser = fs.raiser, x.caller = fs.caller, x.potBB = fs.potBB, x.stackBB = fs.stackBB, x.flops = 5;
            for (int role = 0; role < 2; ++role) {
                auto q = eqr(fs, role);
                bool ip = (role ? fs.caller : fs.raiser) == fs.ip();
                for (int k = 0; k < NBUCKETS; ++k) {
                    if (rng() % 5 == 0) continue;  // a hole: class absent, n = 0, R = 0
                    (role ? x.rCaller : x.rRaiser)[k] = float(truth.predict(k, ip, role == 0, fs.stackBB / fs.potBB, q[k]));
                    (role ? x.nCaller : x.nRaiser)[k] = float(0.5 + u(rng));
                }
            }
            measured.entries.push_back(x);
        }
        RealizationModel m;
        RealizationTable filled = fillRealizationTable(measured, s, e, 0.002, 1e-9, &m);
        auto all = flopSpots(s.config(), true);
        assert(filled.entries.size() == all.size() && all.size() > measured.entries.size());
        int unmeasured = 0;
        for (size_t i = 0; i < all.size(); ++i) {
            const FlopSpot& fs = all[i];
            const RealizationEntry& x = filled.entries[i];
            assert(x.raiser == fs.raiser && x.caller == fs.caller && x.potBB == fs.potBB);
            unmeasured += x.flops == 0;
            for (int role = 0; role < 2; ++role) {
                auto q = eqr(fs, role);
                bool ip = (role ? fs.caller : fs.raiser) == fs.ip();
                for (int k = 0; k < NBUCKETS; ++k) {
                    double want = truth.predict(k, ip, role == 0, fs.stackBB / fs.potBB, q[k]);
                    assert(std::abs((role ? x.rCaller : x.rRaiser)[k] - want) < 1e-4 * want);
                }
            }
        }
        std::printf("%zu measured lines, %d filled from the model alone\n", measured.entries.size(), unmeasured);
        assert(unmeasured == int(all.size() - measured.entries.size()));

        // A squeeze now has an entry: UTG opens, HJ calls, CO squeezes, the rest fold, UTG calls.
        Hand h(rng);
        for (Act a : {Act::Raise, Act::Call, Act::Raise, Act::Fold, Act::Fold, Act::Fold, Act::Call, Act::Fold}) h.apply(a);
        const RealizationEntry* x = findRealization(&filled, h);
        assert(x && x->flops == 0 && x->raiser == CO && x->caller == UTG && !findRealization(&measured, h));
    }
    {  // diff: n-weighted |dR| per pot type and role, model-filled classes (n = 0) ignored
        RealizationEntry x;
        x.pot = PotType::ThreeBet, x.raiser = CO, x.caller = BTN, x.potBB = 20, x.stackBB = 80;
        x.rRaiser.fill(1), x.rCaller.fill(1), x.nRaiser.fill(1), x.nCaller.fill(1);
        x.nCaller[7] = 0, x.rCaller[7] = 5;  // filled class: never counts
        RealizationEntry y = x;
        y.rRaiser[0] = 1.3, y.nRaiser[0] = 3;  // w = 2, |dR| = 0.3
        y.rCaller[7] = 9;
        RealizationEntry other = x;  // no partner in b: ignored
        other.potBB = 21, other.rRaiser.fill(7);
        RealizationTable a, b;
        a.entries = {x, other}, b.entries = {y};
        RealizationDiff d = diffRealizationTables(a, b);
        double wR = 168 + 2, wC = 168;  // raiser: 168 classes at w 1, AA at w 2; caller: 168 classes
        assert(d.matched == 1);
        assert(std::abs(d.mean[1][0] - 2 * 0.3 / wR) < 1e-6 && std::abs(d.max[1][0] - 0.3) < 1e-6);
        assert(d.mean[1][1] == 0 && d.max[1][1] == 0 && d.weight[1][1] == wC && d.weight[0][0] == 0);
        assert(std::abs(d.overall - 2 * 0.3 / (wR + wC)) < 1e-6);
        assert(diffRealizationTables(a, a).overall == 0);
    }
    std::puts("realization_model_test OK");
}
