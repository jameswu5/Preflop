#include "solver/realization_model.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "solver/spots.h"

namespace pf {

std::array<double, RealizationModel::NF> RealizationModel::features(bool ip, bool raiser, double spr, double eqr) {
    return {double(ip), double(raiser), double(ip && raiser), double(spr < 2), double(spr >= 2 && spr < 6), eqr - 0.5};
}

double RealizationModel::predict(int cls, bool ip, bool raiser, double spr, double eqr) const {
    auto phi = features(ip, raiser, spr, eqr);
    double y = base[cls];
    for (int j = 0; j < NF; ++j) y += theta[j] * phi[j];
    return std::max(std::exp(y), 0.05);
}

// Solves A x = b in place (Gaussian elimination, partial pivoting). A is small and symmetric positive
// semi-definite; a tiny ridge on the diagonal keeps it solvable when a feature never varies.
template <int N> static std::array<double, N> solve(std::array<std::array<double, N>, N> A, std::array<double, N> b) {
    for (int i = 0; i < N; ++i) A[i][i] += 1e-9;
    for (int c = 0; c < N; ++c) {
        int p = c;
        for (int r = c + 1; r < N; ++r)
            if (std::abs(A[r][c]) > std::abs(A[p][c])) p = r;
        std::swap(A[c], A[p]), std::swap(b[c], b[p]);
        for (int r = c + 1; r < N; ++r) {
            double f = A[r][c] / A[c][c];
            for (int k = c; k < N; ++k) A[r][k] -= f * A[c][k];
            b[r] -= f * b[c];
        }
    }
    std::array<double, N> x{};
    for (int r = N - 1; r >= 0; --r) {
        double s = b[r];
        for (int k = r + 1; k < N; ++k) s -= A[r][k] * x[k];
        x[r] = s / A[r][r];
    }
    return x;
}

RealizationModel fitRealizationModel(const std::vector<ModelCell>& all, double ridge) {
    constexpr int NF = RealizationModel::NF;
    struct Row {
        int k;
        std::array<double, NF> phi;
        double y, w;
    };
    std::vector<Row> rows;
    for (const ModelCell& c : all)
        if (c.r > 0 && c.w > 0)
            rows.push_back({c.cls, RealizationModel::features(c.ip, c.raiser, c.spr, c.eqr), std::log(std::max(c.r, 0.05)), c.w});
    RealizationModel m;
    if (rows.empty()) return m;
    // Block coordinate descent on the joint least squares: bases given theta (closed form, shrunk toward
    // their mean), then theta given the bases (normal equations). Each step lowers the same objective.
    for (int it = 0; it < 500; ++it) {
        std::array<double, NBUCKETS> sy{}, sw{};
        double my = 0, mw = 0;
        for (const Row& r : rows) {
            double res = r.y;
            for (int j = 0; j < NF; ++j) res -= m.theta[j] * r.phi[j];
            sy[r.k] += r.w * res, sw[r.k] += r.w, my += r.w * res, mw += r.w;
        }
        double mu = my / mw, change = 0;
        for (int k = 0; k < NBUCKETS; ++k) {
            double a = (sy[k] + ridge * mu) / (sw[k] + ridge);
            change = std::max(change, std::abs(a - m.base[k])), m.base[k] = a;
        }
        std::array<std::array<double, NF>, NF> A{};
        std::array<double, NF> b{};
        for (const Row& r : rows)
            for (int i = 0; i < NF; ++i) {
                b[i] += r.w * r.phi[i] * (r.y - m.base[r.k]);
                for (int j = 0; j < NF; ++j) A[i][j] += r.w * r.phi[i] * r.phi[j];
            }
        auto theta = solve<NF>(A, b);
        for (int j = 0; j < NF; ++j) change = std::max(change, std::abs(theta[j] - m.theta[j]));
        m.theta = theta;
        if (change < 1e-10) break;
    }
    return m;
}

std::array<double, NBUCKETS> classEquity(const Equity& e, const std::array<float, NCOMBOS>& opp) {
    static const auto combos = [] {
        std::vector<std::pair<uint64_t, int>> c(NCOMBOS);  // card mask, class
        for (int a = 0; a < 52; ++a)
            for (int b = a + 1; b < 52; ++b)
                c[comboIndex(cardFromIndex(a), cardFromIndex(b))] = {1ull << a | 1ull << b, bucket(cardFromIndex(a), cardFromIndex(b))};
        return c;
    }();
    std::vector<int> ys;  // the opponent's combos
    for (int y = 0; y < NCOMBOS; ++y)
        if (opp[y] > 0) ys.push_back(y);
    std::array<double, NBUCKETS> num{}, den{};
    for (int x = 0; x < NCOMBOS; ++x) {
        uint64_t mx = combos[x].first;
        double n = 0, d = 0;
        for (int y : ys)
            if (!(combos[y].first & mx)) n += opp[y] * e.hu(x, y), d += opp[y];
        num[combos[x].second] += n, den[combos[x].second] += d;
    }
    std::array<double, NBUCKETS> eq;
    for (int k = 0; k < NBUCKETS; ++k) eq[k] = den[k] > 0 ? num[k] / den[k] : 0.5;
    return eq;
}

static bool sameKey(const RealizationEntry& e, const FlopSpot& s) {
    return e.pot == s.pot && e.raiser == s.raiser && e.caller == s.caller && std::abs(e.potBB - s.potBB) < 1e-9 &&
           std::abs(e.stackBB - s.stackBB) < 1e-9;
}

RealizationTable fillRealizationTable(const RealizationTable& measured, const Strategy& s, const Equity& e, double tau,
                                      double ridge, RealizationModel* fitted) {
    struct Line {
        FlopSpot spot;
        const RealizationEntry* entry = nullptr;
        std::array<std::array<double, NBUCKETS>, 2> eqr;  // [0] raiser, [1] caller
    };
    std::vector<Line> lines;
    std::map<std::vector<float>, std::array<double, NBUCKETS>> memo;  // ranges repeat across lines
    auto eqVs = [&](const post::ComboWeights& opp) -> const std::array<double, NBUCKETS>& {
        std::vector<float> key(opp.begin(), opp.end());
        auto it = memo.find(key);
        return it != memo.end() ? it->second : memo[key] = classEquity(e, opp);
    };
    for (const FlopSpot& fs : flopSpots(s.config(), true)) {
        Line l{fs};
        for (const RealizationEntry& m : measured.entries)
            if (sameKey(m, fs)) l.entry = &m;
        l.eqr[0] = eqVs(lineRange(s, fs.history, fs.caller));
        l.eqr[1] = eqVs(lineRange(s, fs.history, fs.raiser));
        lines.push_back(l);
    }
    // ponytail: lines with the same key (seats, pot type, pot and stack) collide in findRealization(),
    // which takes the first; their contexts differ only in eqr.
    std::vector<ModelCell> cells;
    for (const Line& l : lines) {
        if (!l.entry) continue;
        double spr = l.spot.stackBB / l.spot.potBB;
        for (int role = 0; role < 2; ++role) {
            auto& r = role ? l.entry->rCaller : l.entry->rRaiser;
            auto& n = role ? l.entry->nCaller : l.entry->nRaiser;
            double total = 0;
            for (float x : n) total += x;
            bool ip = (role ? l.spot.caller : l.spot.raiser) == l.spot.ip();
            for (int k = 0; k < NBUCKETS && total > 0; ++k)
                cells.push_back({k, ip, role == 0, spr, l.eqr[role][k], r[k], n[k] / total});
        }
    }
    RealizationModel m = fitRealizationModel(cells, ridge);
    if (fitted) *fitted = m;

    RealizationTable out;
    out.source = measured.source + " filled tau=" + std::to_string(tau) + " ridge=" + std::to_string(ridge);
    for (const Line& l : lines) {
        RealizationEntry x = l.entry ? *l.entry : RealizationEntry{};
        x.pot = l.spot.pot, x.raiser = l.spot.raiser, x.caller = l.spot.caller;
        x.potBB = l.spot.potBB, x.stackBB = l.spot.stackBB;
        double spr = l.spot.stackBB / l.spot.potBB;
        for (int role = 0; role < 2; ++role) {
            auto& r = role ? x.rCaller : x.rRaiser;
            auto& n = role ? x.nCaller : x.nRaiser;
            double total = 0;
            for (float v : n) total += v;
            bool ip = (role ? l.spot.caller : l.spot.raiser) == l.spot.ip();
            for (int k = 0; k < NBUCKETS; ++k) {
                double model = m.predict(k, ip, role == 0, spr, l.eqr[role][k]);
                r[k] = float(n[k] > 0 && r[k] > 0 ? (n[k] * r[k] + tau * total * model) / (n[k] + tau * total) : model);
            }
        }
        out.entries.push_back(x);
    }
    return out;
}

}  // namespace pf
