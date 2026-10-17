// Compares a solved strategy with reference charts (docs/validation.md "Distance metric").
// Usage: validate [--strategy file] [--ref dir]
// Each ref/*.txt holds one spot:
//   # comment
//   spot: FFF              (action history from UTG, F/C/R; empty = UTG's open)
//   raise: AA,KK,A5s:0.5   (solver/range.h notation; fold is the remainder)
//   call: QQ-22
// Per spot: distance = sum over classes of w * |ours - ref| / 2 / sum w, with w = combos * our
// reach of the class (classes we never bring to the spot don't count). 0 = identical, 1 = disjoint.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "solver/range.h"
#include "solver/strategy.h"

using namespace pf;
namespace fs = std::filesystem;

struct Ref {
    std::string name, spot;
    RangeFreqs act[3] = {};  // fold derived, call, raise
    bool given[3] = {};
};

static bool readRef(const fs::path& p, Ref& r, std::string& err) {
    std::ifstream in(p);
    r.name = p.stem().string();
    bool haveSpot = false;
    int ln = 0;
    for (std::string line; std::getline(in, line);) {
        ++ln;
        line = line.substr(0, line.find('#'));
        auto c = line.find(':');
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        if (c == std::string::npos) return err = "line " + std::to_string(ln) + ": expected key: value", false;
        std::string key = line.substr(0, c), val = line.substr(c + 1);
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        val.erase(0, val.find_first_not_of(" \t"));
        val.erase(val.find_last_not_of(" \t\r") + 1);
        if (key == "spot") {
            r.spot = val, haveSpot = true;
        } else if (key == "raise" || key == "call") {
            int a = key == "raise" ? int(Act::Raise) : int(Act::Call);
            std::string e;
            if (!parseRange(val, r.act[a], &e)) return err = "line " + std::to_string(ln) + ": " + e, false;
            r.given[a] = true;
        } else {
            return err = "line " + std::to_string(ln) + ": unknown key '" + key + "'", false;
        }
    }
    if (!haveSpot) return err = "missing 'spot:' line", false;
    for (int b = 0; b < NBUCKETS; ++b) {
        float rest = 1 - r.act[1][b] - r.act[2][b];
        if (rest < -1e-4) return err = bucketName(b) + ": raise + call > 1", false;
        r.act[0][b] = std::max(rest, 0.0f);
    }
    return true;
}

int main(int argc, char** argv) {
    std::string stratPath = DATA_DIR "/strategy_6max.bin", refDir = REF_DIR;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--strategy") stratPath = argv[i + 1];
        else if (a == "--ref") refDir = argv[i + 1];
        else return std::fprintf(stderr, "unknown option %s\n", a.c_str()), 2;
    }
    Strategy s;
    if (!s.load(stratPath)) return std::fprintf(stderr, "cannot load %s\n", stratPath.c_str()), 1;
    std::vector<fs::path> files;
    if (fs::is_directory(refDir))
        for (auto& e : fs::directory_iterator(refDir))
            if (e.path().extension() == ".txt") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    if (files.empty()) return std::fprintf(stderr, "no reference files in %s\n", refDir.c_str()), 1;

    const char* actName[] = {"fold", "call", "raise"};
    double total = 0;
    int scored = 0, bad = 0;
    for (auto& f : files) {
        Ref r;
        std::string err;
        std::vector<Act> h;
        int node = -1;
        if (readRef(f, r, err) && !parseHistory(r.spot, h)) err = "bad spot '" + r.spot + "'";
        if (err.empty() && (node = s.nodeFor(h)) < 0) err = "spot '" + r.spot + "' is not a decision node";
        for (int a = 1; a < 3 && err.empty(); ++a)
            if (r.given[a] && !(s.tree().legal[node] >> a & 1)) err = std::string(actName[a]) + " is not legal at this spot";
        if (!err.empty()) {
            std::printf("%s: %s\n", f.filename().c_str(), err.c_str()), ++bad;
            continue;
        }
        auto reach = s.reach(h);
        double wsum = 0, dist = 0, ours[3] = {}, ref[3] = {};
        std::vector<std::pair<double, int>> diff;
        for (int b = 0; b < NBUCKETS; ++b) {
            double w = comboCount(b) * reach[b], d = 0;
            for (int a = 0; a < 3; ++a) {
                d += std::fabs(s.probs(node, b)[a] - r.act[a][b]) / 2;
                ours[a] += w * s.probs(node, b)[a], ref[a] += w * r.act[a][b];
            }
            wsum += w, dist += w * d;
            if (reach[b] > 1e-3) diff.push_back({d, b});
        }
        dist /= wsum;
        total += dist, ++scored;
        std::printf("%-16s spot %-8s distance %.3f   ", r.name.c_str(), r.spot.empty() ? "(root)" : r.spot.c_str(), dist);
        for (int a = 2; a >= 0; --a)
            if (s.tree().legal[node] >> a & 1) std::printf(" %s %.1f%% (ref %.1f%%)", actName[a], 100 * ours[a] / wsum, 100 * ref[a] / wsum);
        std::printf("\n    most different:");
        std::sort(diff.rbegin(), diff.rend());
        for (int i = 0; i < 5 && i < int(diff.size()) && diff[i].first > 0.05; ++i) {
            int b = diff[i].second;
            std::printf(" %s r%.0f/c%.0f (ref r%.0f/c%.0f)", bucketName(b).c_str(), 100 * s.probs(node, b)[2],
                        100 * s.probs(node, b)[1], 100 * r.act[2][b], 100 * r.act[1][b]);
        }
        std::printf("\n");
    }
    if (scored) std::printf("score %.4f (mean distance over %d spots)\n", total / scored, scored);
    return bad ? 1 : 0;
}
