#include "solver/mccfr.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "solver/cards.h"

namespace pf {

Solver::Solver(const Config& cfg, const SolverConfig& sc, const Equity& eq, uint32_t seed)
    : cfg_(cfg), sc_(sc), eq_(eq), tree_(cfg), rng_(seed),
      regret_(size_t(tree_.size()) * NBUCKETS), stratSum_(regret_.size()) {}

std::array<float, 3> Solver::regretMatch(int node, int bucket) const {
    const auto& r = regret_[size_t(node) * NBUCKETS + bucket];
    uint8_t legal = tree_.legal[node];
    std::array<float, 3> s{};
    float sum = 0;
    for (int a = 0; a < 3; ++a)
        if (legal >> a & 1) sum += s[a] = std::max(r[a], 0.0f);
    if (sum > 0) {
        for (float& x : s) x /= sum;
    } else {
        int k = __builtin_popcount(legal);
        for (int a = 0; a < 3; ++a) s[a] = legal >> a & 1 ? 1.0f / k : 0.0f;
    }
    return s;
}

double Solver::traverse(const Hand& h, int node, int traverser) {
    if (h.done()) return utility(h, eq_, rng_, sc_.flopBoards, sc_.realization)[traverser];
    int p = tree_.actor[node];
    auto step = [&](Act a) {
        Hand c = h;
        c.apply(a);
        return traverse(c, tree_.next[node][int(a)], traverser);
    };
    if (p < sc_.rootFolds) return step(Act::Fold);

    int b = bucket(h.cards[p][0], h.cards[p][1]);
    auto sigma = regretMatch(node, b);
    size_t i = size_t(node) * NBUCKETS + b;
    if (p == traverser) {
        double v[3] = {}, ev = 0;
        for (int a = 0; a < 3; ++a)
            if (tree_.legal[node] >> a & 1) ev += sigma[a] * (v[a] = step(Act(a)));
        for (int a = 0; a < 3; ++a)
            if (tree_.legal[node] >> a & 1) regret_[i][a] += float(v[a] - ev);
        return ev;
    }
    for (int a = 0; a < 3; ++a) stratSum_[i][a] += sigma[a];
    float u = std::uniform_real_distribution<float>(0, 1)(rng_), acc = 0;
    int pick = 0;
    for (int a = 0; a < 3; ++a)
        if (sigma[a] > 0) {
            pick = a;  // last positive action absorbs float rounding
            if (u < (acc += sigma[a])) break;
        }
    return step(Act(pick));
}

void Solver::run(long iterations) {
    int seats = NPOS - sc_.rootFolds;
    for (long end = t_ + iterations; t_ < end;) {
        ++t_;
        Hand h(rng_, cfg_);
        traverse(h, 0, sc_.rootFolds + int(t_ % seats));
        if (sc_.discountEvery > 0 && t_ % sc_.discountEvery == 0) {
            float k = float(t_ / sc_.discountEvery), d = k / (k + 1);
            for (auto& r : regret_) for (float& x : r) x *= d;
            for (auto& s : stratSum_) for (float& x : s) x *= d;
        }
    }
}

Strategy Solver::average() const {
    Strategy s(cfg_, sc_.realization);
    for (int n = 0; n < tree_.size(); ++n)
        for (int b = 0; b < NBUCKETS; ++b) {
            const auto& ss = stratSum_[size_t(n) * NBUCKETS + b];
            float sum = ss[0] + ss[1] + ss[2];
            if (tree_.actor[n] < sc_.rootFolds) s.probs(n, b) = {1, 0, 0};
            else if (sum > 0) s.probs(n, b) = {ss[0] / sum, ss[1] / sum, ss[2] / sum};
        }
    return s;
}

// File: "PFCK" u32 version, Config, SolverConfig (raw; they hold only doubles/ints, no padding),
// i64 iteration, u32 rng-state length + mt19937 text state, then regret and strategy sums.
static constexpr uint32_t CK_VERSION = 1;
static_assert(sizeof(Config) % sizeof(double) == 0 && sizeof(RealizationConfig) % sizeof(double) == 0 &&
                  sizeof(SolverConfig) == 2 * sizeof(int) + sizeof(long) + sizeof(RealizationConfig),
              "configs are compared bytewise; keep them free of padding");

template <class T> static void put(std::ostream& o, const T& x) { o.write(reinterpret_cast<const char*>(&x), sizeof x); }
template <class T> static bool get(std::istream& i, T& x) { return bool(i.read(reinterpret_cast<char*>(&x), sizeof x)); }

bool Solver::saveCheckpoint(const std::string& path) const {
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        std::ostringstream rs;
        rs << rng_;
        std::string r = rs.str();
        out.write("PFCK", 4);
        put(out, CK_VERSION), put(out, cfg_), put(out, sc_), put(out, int64_t(t_)), put(out, uint32_t(r.size()));
        out.write(r.data(), r.size());
        out.write(reinterpret_cast<const char*>(regret_.data()), regret_.size() * sizeof regret_[0]);
        out.write(reinterpret_cast<const char*>(stratSum_.data()), stratSum_.size() * sizeof stratSum_[0]);
        if (!out) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;  // never leave a half-written checkpoint
}

bool Solver::loadCheckpoint(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    char magic[4];
    uint32_t version, rlen;
    Config cfg;
    SolverConfig sc;
    int64_t t;
    if (!in.read(magic, 4) || std::memcmp(magic, "PFCK", 4) || !get(in, version) || version != CK_VERSION ||
        !get(in, cfg) || !get(in, sc) || !get(in, t) || !get(in, rlen) || rlen > 1 << 16)
        return false;
    sc.realization = sc_.realization;  // R may differ: warm start
    if (std::memcmp(&cfg, &cfg_, sizeof cfg) || std::memcmp(&sc, &sc_, sizeof sc)) return false;
    std::string r(rlen, '\0');
    std::mt19937 rng;
    if (!in.read(r.data(), rlen) || !(std::istringstream(r) >> rng)) return false;
    std::vector<std::array<float, 3>> regret(regret_.size()), stratSum(stratSum_.size());
    if (!in.read(reinterpret_cast<char*>(regret.data()), regret.size() * sizeof regret[0]) ||
        !in.read(reinterpret_cast<char*>(stratSum.data()), stratSum.size() * sizeof stratSum[0]) || in.peek() != EOF)
        return false;
    t_ = long(t), rng_ = rng, regret_ = std::move(regret), stratSum_ = std::move(stratSum);
    return true;
}

}  // namespace pf
