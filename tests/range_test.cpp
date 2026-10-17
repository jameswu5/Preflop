#undef NDEBUG  // the build defaults to Release; keep asserts on
#include <cassert>
#include <cstdio>

#include "solver/range.h"

using namespace pf;

static float at(const RangeFreqs& r, const char* name) { return r[bucketFromName(name)]; }

static int count(const RangeFreqs& r) {  // combos in range (frequency 1 only)
    int n = 0;
    for (int b = 0; b < NBUCKETS; ++b) n += r[b] == 1 ? comboCount(b) : 0;
    return n;
}

int main() {
    RangeFreqs r;
    assert(parseRange("AA, AKs ,AKo:0.5,KQ", r));
    assert(at(r, "AA") == 1 && at(r, "AKs") == 1 && at(r, "AKo") == 0.5f && at(r, "KQs") == 1 && at(r, "KQo") == 1);
    assert(at(r, "KK") == 0 && at(r, "AQs") == 0);

    assert(parseRange("QQ-22", r) && at(r, "QQ") == 1 && at(r, "22") == 1 && at(r, "KK") == 0 && count(r) == 11 * 6);
    RangeFreqs rev;
    assert(parseRange("22-QQ", rev) && rev == r);
    assert(parseRange("AKs-A2s", r) && count(r) == 12 * 4 && at(r, "AKo") == 0);
    assert(parseRange("K9o-K5o:0.25", r) && at(r, "K7o") == 0.25f && at(r, "K4o") == 0 && at(r, "K9s") == 0);
    assert(parseRange("TT+", r) && at(r, "AA") == 1 && at(r, "TT") == 1 && at(r, "99") == 0);
    assert(parseRange("ATs+", r) && at(r, "AKs") == 1 && at(r, "ATs") == 1 && at(r, "A9s") == 0 && at(r, "AA") == 0);
    assert(parseRange("KTo+", r) && at(r, "KQo") == 1 && at(r, "KTo") == 1 && at(r, "K9o") == 0);
    assert(parseRange("AK:0.5,AK:1", r) && at(r, "AKs") == 1);  // last one wins
    assert(parseRange("", r) && count(r) == 0);

    std::string err;
    for (const char* bad : {"KA", "AAs", "AKx", "AK:1.5", "AK:-1", "AK:", "QQ-AKs", "AKs-QJs", "XX", "AKs-A2o"})
        assert(!parseRange(bad, r, &err) && !err.empty());

    assert(parseRange("AA,KK:0.75,AKs,72o:0.5", r));
    assert(formatRange(r) == "AA,AKs,KK:0.75,72o:0.5");  // grid order
    RangeFreqs back;
    assert(parseRange(formatRange(r), back) && back == r);
    std::puts("range_test: ok");
    return 0;
}
