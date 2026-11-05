#include "solver/realization_table.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace pf {

// File: "PFRT" u32 version, u32 source length, source bytes, u32 entries, then per entry
// u8 pot, u8 raiser, u8 caller, f64 potBB, f64 stackBB, i32 flops, f32 rRaiser/rCaller/nRaiser/nCaller [169].
// Fields are written one by one, so the file doesn't depend on struct padding.
static constexpr char MAGIC[4] = {'P', 'F', 'R', 'T'};

template <class T> static void put(std::ostream& o, const T& x) { o.write(reinterpret_cast<const char*>(&x), sizeof x); }
template <class T> static bool get(std::istream& i, T& x) { return bool(i.read(reinterpret_cast<char*>(&x), sizeof x)); }

static bool fail(std::string* err, const std::string& e) {
    if (err) *err = e;
    return false;
}

bool saveRealizationTable(const std::string& path, const RealizationTable& t, std::string* err) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        out.write(MAGIC, 4);
        put(out, t.version);
        put(out, uint32_t(t.source.size()));
        out.write(t.source.data(), t.source.size());
        put(out, uint32_t(t.entries.size()));
        for (const RealizationEntry& e : t.entries) {
            put(out, uint8_t(e.pot)), put(out, uint8_t(e.raiser)), put(out, uint8_t(e.caller));
            put(out, e.potBB), put(out, e.stackBB), put(out, int32_t(e.flops));
            put(out, e.rRaiser), put(out, e.rCaller), put(out, e.nRaiser), put(out, e.nCaller);
        }
        if (!out) return fail(err, "cannot write " + tmp);
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) return fail(err, "cannot rename " + tmp);  // never half-written
    return true;
}

bool loadRealizationTable(const std::string& path, RealizationTable& t, std::string* err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail(err, "cannot open " + path);
    char magic[4];
    RealizationTable r;
    uint32_t len, n;
    if (!in.read(magic, 4) || std::memcmp(magic, MAGIC, 4)) return fail(err, path + ": not a realization table");
    if (!get(in, r.version) || r.version != RealizationTable{}.version)
        return fail(err, path + ": unsupported version " + std::to_string(r.version));
    if (!get(in, len) || len > 1 << 20) return fail(err, path + ": bad source length");
    r.source.resize(len);
    if (!in.read(r.source.data(), len) || !get(in, n) || n > 1 << 16) return fail(err, path + ": truncated header");
    r.entries.resize(n);
    for (RealizationEntry& e : r.entries) {
        uint8_t pot, raiser, caller;
        int32_t flops;
        if (!get(in, pot) || !get(in, raiser) || !get(in, caller) || !get(in, e.potBB) || !get(in, e.stackBB) ||
            !get(in, flops) || !get(in, e.rRaiser) || !get(in, e.rCaller) || !get(in, e.nRaiser) || !get(in, e.nCaller))
            return fail(err, path + ": truncated entry");
        if (pot > uint8_t(PotType::FourBet) || raiser >= NPOS || caller >= NPOS || raiser == caller)
            return fail(err, path + ": bad entry key");
        e.pot = PotType(pot), e.raiser = Pos(raiser), e.caller = Pos(caller), e.flops = flops;
    }
    if (in.peek() != EOF) return fail(err, path + ": trailing bytes");
    t = std::move(r);
    return true;
}

}  // namespace pf
