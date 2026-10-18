#undef NDEBUG  // the build defaults to Release; keep asserts on
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "solver/range.h"
#include "solver/strategy.h"

using namespace pf;

static float at(const RangeFreqs& r, const char* name) { return r[bucketFromName(name)]; }

static void write(const std::filesystem::path& p, const char* text) { std::ofstream(p) << text; }

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

    // applyCharts: a full chart at the root (UTG open), a `scope: listed` one at "R" (HJ vs the open)
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "range_test_charts";
    fs::remove_all(dir);
    fs::create_directories(dir / "sub");
    write(dir / "open.txt", "# a comment\ncategory: open\nspot:\nraise: AA,KK:0.5\n");
    write(dir / "hj.txt", "spot: R  # trailing comment\nscope: listed\nraise: AA\ncall: QQ:0.25\n");
    write(dir / "skip.off", "not a chart");
    write(dir / "sub" / "skip.txt", "not a chart");
    Chart c;
    assert(readChart((dir / "open.txt").string(), c, err) && c.spot.empty() && c.category == "open" && c.comments == "# a comment\n");
    assert(c.given[2] && !c.given[1] && !c.listedOnly && at(c.act[0], "KK") == 0.5f && at(c.act[0], "72o") == 1);

    Strategy s, orig;
    std::vector<uint8_t> locked;
    int root = s.root(), hj = s.nodeFor({Act::Raise}), other = s.nodeFor({Act::Fold});
    auto p = [&](const Strategy& st, int n, const char* h) { return st.probs(n, bucketFromName(h)); };
    assert(applyCharts(s, dir.string(), &locked, &err) == 2);
    assert((p(s, root, "AA") == std::array<float, 3>{0, 0, 1}) && (p(s, root, "KK") == std::array<float, 3>{0.5f, 0, 0.5f}));
    assert((p(s, root, "72o") == std::array<float, 3>{1, 0, 0}));  // unlisted = fold in a full chart
    assert((p(s, hj, "AA") == std::array<float, 3>{0, 0, 1}) && (p(s, hj, "QQ") == std::array<float, 3>{0.75f, 0.25f, 0}));
    assert(p(s, hj, "72o") == p(orig, hj, "72o"));  // scope: listed leaves unlisted classes alone
    for (int b = 0; b < NBUCKETS; ++b) assert(s.probs(other, b) == orig.probs(other, b));
    assert(locked.size() == size_t(s.tree().size()) * NBUCKETS);
    auto lk = [&](int n, const char* h) { return locked[size_t(n) * NBUCKETS + bucketFromName(h)]; };
    assert(lk(root, "72o") && lk(hj, "AA") && lk(hj, "QQ") && !lk(hj, "72o") && !lk(other, "AA"));

    Strategy bad = orig;  // errors leave the strategy unchanged
    write(dir / "zz.txt", "spot:\ncall: AA\n");  // no limping: call is illegal at the root
    assert(applyCharts(bad, dir.string(), &locked, &err) == -1 && err.find("illegal") != std::string::npos);
    write(dir / "zz.txt", "spot: FFFFFF\n");
    assert(applyCharts(bad, dir.string(), nullptr, &err) == -1 && err.find("decision node") != std::string::npos);
    assert(bad.probs(root, bucketFromName("AA")) == orig.probs(root, bucketFromName("AA")));
    assert(applyCharts(bad, (dir / "missing").string(), nullptr, &err) == -1);
    fs::remove_all(dir);

    Strategy ref;
    assert(applyCharts(ref, REF_DIR, nullptr, &err) == 65);
    std::puts("range_test: ok");
    return 0;
}
