// Strategy consistency audit: finds spots where a hand continues while a clearly better hand of the
// same shape folds ("fold 76s but call 74s").
// Usage: audit [--strategy file] [--mode continue|call] [--min-reach r] [--top N] [--self-check]
//   --strategy   default data/strategy_6max.bin
//   --mode       continue (default): flag a worse hand that continues (calls or raises) more than a better
//                one. call: count only the worse hand's calls, still against the better hand's calls +
//                raises: flags "flats while a better hand folds", not bluff-raise selection (K6s 3-bets while
//                K7s folds) or value raising (AKs 3-bets while ATs flats)
//   --min-reach  ignore classes the actor brings to the node less often than this (default 0.05)
//   --top        how many of the worst violations to list (default 20)
//   --self-check run on hand-built strategies and exit 0 if consistent ones pass and broken ones are flagged
//
// Checked pairs, per decision node, both classes reaching it at least --min-reach:
//   - same top card and suitedness, kickers k1 > k2, and straights(lo kicker) <= straights(hi kicker)
//     (so A5s continuing while A6s folds is fine: the wheel makes A5s the better hand);
//   - pocket pairs, higher vs lower.
// Violation: excess = continue(worse) - continue(better) > 0.3, continue = P(call) + P(raise);
// with --mode call the worse hand's raises don't count.
// Each violation is weighted by how often the node is reached over random deals (every seat's range
// carried down the tree, card removal ignored, as solve's strategyChange); score = sum weight * excess.
// A second score counts only nodes with weight >= 0.01: the spots a player actually meets, since the
// many deep, rarely reached nodes dominate the violation count.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "solver/cards.h"
#include "solver/strategy.h"
#include "solver/utility.h"

using namespace pf;

namespace {

const char* POS[] = {"UTG", "HJ", "CO", "BTN", "SB", "BB"};
constexpr double THRESHOLD = 0.3, COMMON = 0.01;  // COMMON: node weight for the "common spots" score

struct Violation {
    std::string history;
    int actor, worse, better, node;
    double weight, excess;
};

// (worse, better) class pairs to compare, built once: worse is the lower kicker / lower pair.
std::vector<std::pair<int, int>> comparisons() {
    std::vector<std::pair<int, int>> out;
    auto b = [](int r1, int r2, bool suited) { return bucket(Card{uint8_t(r1), 0}, Card{uint8_t(r2), uint8_t(suited ? 0 : 1)}); };
    for (int top = 0; top < 13; ++top)
        for (int k1 = 0; k1 < top; ++k1)
            for (int k2 = 0; k2 < k1; ++k2)
                for (bool s : {true, false}) {
                    Card hiA{uint8_t(top), 0}, hiB{uint8_t(k1), uint8_t(s ? 0 : 1)}, loB{uint8_t(k2), uint8_t(s ? 0 : 1)};
                    if (straights(hiA, loB) <= straights(hiA, hiB)) out.push_back({b(top, k2, s), b(top, k1, s)});
                }
    for (int p1 = 0; p1 < 13; ++p1)
        for (int p2 = 0; p2 < p1; ++p2) out.push_back({b(p2, p2, false), b(p1, p1, false)});
    return out;
}

struct Audit {
    const Strategy& s;
    double minReach;
    bool callOnly;
    std::vector<std::pair<int, int>> pairs = comparisons();
    std::array<std::array<double, NBUCKETS>, NPOS> range{};  // each seat's own reach per class
    std::vector<Act> hist;
    std::vector<Violation> found;

    Audit(const Strategy& st, double mr, bool co = false) : s(st), minReach(mr), callOnly(co) {}

    double cont(int node, int b, bool worse) const {
        return s.probs(node, b)[1] + (callOnly && worse ? 0 : s.probs(node, b)[2]);
    }

    void walk(int node, double reach) {
        int p = s.actor(node);
        auto& r = range[p];
        double tot = 0;
        bool forced = true;  // a seat forced to fold (every class folds) has nothing to audit
        for (int k = 0; k < NBUCKETS; ++k) {
            tot += comboCount(k) * r[k];
            forced &= s.probs(node, k)[0] >= 1 - 1e-6;
        }
        if (tot <= 0) return;
        if (!forced)
            for (auto [w, b] : pairs) {
                double ex = cont(node, w, true) - cont(node, b, false);
                if (ex > THRESHOLD && r[w] >= minReach && r[b] >= minReach) {
                    std::string h;
                    for (Act a : hist) h += "FCR"[int(a)];
                    found.push_back({h, p, w, b, node, reach, ex});
                }
            }
        for (int x = 0; x < 3; ++x) {
            int c = s.child(node, Act(x));
            if (c < 0) continue;
            double px = 0;
            for (int k = 0; k < NBUCKETS; ++k) px += comboCount(k) * r[k] * s.probs(node, k)[x];
            if (px <= 0) continue;
            auto saved = r;
            for (int k = 0; k < NBUCKETS; ++k) r[k] *= s.probs(node, k)[x];
            hist.push_back(Act(x));
            walk(c, reach * px / tot);
            hist.pop_back();
            r = saved;
        }
    }

    std::vector<Violation> run() {
        for (auto& r : range) r.fill(1);
        found.clear();
        walk(s.root(), 1);
        std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.weight * a.excess > b.weight * b.excess; });
        return found;
    }
};

double score(const std::vector<Violation>& v, double minWeight = 0) {
    double t = 0;
    for (auto& x : v)
        if (x.weight >= minWeight) t += x.weight * x.excess;
    return t;
}

bool flags(const std::vector<Violation>& v, const char* worse, const char* better, const std::string& hist = "") {
    for (auto& x : v)
        if (x.history == hist && bucketName(x.worse) == worse && bucketName(x.better) == better) return true;
    return false;
}

int selfCheck() {
    Strategy uniform;  // every class plays the same mix: nothing to flag
    auto u = Audit{uniform, 0.05}.run();

    Strategy broken;  // UTG opens 74s but folds 76s: flagged
    broken.probs(0, bucketFromName("76s")) = {1, 0, 0};
    broken.probs(0, bucketFromName("74s")) = {0, 0, 1};
    auto b = Audit{broken, 0.05}.run();

    Strategy wheel;  // UTG opens A5s and folds A6s: the wheel exception, not flagged
    wheel.probs(0, bucketFromName("A6s")) = {1, 0, 0};
    wheel.probs(0, bucketFromName("A5s")) = {0, 0, 1};
    auto w = Audit{wheel, 0.05}.run();

    Strategy vs;  // HJ facing UTG's open: K6s 3-bets as a bluff while K7s folds, and 74s flats while 76s folds
    int n = vs.nodeFor({Act::Raise});
    vs.probs(n, bucketFromName("K7s")) = {1, 0, 0};
    vs.probs(n, bucketFromName("K6s")) = {0, 0, 1};
    vs.probs(n, bucketFromName("76s")) = {1, 0, 0};
    vs.probs(n, bucketFromName("74s")) = {0, 1, 0};
    vs.probs(n, bucketFromName("AKs")) = {0, 0, 1};  // value 3-bet over a flat: consistent in both modes
    vs.probs(n, bucketFromName("ATs")) = {0, 1, 0};
    auto vc = Audit{vs, 0.05}.run(), vcall = Audit{vs, 0.05, true}.run();

    bool checks[] = {u.empty(), flags(b, "74s", "76s"), !flags(w, "A5s", "A6s"),
                     flags(vc, "K6s", "K7s", "R"), !flags(vcall, "K6s", "K7s", "R"),  // bluff: continue mode only
                     flags(vc, "74s", "76s", "R"), flags(vcall, "74s", "76s", "R"),   // flat: both modes
                     !flags(vc, "ATs", "AKs", "R"), !flags(vcall, "ATs", "AKs", "R")};  // value raise: neither
    const char* what[] = {"uniform strategy: no violations", "UTG opens 74s, folds 76s: flagged",
                          "UTG opens A5s, folds A6s: not flagged (wheel)", "K6s 3-bet / K7s fold: flagged in continue mode",
                          "K6s 3-bet / K7s fold: not flagged in call mode", "74s flat / 76s fold: flagged in continue mode",
                          "74s flat / 76s fold: flagged in call mode", "ATs flat / AKs 3-bet: not flagged in continue mode",
                          "ATs flat / AKs 3-bet: not flagged in call mode"};
    bool ok = true;
    for (int i = 0; i < 9; ++i) std::printf("%s  %s\n", checks[i] ? "ok  " : "FAIL", what[i]), ok &= checks[i];
    std::printf("%s\n", ok ? "self-check passed" : "SELF-CHECK FAILED");
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path = DATA_DIR "/strategy_6max.bin";
    double minReach = 0.05;
    int top = 20;
    bool callOnly = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--self-check") return selfCheck();
        if (i + 1 >= argc) return std::fprintf(stderr, "missing value for %s\n", a.c_str()), 2;
        const char* v = argv[++i];
        if (a == "--strategy") path = v;
        else if (a == "--min-reach") minReach = std::atof(v);
        else if (a == "--top") top = std::atoi(v);
        else if (a == "--mode" && (std::string(v) == "continue" || std::string(v) == "call")) callOnly = std::string(v) == "call";
        else return std::fprintf(stderr, "unknown option %s\n", a.c_str()), 2;
    }
    Strategy s;
    if (!s.load(path)) return std::fprintf(stderr, "cannot load %s\n", path.c_str()), 1;
    auto v = Audit{s, minReach, callOnly}.run();
    long common = std::count_if(v.begin(), v.end(), [](auto& x) { return x.weight >= COMMON; });
    std::printf("%s (mode %s, min reach %g, threshold %.1f)\n", path.c_str(), callOnly ? "call" : "continue", minReach, THRESHOLD);
    std::printf("  all nodes:           %6zu violations, weighted score %.6f\n", v.size(), score(v));
    std::printf("  node weight >= %.2f: %6ld violations, weighted score %.6f\n", COMMON, common, score(v, COMMON));
    for (int i = 0; i < top && i < int(v.size()); ++i) {
        auto& x = v[i];
        auto pw = s.probs(x.node, x.worse), pb = s.probs(x.node, x.better);
        std::printf("%2d. %-10s %-3s  %-4s f/c/r %3.0f/%3.0f/%3.0f  vs %-4s f/c/r %3.0f/%3.0f/%3.0f  excess %.2f  node weight %.5f\n",
                    i + 1, x.history.empty() ? "(root)" : x.history.c_str(), POS[x.actor], bucketName(x.worse).c_str(),
                    100 * pw[0], 100 * pw[1], 100 * pw[2], bucketName(x.better).c_str(), 100 * pb[0], 100 * pb[1],
                    100 * pb[2], x.excess, x.weight);
    }
}
