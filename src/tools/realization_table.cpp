// Builds the measured realization table (docs/architecture.md, "Version 2", Phase 1): for every
// heads-up flop spot of the tree (flopSpots) and every flop, solve postflop with both players' ranges
// along the line, and aggregate R per role and class over the flops as a ratio of sums.
// Usage: realization_table [--strategy file] [--charts dir] [--out file] [--records file] [--iters N]
//                          [--expl X] [--threads N] [--jobs N] [--flops N] [--spots N]
//   --strategy  ranges from this solve (default data/strategy_anchored.bin)
//   --charts    play the charts in dir over the strategy first (applyCharts), e.g. ref
//   --records   one line per solved (spot, flop), appended as it finishes (default <out>.records).
//               A rerun skips the pairs already there, so an interrupted run resumes; delete the file
//               to start over (it must come from the same strategy and settings).
//   --threads   solver threads per flop (post::Options::threads, default 1)
//   --jobs N    flops solved at once (default 1); only with --threads 1
//   --flops N   a sample of N canonical flops (sampleFlops; default 0 = all 1755)
//   --spots N   only the first N spots (testing)
// Fill mode: realization_table --fill measured.bin [--strategy file] [--charts dir] [--out file] [--tau X] [--ridge X]
//   fits the model of realization_model.h on a measured table and writes a table with every class of
//   every heads-up flop line filled (fillRealizationTable); ranges from the strategy, as when measuring.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "postflop/postflop.h"
#include "solver/flops.h"
#include "solver/realization_model.h"
#include "solver/spots.h"

using namespace pf;

struct Flop {
    std::array<int, 3> cards;
    std::string text;  // "2c7dAs"
    double weight;     // flops it stands for (CanonFlop::count)
};

// Every k-th canonical flop (k = 1755 / n, rounded down), weighted by its count; all of them for n <= 0.
// ponytail: a systematic sample over the card-ordered list; a stratified or importance sample if
// the budget stays this tight.
static std::vector<Flop> sampleFlops(int n) {
    const auto& all = canonicalFlops();
    size_t step = n > 0 ? std::max<size_t>(1, all.size() / n) : 1;
    std::vector<Flop> out;
    for (size_t i = 0; i < all.size() && (n <= 0 || int(out.size()) < n); i += step) {
        Flop f{all[i].cards, "", double(all[i].count)};
        for (int c : f.cards) {
            Card x = cardFromIndex(c);
            f.text += "23456789TJQKA"[x.rank], f.text += "cdhs"[x.suit];
        }
        out.push_back(f);
    }
    return out;
}

// ponytail: one bet size per street for every pot type; per-pot-type menus once the solver's timing is known.
static post::TreeConfig tree(const FlopSpot& s) {
    post::TreeConfig t;
    t.pot = s.potBB, t.stack = s.stackBB;
    t.sizes[0] = {{0.33}, {3}}, t.sizes[1] = {{0.75}, {3}}, t.sizes[2] = {{0.75}, {3}};
    return t;
}

static std::string historyText(const std::vector<Act>& h) {
    std::string s;
    for (Act a : h) s += "FCR"[int(a)];
    return s;
}

// Record line: history flop exploitability iterations, then ev, eq, n per role (raiser, caller) and
// class, as RealizationAccum sums for that flop alone (multiplicity 1), in hex floats so they are exact.
using Sums = std::array<std::array<double, NBUCKETS>, 6>;  // ev[0], ev[1], eq[0], eq[1], n[0], n[1]

static std::map<std::string, Sums> readRecords(const std::string& path) {
    std::map<std::string, Sums> out;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        std::istringstream ls(line);
        std::string hist, flop;
        double expl;
        int iters;
        Sums s;
        bool ok = bool(ls >> hist >> flop >> expl >> iters);
        for (auto& row : s)
            for (double& x : row) {
                std::string tok;
                ok = ok && (ls >> tok);
                if (ok) x = std::strtod(tok.c_str(), nullptr);
            }
        if (ok) out[hist + " " + flop] = s;  // a half-written last line (interrupted run) is dropped
    }
    return out;
}

int main(int argc, char** argv) {
    std::string stratPath = DATA_DIR "/strategy_anchored.bin", chartDir, out = "realization_table.bin", recPath, fillPath;
    double tau = 0.002, ridge = 0.1;
    post::Options opt;
    opt.threads = 1;
    int jobs = 1, nFlops = 0;
    size_t maxSpots = size_t(-1);
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        bool more = i + 1 < argc;
        if (a == "--strategy" && more) stratPath = argv[++i];
        else if (a == "--charts" && more) chartDir = argv[++i];
        else if (a == "--out" && more) out = argv[++i];
        else if (a == "--records" && more) recPath = argv[++i];
        else if (a == "--iters" && more) opt.maxIters = std::stoi(argv[++i]);
        else if (a == "--expl" && more) opt.targetExpl = std::stod(argv[++i]);
        else if (a == "--threads" && more) opt.threads = std::stoi(argv[++i]);
        else if (a == "--jobs" && more) jobs = std::stoi(argv[++i]);
        else if (a == "--flops" && more) nFlops = std::stoi(argv[++i]);
        else if (a == "--fill" && more) fillPath = argv[++i];
        else if (a == "--tau" && more) tau = std::stod(argv[++i]);
        else if (a == "--ridge" && more) ridge = std::stod(argv[++i]);
        else if (a == "--spots" && more) maxSpots = std::stoul(argv[++i]);
        else return std::fprintf(stderr, "unknown argument '%s'\n", a.c_str()), 1;
    }
    if (recPath.empty()) recPath = out + ".records";
    if (jobs < 1 || (jobs > 1 && opt.threads != 1))
        return std::fprintf(stderr, "--jobs > 1 needs --threads 1: parallel across flops or inside the solver, not both\n"), 1;
    Strategy s;
    if (!s.load(stratPath)) return std::fprintf(stderr, "cannot load strategy '%s'\n", stratPath.c_str()), 1;
    if (std::string err; !chartDir.empty() && applyCharts(s, chartDir, nullptr, &err) < 0)
        return std::fprintf(stderr, "%s: %s\n", chartDir.c_str(), err.c_str()), 1;

    if (!fillPath.empty()) {
        RealizationTable measured;
        Equity e;
        RealizationModel m;
        if (std::string err; !loadRealizationTable(fillPath, measured, &err)) return std::fprintf(stderr, "%s\n", err.c_str()), 1;
        if (!e.loadHuTable(DATA_DIR "/hu_equity.bin")) return std::fprintf(stderr, "cannot load hu_equity.bin\n"), 1;
        RealizationTable filled = fillRealizationTable(measured, s, e, tau, ridge, &m);
        std::printf("theta (ip raiser ip*raiser spr<2 spr2-6 eqr-0.5):");
        for (double t : m.theta) std::printf(" %.4f", t);
        std::printf("\n%zu measured entries, %zu lines filled\n", measured.entries.size(), filled.entries.size());
        if (std::string err; !saveRealizationTable(out, filled, &err)) return std::fprintf(stderr, "%s\n", err.c_str()), 1;
        std::printf("wrote %s\n", out.c_str());
        return 0;
    }

    const auto FLOPS = sampleFlops(nFlops);
    auto spots = flopSpots(s.config());
    if (spots.size() > maxSpots) spots.resize(maxSpots);
    auto done = readRecords(recPath);
    std::vector<std::pair<int, int>> todo;  // (spot, flop) still to solve
    for (int i = 0; i < int(spots.size()); ++i)
        for (int f = 0; f < int(std::size(FLOPS)); ++f)
            if (!done.count(historyText(spots[i].history) + " " + FLOPS[f].text)) todo.push_back({i, f});
    std::printf("%zu spots x %zu flops: %zu recorded, %zu to solve\n", spots.size(), std::size(FLOPS), done.size(),
                todo.size());

    std::FILE* rec = std::fopen(recPath.c_str(), "a");
    if (!rec) return std::fprintf(stderr, "cannot append to %s\n", recPath.c_str()), 1;
    std::mutex mu;
    std::atomic<size_t> next{0};
    auto worker = [&] {
        static const char* seat[] = {"UTG", "HJ", "CO", "BTN", "SB", "BB"};
        for (size_t k; (k = next++) < todo.size();) {
            auto [i, f] = todo[k];
            const FlopSpot& fs = spots[i];
            auto t0 = std::chrono::steady_clock::now();
            post::Spot spot;
            spot.range[0] = lineRange(s, fs.history, fs.oop()), spot.range[1] = lineRange(s, fs.history, fs.ip());
            spot.tree = tree(fs);
            spot.flop = FLOPS[f].cards;
            post::Result r = post::solve(spot, opt);
            RealizationAccum acc(fs);
            acc.add(r, 1);
            std::string line = historyText(fs.history) + " " + FLOPS[f].text + " " + std::to_string(r.exploitability) +
                               " " + std::to_string(r.iterations);
            char buf[32];
            for (auto* sums : {&acc.ev, &acc.eq, &acc.n})
                for (auto& row : *sums)
                    for (double x : row) std::snprintf(buf, sizeof buf, " %a", x), line += buf;
            std::lock_guard lock(mu);
            std::fprintf(rec, "%s\n", line.c_str());
            std::fflush(rec);
            std::printf("[%zu/%zu] %d-bet %s vs %s %s  expl %.4f  %d iters  %.1fs\n", k + 1, todo.size(), int(fs.pot) + 2,
                        seat[fs.raiser], seat[fs.caller], FLOPS[f].text.c_str(), r.exploitability, r.iterations,
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            std::fflush(stdout);
        }
    };
    std::vector<std::thread> pool;
    for (int j = 0; j < jobs; ++j) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    std::fclose(rec);

    // Every pair is recorded now: assemble the table from the records alone.
    done = readRecords(recPath);
    RealizationTable table;
    table.source = "solver strategy=" + stratPath + (chartDir.empty() ? "" : " charts=" + chartDir) +
                   " iters=" + std::to_string(opt.maxIters) + " expl=" + std::to_string(opt.targetExpl) +
                   " flops=" + std::to_string(std::size(FLOPS));
    for (const FlopSpot& fs : spots) {
        RealizationAccum acc(fs);
        for (const Flop& f : FLOPS) {
            auto it = done.find(historyText(fs.history) + " " + f.text);
            if (it == done.end())
                return std::fprintf(stderr, "record missing for %s %s\n", historyText(fs.history).c_str(), f.text.c_str()), 1;
            const Sums& x = it->second;
            for (int role = 0; role < 2; ++role)
                for (int b = 0; b < NBUCKETS; ++b)
                    acc.ev[role][b] += f.weight * x[role][b], acc.eq[role][b] += f.weight * x[2 + role][b],
                        acc.n[role][b] += f.weight * x[4 + role][b];
            ++acc.flops;
        }
        table.entries.push_back(acc.entry());
    }
    if (std::string err; !saveRealizationTable(out, table, &err)) return std::fprintf(stderr, "%s\n", err.c_str()), 1;
    std::printf("wrote %zu entries to %s\n", table.entries.size(), out.c_str());
}
