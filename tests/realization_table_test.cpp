#undef NDEBUG  // the build defaults to Release; keep asserts on
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "solver/realization_table.h"

using namespace pf;

int main() {
    RealizationTable t;
    t.source = "solver test";
    for (int k = 0; k < 3; ++k) {
        RealizationEntry e;
        e.pot = PotType(k), e.raiser = Pos(k), e.caller = BB, e.potBB = 5.5 + k, e.stackBB = 97.5 - k, e.flops = 10 * k;
        for (int b = 0; b < NBUCKETS; ++b)
            e.rRaiser[b] = 1 + b * 0.001f, e.rCaller[b] = k + b, e.nRaiser[b] = b % 7, e.nCaller[b] = 1.0f / (b + 1);
        t.entries.push_back(e);
    }
    const char* path = "realization_table_test.bin";
    std::string err;
    assert(saveRealizationTable(path, t, &err));

    RealizationTable u;
    assert(loadRealizationTable(path, u, &err));
    assert(u.version == t.version && u.source == t.source && u.entries.size() == t.entries.size());
    for (size_t i = 0; i < t.entries.size(); ++i) {
        auto &a = t.entries[i], &b = u.entries[i];
        assert(a.pot == b.pot && a.raiser == b.raiser && a.caller == b.caller && a.potBB == b.potBB &&
               a.stackBB == b.stackBB && a.flops == b.flops && a.rRaiser == b.rRaiser && a.rCaller == b.rCaller &&
               a.nRaiser == b.nRaiser && a.nCaller == b.nCaller);
    }

    // Malformed files fail and leave the table unchanged.
    auto bytes = [&] {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }();
    auto rejects = [&](const std::string& data) {
        std::ofstream("bad.bin", std::ios::binary) << data;
        RealizationTable v = u;
        bool ok = loadRealizationTable("bad.bin", v, &err);
        assert(v.entries.size() == u.entries.size() && v.source == u.source);
        return !ok && !err.empty();
    };
    assert(rejects(bytes.substr(0, bytes.size() - 1)));  // truncated
    assert(rejects(bytes + "x"));                        // trailing bytes
    assert(rejects("PFST" + bytes.substr(4)));            // wrong magic
    std::string v2 = bytes;
    v2[4] = 2;  // version
    assert(rejects(v2));
    assert(!loadRealizationTable("does_not_exist.bin", u, &err));

    std::filesystem::remove(path), std::filesystem::remove("bad.bin");
    std::puts("realization_table_test OK");
}
