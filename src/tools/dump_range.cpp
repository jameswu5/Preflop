// Prints a strategy's (or a chart's) per-class probabilities at spots, for the writeup's range grids.
// Usage: dump_range --strategy file [--charts dir] SPOT...
//   SPOT: a ref chart file (*.txt: its spot line gives the history) or a history (F/C/R letters, "-" = root)
//   --charts: play the charts in dir over the strategy first (applyCharts), e.g. Anchored + ref/pto
// One line per (spot, class): label class raise call reach, where label is the chart's file stem or
// the history, and reach is the probability that the actor's own earlier actions bring the class
// there (Strategy::reach). A chart's own grid: --strategy Anchored --charts <its dir>.
#include <cstdio>
#include <filesystem>

#include "solver/range.h"
#include "solver/strategy.h"

using namespace pf;

int main(int argc, char** argv) {
    std::string stratPath, chartDir;
    std::vector<std::string> spots;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--strategy" && i + 1 < argc) stratPath = argv[++i];
        else if (a == "--charts" && i + 1 < argc) chartDir = argv[++i];
        else spots.push_back(a);
    }
    Strategy s;
    if (!s.load(stratPath)) return std::fprintf(stderr, "cannot load strategy '%s'\n", stratPath.c_str()), 1;
    if (std::string err; !chartDir.empty() && applyCharts(s, chartDir, nullptr, &err) < 0)
        return std::fprintf(stderr, "%s: %s\n", chartDir.c_str(), err.c_str()), 1;
    for (const std::string& spot : spots) {
        Chart c;
        bool isChart = spot.ends_with(".txt");
        std::string err, hist = spot == "-" ? "" : spot, label = spot;
        if (isChart) {
            if (!readChart(spot, c, err)) return std::fprintf(stderr, "%s: %s\n", spot.c_str(), err.c_str()), 1;
            hist = c.spot, label = std::filesystem::path(spot).stem().string();
        }
        std::vector<Act> h;
        int node = -1;
        if (!parseHistory(hist, h) || (node = s.nodeFor(h)) < 0)
            return std::fprintf(stderr, "%s: not a decision node\n", spot.c_str()), 1;
        auto reach = s.reach(h);
        for (int b = 0; b < NBUCKETS; ++b) {
            auto& p = s.probs(node, b);
            std::printf("%s %s %.4f %.4f %.4f\n", label.c_str(), bucketName(b).c_str(), p[int(Act::Raise)],
                        p[int(Act::Call)], reach[b]);
        }
    }
}
