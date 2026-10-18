// Compares a solved strategy with reference charts (docs/validation.md "Distance metric").
// Usage: validate [--strategy file] [--ref dir] [--only text] [--exclude text]
//   --only / --exclude: keep / drop reference files whose name contains `text` (hold-out checks)
// Each ref/*.txt holds one spot:
//   # comment
//   spot: FFF              (action history from UTG, F/C/R; empty = UTG's open)
//   raise: AA,KK,A5s:0.5   (solver/range.h notation; fold is the remainder)
//   call: QQ-22
//   category: vs_3bet      (optional: the score averages spots within each category, then
//                           averages the categories, so a category with many spots doesn't dominate)
//   scope: listed          (optional: score only the classes named in raise/call, for partial
//                           sources that list some hands; otherwise unlisted classes count as fold)
// Per spot: distance = sum over classes of w * |ours - ref| / 2 / sum w, with w = combos * our
// reach of the class (classes we never bring to the spot don't count). 0 = identical, 1 = disjoint.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>

#include "solver/range.h"
#include "solver/strategy.h"

using namespace pf;
namespace fs = std::filesystem;

int main(int argc, char** argv) {
    std::string stratPath = DATA_DIR "/strategy_6max.bin", refDir = REF_DIR, only, exclude;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--strategy") stratPath = argv[i + 1];
        else if (a == "--ref") refDir = argv[i + 1];
        else if (a == "--only") only = argv[i + 1];
        else if (a == "--exclude") exclude = argv[i + 1];
        else return std::fprintf(stderr, "unknown option %s\n", a.c_str()), 2;
    }
    Strategy s;
    if (!s.load(stratPath)) return std::fprintf(stderr, "cannot load %s\n", stratPath.c_str()), 1;
    std::vector<fs::path> files;
    if (fs::is_directory(refDir))
        for (auto& e : fs::directory_iterator(refDir))
            if (std::string n = e.path().filename().string(); e.path().extension() == ".txt" &&
                (only.empty() || n.find(only) != std::string::npos) &&
                (exclude.empty() || n.find(exclude) == std::string::npos))
                files.push_back(e.path());
    std::sort(files.begin(), files.end());
    if (files.empty()) return std::fprintf(stderr, "no reference files in %s\n", refDir.c_str()), 1;

    const char* actName[] = {"fold", "call", "raise"};
    std::map<std::string, std::pair<double, int>> byCat;  // category -> (sum of distances, spots)
    int scored = 0, bad = 0;
    for (auto& f : files) {
        Chart r;
        std::string err, name = f.stem().string();
        std::vector<Act> h;
        int node = -1;
        if (readChart(f.string(), r, err) && !parseHistory(r.spot, h)) err = "bad spot '" + r.spot + "'";
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
            if (r.listedOnly && !r.listed(b)) continue;
            double w = comboCount(b) * reach[b], d = 0;
            for (int a = 0; a < 3; ++a) {
                d += std::fabs(s.probs(node, b)[a] - r.act[a][b]) / 2;
                ours[a] += w * s.probs(node, b)[a], ref[a] += w * r.act[a][b];
            }
            wsum += w, dist += w * d;
            if (reach[b] > 1e-3) diff.push_back({d, b});
        }
        if (wsum <= 0) {
            std::printf("%s: no scored hand class reaches this spot\n", f.filename().c_str()), ++bad;
            continue;
        }
        dist /= wsum;
        byCat[r.category].first += dist, ++byCat[r.category].second, ++scored;
        std::printf("%-22s spot %-8s distance %.3f %s  ", name.c_str(), r.spot.empty() ? "(root)" : r.spot.c_str(), dist,
                    r.listedOnly ? "(listed hands)" : "");
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
    double score = 0;
    for (auto& [cat, v] : byCat) {
        std::printf("category %-10s %.4f over %d spots\n", cat.c_str(), v.first / v.second, v.second);
        score += v.first / v.second / byCat.size();
    }
    if (scored) std::printf("score %.4f (mean over %zu categories, %d spots)\n", score, byCat.size(), scored);
    return bad ? 1 : 0;
}
