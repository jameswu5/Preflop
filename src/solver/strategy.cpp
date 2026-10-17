#include "solver/strategy.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>

#include "solver/cards.h"

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

Tree::Tree(const Config& cfg) {
    std::mt19937 rng(0);  // cards don't affect the betting tree
    addNode(*this, Hand(rng, cfg));
}

Strategy::Strategy(const Config& cfg) : cfg_(cfg), tree_(cfg), p_(size_t(tree_.size()) * NBUCKETS) {
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

const std::array<float, 3>& Strategy::probs(int node, int bucket) const { return p_[size_t(node) * NBUCKETS + bucket]; }
std::array<float, 3>& Strategy::probs(int node, int bucket) { return p_[size_t(node) * NBUCKETS + bucket]; }

// File: "PFST" u32 version, Config (raw), i32 nodes, then probs float[nodes][169][3].
// The tree itself is rebuilt from the Config and checked against the node count.
static constexpr uint32_t VERSION = 1;

bool Strategy::save(const std::string& path) const {
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        int32_t n = tree_.size();
        out.write("PFST", 4);
        out.write(reinterpret_cast<const char*>(&VERSION), sizeof VERSION);
        out.write(reinterpret_cast<const char*>(&cfg_), sizeof cfg_);
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
    int32_t n;
    if (!in.read(magic, 4) || std::memcmp(magic, "PFST", 4) || !in.read(reinterpret_cast<char*>(&version), sizeof version) ||
        version != VERSION || !in.read(reinterpret_cast<char*>(&cfg), sizeof cfg) ||
        !in.read(reinterpret_cast<char*>(&n), sizeof n))
        return false;
    Tree tree(cfg);
    if (n != tree.size()) return false;
    std::vector<std::array<float, 3>> p(size_t(n) * NBUCKETS);
    if (!in.read(reinterpret_cast<char*>(p.data()), p.size() * sizeof p[0]) || in.peek() != EOF) return false;
    cfg_ = cfg, tree_ = std::move(tree), p_ = std::move(p);
    return true;
}

}  // namespace pf
