#pragma once
// Fitted realization model (version 2, Phase 1): fills what a measured table lacks. Classes a spot's
// range never holds, and heads-up lines the solves don't cover (squeezes, cold calls of a 3-bet, cold
// 4-bets), get R from a per-class base times a context term:
//   log R = a_k + theta . phi,  phi = (ip, raiser, ip * raiser, spr < 2, 2 <= spr < 6, eqr - 1/2)
// with eqr the class's equity against the opponent's range at the line. Fitted by weighted least
// squares on log R over the measured cells with weights n, the a_k shrunk toward their mean.
#include <array>
#include <vector>

#include "solver/equity.h"
#include "solver/realization_table.h"
#include "solver/strategy.h"

namespace pf {

struct RealizationModel {
    static constexpr int NF = 6;
    std::array<double, NBUCKETS> base{};  // a_k
    std::array<double, NF> theta{};
    static std::array<double, NF> features(bool ip, bool raiser, double spr, double eqr);
    double predict(int cls, bool ip, bool raiser, double spr, double eqr) const;  // floored at 0.05, as realization()
};

// One measured (spot, role, class): R and its weight.
struct ModelCell {
    int cls;
    bool ip, raiser;
    double spr, eqr, r, w;
};

// `ridge`: pseudo-weight pulling each a_k toward the weighted mean of all of them, so classes with
// little weight stay near the average and classes with none sit at it. Cells with r <= 0 or w <= 0 are skipped.
RealizationModel fitRealizationModel(const std::vector<ModelCell>& cells, double ridge = 0.1);

// Per class: equity against `opp` (combo weights), every combo of the class counted equally and card
// removal exact. 0.5 where `opp` is empty. Needs e.loadHuTable().
std::array<double, NBUCKETS> classEquity(const Equity& e, const std::array<float, NCOMBOS>& opp);

// A table with every class of every heads-up flop line of s's tree filled (flopSpots(dead = true), ranges
// from s). The model is fitted on `measured` (cell weight n / the entry's total n per role, so every
// measured spot and role counts alike, and `ridge` is in those units). Measured classes shrink toward the model,
// R = (n R + tau N M) / (n + tau N) with N the role's total n; absent classes and unmeasured lines take M.
// Unmeasured lines come out with flops = 0 and n = 0. `fitted`, if given, receives the model.
RealizationTable fillRealizationTable(const RealizationTable& measured, const Strategy& s, const Equity& e,
                                      double tau = 0.002, double ridge = 0.1, RealizationModel* fitted = nullptr);

// How far two tables are apart, the fixed-point iteration's convergence metric (architecture.md,
// Phase 2). Entries match on their key (pot type, seats, pot, stack); within them every class weighs
// w = (n_a + n_b) / 2, so model-filled values (n = 0) don't count. Per pot type and role ([pot][0] = raiser,
// [1] = caller): the w-weighted mean and the max of |R_a - R_b| over classes with w > 0; overall: the
// w-weighted mean over everything.
struct RealizationDiff {
    std::array<std::array<double, 2>, 3> mean{}, max{}, weight{};
    double overall = 0;
    int matched = 0;  // entries found in both tables
};
RealizationDiff diffRealizationTables(const RealizationTable& a, const RealizationTable& b);

}  // namespace pf
