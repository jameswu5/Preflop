// Exploitability trace of heads-up 10bb push/fold (the solver_test game): exact exploitability in
// bb/hand at log-spaced iteration counts, for linear averaging (SolverConfig default discountEvery)
// and uniform averaging (discountEvery 0), several seeds each, one thread per run (bit-reproducible).
// Usage: pushfold_trace [--max 1e7] [--seeds 3] [--per-decade 4] [--out file.dat] [--strategy file]
//   --out       whitespace table: iters, linear_s1.., uniform_s1.., linear_mean, uniform_mean, uniform
//               (the last column is the uniform strategy's exploitability, constant)
//   --strategy  final average of linear seed 1, for the range grids
#include <cmath>
#include <cstdio>
#include <fstream>
#include <thread>

#include "solver/cards.h"
#include "solver/mccfr.h"

using namespace pf;

int main(int argc, char** argv) {
    double maxIters = 1e7;
    int seeds = 3, perDecade = 4;
    std::string out = "pushfold_expl.dat", stratOut, table = DATA_DIR "/hu_equity.bin";
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i], v = argv[i + 1];
        if (a == "--max") maxIters = std::stod(v);
        else if (a == "--seeds") seeds = std::stoi(v);
        else if (a == "--per-decade") perDecade = std::stoi(v);
        else if (a == "--out") out = v;
        else if (a == "--strategy") stratOut = v;
        else if (a == "--equity") table = v;
        else return std::fprintf(stderr, "unknown option %s\n", a.c_str()), 2;
    }
    Equity eq;
    std::vector<float> hu(size_t(NCOMBOS) * NCOMBOS);
    if (!eq.loadHuTable(table) ||
        !std::ifstream(table, std::ios::binary).read(reinterpret_cast<char*>(hu.data()), hu.size() * 4))
        return std::fprintf(stderr, "cannot load %s\n", table.c_str()), 1;
    Config pf10;
    pf10.stack = pf10.open = pf10.sbOpen = 10;
    pf10.rake = 0;
    std::vector<long> points;
    for (int i = 0;; ++i) {
        long n = std::lround(std::pow(10, 3 + double(i) / perDecade));
        if (n > maxIters * 1.0001) break;
        points.push_back(n);
    }
    int runs = 2 * seeds;
    std::vector<std::vector<double>> expl(runs, std::vector<double>(points.size()));
    double uniform = pushFoldExploitability(Strategy(pf10), hu, 10);
    std::vector<std::thread> pool;
    for (int r = 0; r < runs; ++r)
        pool.emplace_back([&, r] {
            SolverConfig sc;
            sc.rootFolds = 4;
            if (r >= seeds) sc.discountEvery = 0;
            Solver s(pf10, sc, eq, uint32_t(r % seeds + 1));
            for (size_t i = 0; i < points.size(); ++i) {
                s.run(points[i] - s.iterations(), 1);
                Strategy avg = s.average();
                expl[r][i] = pushFoldExploitability(avg, hu, 10);
                if (r == 0 && i + 1 == points.size() && !stratOut.empty()) avg.save(stratOut);
            }
        });
    for (auto& t : pool) t.join();

    std::ofstream f(out);
    f << "iters";
    for (const char* v : {"linear", "uniform"})
        for (int s = 1; s <= seeds; ++s) f << ' ' << v << "_s" << s;
    f << " linear_mean uniform_mean uniform\n";
    for (size_t i = 0; i < points.size(); ++i) {
        double m[2] = {};
        f << points[i];
        for (int r = 0; r < runs; ++r) f << ' ' << expl[r][i], m[r / seeds] += expl[r][i] / seeds;
        f << ' ' << m[0] << ' ' << m[1] << ' ' << uniform << '\n';
    }
    std::printf("uniform %.4f; at %ld iterations: linear %.5f, uniform averaging %.5f bb/hand\n", uniform, points.back(),
                expl[0].back(), expl[seeds].back());
}
