#include "solver/strategy.h"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

#include "solver/cards.h"
#include "solver/range.h"

namespace pf {

static int addNode(Tree& t, const Hand& h) {
    int id = t.size();
    t.actor.push_back(int8_t(h.toAct()));
    t.legal.push_back(0);
    t.next.push_back({-1, -1, -1});
    for (Act a : h.legal()) {
        t.legal[id] |= 1 << int(a);
        Hand c = h;
        c.apply(a);
        if (!c.done()) {
            int k = addNode(t, c);
            t.next[id][int(a)] = k;
        }
    }
    return id;
}

bool parseHistory(const std::string& letters, std::vector<Act>& out) {
    out.clear();
    for (char c : letters) {
        if (c != 'F' && c != 'C' && c != 'R') return false;
        out.push_back(c == 'F' ? Act::Fold : c == 'C' ? Act::Call : Act::Raise);
    }
    return true;
}

Tree::Tree(const Config& cfg) {
    std::mt19937 rng(0);  // cards don't affect the betting tree
    addNode(*this, Hand(rng, cfg));
}

Strategy::Strategy(const Config& cfg, const RealizationConfig& rc)
    : cfg_(cfg), rc_(rc), tree_(cfg), p_(size_t(tree_.size()) * NBUCKETS) {
    for (int n = 0; n < tree_.size(); ++n) {
        int k = __builtin_popcount(tree_.legal[n]);
        for (int b = 0; b < NBUCKETS; ++b)
            for (int a = 0; a < 3; ++a) probs(n, b)[a] = tree_.legal[n] >> a & 1 ? 1.0f / k : 0.0f;
    }
}

int Strategy::nodeFor(const std::vector<Act>& history) const {
    int n = root();
    for (Act a : history)
        if (n < 0 || (n = child(n, a)) < 0) return -1;
    return n;
}

std::array<double, NBUCKETS> Strategy::reach(const std::vector<Act>& history) const {
    std::array<double, NBUCKETS> r;
    r.fill(1);
    int who = actor(nodeFor(history));
    for (int i = 0, m = root(); i < int(history.size()); m = child(m, history[i++]))
        if (actor(m) == who)
            for (int b = 0; b < NBUCKETS; ++b) r[b] *= probs(m, b)[int(history[i])];
    return r;
}

const std::array<float, 3>& Strategy::probs(int node, int bucket) const { return p_[size_t(node) * NBUCKETS + bucket]; }
std::array<float, 3>& Strategy::probs(int node, int bucket) { return p_[size_t(node) * NBUCKETS + bucket]; }

// File: "PFST" u32 version, Config (raw), RealizationConfig (raw), i32 nodes,
// then probs float[nodes][169][3].
// The tree itself is rebuilt from the Config and checked against the node count.
static constexpr uint32_t VERSION = 7;  // 7: RealizationConfig.oopCall/callerShrink

bool Strategy::save(const std::string& path) const {
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        int32_t n = tree_.size();
        out.write("PFST", 4);
        out.write(reinterpret_cast<const char*>(&VERSION), sizeof VERSION);
        out.write(reinterpret_cast<const char*>(&cfg_), sizeof cfg_);
        out.write(reinterpret_cast<const char*>(&rc_), sizeof rc_);
        out.write(reinterpret_cast<const char*>(&n), sizeof n);
        out.write(reinterpret_cast<const char*>(p_.data()), p_.size() * sizeof p_[0]);
        if (!out) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;  // never leave a half-written file
}

bool Strategy::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    char magic[4];
    uint32_t version;
    Config cfg;
    RealizationConfig rc;
    int32_t n;
    if (!in.read(magic, 4) || std::memcmp(magic, "PFST", 4) || !in.read(reinterpret_cast<char*>(&version), sizeof version) ||
        version != VERSION || !in.read(reinterpret_cast<char*>(&cfg), sizeof cfg) ||
        !in.read(reinterpret_cast<char*>(&rc), sizeof rc) ||
        !in.read(reinterpret_cast<char*>(&n), sizeof n))
        return false;
    Tree tree(cfg);
    if (n != tree.size()) return false;
    std::vector<std::array<float, 3>> p(size_t(n) * NBUCKETS);
    if (!in.read(reinterpret_cast<char*>(p.data()), p.size() * sizeof p[0]) || in.peek() != EOF) return false;
    cfg_ = cfg, rc_ = rc, tree_ = std::move(tree), p_ = std::move(p);
    return true;
}

int applyCharts(Strategy& s, const std::string& dir, std::vector<uint8_t>* locked, std::string* err) {
    namespace fs = std::filesystem;
    auto fail = [&](const std::string& e) {
        if (err) *err = e;
        return -1;
    };
    std::vector<fs::path> files;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".txt") files.push_back(e.path());
    if (ec) return fail(dir + ": " + ec.message());
    std::sort(files.begin(), files.end());
    std::vector<std::pair<int, Chart>> charts;  // checked before anything is written
    for (auto& f : files) {
        Chart c;
        std::string e;
        std::vector<Act> h;
        int node = -1;
        if (!readChart(f.string(), c, e)) return fail(f.filename().string() + ": " + e);
        if (!parseHistory(c.spot, h) || (node = s.nodeFor(h)) < 0)
            return fail(f.filename().string() + ": spot '" + c.spot + "' is not a decision node");
        for (int b = 0; b < NBUCKETS; ++b)
            for (int a = 0; a < 3 && (!c.listedOnly || c.listed(b)); ++a)
                if (c.act[a][b] > 0 && !(s.tree().legal[node] >> a & 1))
                    return fail(f.filename().string() + ": " + bucketName(b) + " plays an illegal action");
        charts.push_back({node, std::move(c)});
    }
    if (locked) locked->assign(size_t(s.tree().size()) * NBUCKETS, 0);
    for (auto& [node, c] : charts)
        for (int b = 0; b < NBUCKETS; ++b) {
            if (c.listedOnly && !c.listed(b)) continue;
            for (int a = 0; a < 3; ++a) s.probs(node, b)[a] = c.act[a][b];
            if (locked) (*locked)[size_t(node) * NBUCKETS + b] = 1;
        }
    return int(charts.size());
}

}  // namespace pf
