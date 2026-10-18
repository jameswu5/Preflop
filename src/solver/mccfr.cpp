#include "solver/mccfr.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include "solver/cards.h"

namespace pf {

Solver::Solver(const Config& cfg, const SolverConfig& sc, const Equity& eq, uint32_t seed)
    : cfg_(cfg), sc_(sc), eq_(eq), tree_(cfg), rng_(seed),
      regret_(size_t(tree_.size()) * NBUCKETS), stratSum_(regret_.size()), terminal_(size_t(tree_.size()) * 3) {
    std::mt19937 rng(0);  // cards don't affect the betting
    auto walk = [&](auto& self, const Hand& h, int node) -> void {
        for (int a = 0; a < 3; ++a)
            if (tree_.legal[node] >> a & 1) {
                Hand c = h;
                c.apply(Act(a));
                if (c.done()) terminal_[node * 3 + a] = Terminal(c);
                else self(self, c, tree_.next[node][a]);
            }
    };
    walk(walk, Hand(rng, cfg_), 0);
}

// Table access: plain when single-threaded, relaxed atomics when threads share the tables.
static float load(const float& x, bool shared) {
    return shared ? std::atomic_ref<float>(const_cast<float&>(x)).load(std::memory_order_relaxed) : x;
}
static void add(float& x, float v, bool shared) {
    if (shared) std::atomic_ref<float>(x).fetch_add(v, std::memory_order_relaxed);
    else x += v;
}

std::array<float, 3> Solver::regretMatch(int node, int bucket, bool shared) const {
    const auto& r = regret_[size_t(node) * NBUCKETS + bucket];
    uint8_t legal = tree_.legal[node];
    std::array<float, 3> s{};
    float sum = 0;
    for (int a = 0; a < 3; ++a)
        if (legal >> a & 1) sum += s[a] = std::max(load(r[a], shared), 0.0f);
    if (sum > 0) {
        for (float& x : s) x /= sum;
    } else {
        int k = __builtin_popcount(legal);
        for (int a = 0; a < 3; ++a) s[a] = legal >> a & 1 ? 1.0f / k : 0.0f;
    }
    return s;
}

double Solver::terminal(Deal& d, int edge, int traverser, std::mt19937& rng) {
    const Terminal& t = terminal_[edge];
    std::array<double, NPOS> eq{};
    if (__builtin_popcount(t.live) > 1) {
        auto& cache = d.cache;
        int ai = t.allIn;
        if (!(cache.valid[ai] >> t.live & 1)) {
            std::array<bool, NPOS> live;
            for (int p = 0; p < NPOS; ++p) live[p] = t.live >> p & 1;
            eq_.compute(d.cards, live, rng, ai ? 1 : sc_.flopBoards, cache.eq[ai][t.live]);
            cache.valid[ai] |= uint64_t(1) << t.live;
        }
        eq = cache.eq[ai][t.live];
    }
    return payoffs(t, d.cards, eq, sc_.realization)[traverser];
}

double Solver::traverse(Deal& d, int node, int traverser, std::mt19937& rng, bool shared) {
    int p = tree_.actor[node];
    auto step = [&](int a) {
        int c = tree_.next[node][a];
        return c < 0 ? terminal(d, node * 3 + a, traverser, rng) : traverse(d, c, traverser, rng, shared);
    };
    if (p < sc_.rootFolds) return step(int(Act::Fold));

    int b = d.bucket[p];
    auto sigma = regretMatch(node, b, shared);
    size_t i = size_t(node) * NBUCKETS + b;
    if (p == traverser) {
        double v[3] = {}, ev = 0;
        for (int a = 0; a < 3; ++a)
            if (tree_.legal[node] >> a & 1) ev += sigma[a] * (v[a] = step(a));
        for (int a = 0; a < 3; ++a)
            if (tree_.legal[node] >> a & 1) add(regret_[i][a], float(v[a] - ev) * d.weight, shared);
        return ev;
    }
    for (int a = 0; a < 3; ++a) add(stratSum_[i][a], sigma[a] * d.weight, shared);
    float u = std::uniform_real_distribution<float>(0, 1)(rng), acc = 0;
    int pick = 0;
    for (int a = 0; a < 3; ++a)
        if (sigma[a] > 0) {
            pick = a;  // last positive action absorbs float rounding
            if (u < (acc += sigma[a])) break;
        }
    return step(pick);
}

void Solver::iterate(long t, std::mt19937& rng, bool shared) {
    Deal d;  // eq arrays are only read once marked valid, so no need to zero them
    d.cards = Hand(rng, cfg_).cards;
    for (int p = 0; p < NPOS; ++p) d.bucket[p] = bucket(d.cards[p][0], d.cards[p][1]);
    d.weight = weight(t);
    traverse(d, 0, sc_.rootFolds + int(t % (NPOS - sc_.rootFolds)), rng, shared);
}

void Solver::rescale(float factor) {
    if (factor != 1)
        for (auto* table : {&regret_, &stratSum_})
            for (auto& r : *table)
                for (float& x : r) x *= factor;
}

void Solver::run(long iterations, int threads) {
    if (threads <= 1) {
        for (long end = t_ + iterations; t_ < end;) iterate(++t_, rng_, false);
        return;
    }
    std::vector<std::thread> pool;
    for (int w = 0; w < threads; ++w)
        pool.emplace_back([this, iterations, threads, w, t0 = t_, seed = rng_()] {
            std::mt19937 rng(seed);
            for (long i = w; i < iterations; i += threads) iterate(t0 + 1 + i, rng, true);
        });
    for (auto& th : pool) th.join();
    t_ += iterations;
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
// 6: RealizationConfig.oopCall/callerShrink; 7: tables kept at scale(t) (version 6 holds them at 1);
// 8: RealizationConfig.ipCall/domPot (older versions load with their defaults).
static constexpr uint32_t CK_VERSION = 8;
static_assert(sizeof(Config) % sizeof(double) == 0 && sizeof(RealizationConfig) % sizeof(double) == 0 &&
                  sizeof(SolverConfig) == 2 * sizeof(int) + sizeof(long) + sizeof(RealizationConfig),
              "configs are compared bytewise; keep them free of padding");

template <class T> static void put(std::ostream& o, const T& x) { o.write(reinterpret_cast<const char*>(&x), sizeof x); }
template <class T> static bool get(std::istream& i, T& x) { return bool(i.read(reinterpret_cast<char*>(&x), sizeof x)); }

void Solver::resetAverage(long memoryBlocks) {
    long t = std::min(std::max(t_, loadedT_), memoryBlocks * std::max(sc_.discountEvery, 1L));
    std::fill(stratSum_.begin(), stratSum_.end(), std::array<float, 3>{});
    rescale(scale(t) / scale(t_));
    t_ = t;
}

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
    if (!in.read(magic, 4) || std::memcmp(magic, "PFCK", 4) || !get(in, version) || version < 6 || version > CK_VERSION ||
        !get(in, cfg) ||
        !in.read(reinterpret_cast<char*>(&sc), version < 8 ? offsetof(SolverConfig, realization) + REALIZATION_V1_SIZE : sizeof sc) ||
        !get(in, t) || !get(in, rlen) || rlen > 1 << 16)
        return false;
    bool newR = std::memcmp(&sc.realization, &sc_.realization, sizeof sc.realization) != 0;
    sc.realization = sc_.realization;  // R may differ: warm start
    sc.flopBoards = sc_.flopBoards;    // estimation precision, not part of the game
    if (std::memcmp(&cfg, &cfg_, sizeof cfg) || std::memcmp(&sc, &sc_, sizeof sc)) return false;
    std::string r(rlen, '\0');
    std::mt19937 rng;
    if (!in.read(r.data(), rlen) || !(std::istringstream(r) >> rng)) return false;
    std::vector<std::array<float, 3>> regret(regret_.size()), stratSum(stratSum_.size());
    if (!in.read(reinterpret_cast<char*>(regret.data()), regret.size() * sizeof regret[0]) ||
        !in.read(reinterpret_cast<char*>(stratSum.data()), stratSum.size() * sizeof stratSum[0]) || in.peek() != EOF)
        return false;
    t_ = loadedT_ = long(t), rng_ = rng, regret_ = std::move(regret), stratSum_ = std::move(stratSum);
    if (version < 7) rescale(scale(t_));
    if (newR) resetAverage();
    return true;
}

}  // namespace pf
