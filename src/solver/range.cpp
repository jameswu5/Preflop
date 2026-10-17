#include "solver/range.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <utility>

namespace pf {

static const char RANKS[] = "AKQJT98765432";  // grid order, as in cards.cpp

static int rankIdx(char c) {  // grid index: A = 0 ... 2 = 12
    const char* p = std::strchr(RANKS, c);
    return c && p ? int(p - RANKS) : -1;
}

// Grid indices of one class token without '+' or '-': "AA", "AKs", "AKo", "AK".
static bool cells(const std::string& t, int& hi, int& lo, char& kind) {
    if (t.size() < 2 || t.size() > 3) return false;
    hi = rankIdx(t[0]), lo = rankIdx(t[1]), kind = t.size() == 3 ? t[2] : 0;
    if (hi < 0 || lo < 0 || hi > lo) return false;  // higher card first
    if (hi == lo) return kind == 0;
    return kind == 0 || kind == 's' || kind == 'o';
}

static void set(RangeFreqs& r, int hi, int lo, char kind, float f) {
    if (hi == lo) r[hi * 13 + hi] = f;
    if (hi != lo && kind != 'o') r[hi * 13 + lo] = f;  // suited above the diagonal
    if (hi != lo && kind != 's') r[lo * 13 + hi] = f;
}

bool parseRange(const std::string& text, RangeFreqs& out, std::string* err) {
    RangeFreqs r{};
    std::stringstream ss(text);
    auto fail = [&](const std::string& t) {
        if (err) *err = "bad range token '" + t + "'";
        return false;
    };
    for (std::string tok; std::getline(ss, tok, ',');) {
        tok.erase(0, tok.find_first_not_of(" \t"));
        tok.erase(tok.find_last_not_of(" \t\r") + 1);
        if (tok.empty()) continue;
        float f = 1;
        if (auto c = tok.find(':'); c != std::string::npos) {
            char* end;
            f = std::strtof(tok.c_str() + c + 1, &end);
            if (*end || end == tok.c_str() + c + 1 || f < 0 || f > 1) return fail(tok);
            tok.resize(c);
        }
        int hi, lo, hi2, lo2;
        char kind, kind2;
        if (auto d = tok.find('-'); d != std::string::npos) {  // QQ-22, AKs-A2s, K9o-K5o
            if (!cells(tok.substr(0, d), hi, lo, kind) || !cells(tok.substr(d + 1), hi2, lo2, kind2) || kind != kind2)
                return fail(tok);
            if (hi > hi2 || (hi == hi2 && lo > lo2)) std::swap(hi, hi2), std::swap(lo, lo2);  // 22-QQ == QQ-22
            if (hi == lo && hi2 == lo2) {
                for (int p = hi; p <= hi2; ++p) set(r, p, p, 0, f);
            } else if (hi != lo && hi == hi2 && lo <= lo2) {
                for (int k = lo; k <= lo2; ++k) set(r, hi, k, kind, f);
            } else {
                return fail(tok);
            }
        } else if (tok.back() == '+') {  // 22+ (pairs up to AA), ATs+ (kicker up to just below the top card)
            tok.pop_back();
            if (!cells(tok, hi, lo, kind)) return fail(tok);
            if (hi == lo)
                for (int p = 0; p <= hi; ++p) set(r, p, p, 0, f);
            else
                for (int k = hi + 1; k <= lo; ++k) set(r, hi, k, kind, f);
        } else {
            if (!cells(tok, hi, lo, kind)) return fail(tok);
            set(r, hi, lo, kind, f);
        }
    }
    out = r;
    return true;
}

std::string formatRange(const RangeFreqs& r) {
    std::string s;
    for (int b = 0; b < NBUCKETS; ++b) {
        if (r[b] <= 0) continue;
        if (!s.empty()) s += ',';
        s += bucketName(b);
        if (r[b] < 1) {
            char buf[16];
            std::snprintf(buf, sizeof buf, ":%g", r[b]);
            s += buf;
        }
    }
    return s;
}

}  // namespace pf
