#pragma once
// Measured realization factors (version 2, Phase 1). Produced offline from postflop solves
// (src/postflop) or from the heuristic rollout, in the same format, and read by realization().
// Owned by the realization-table branch; the format here is the contract.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "solver/cards.h"
#include "trainer/game.h"

namespace pf {

enum class PotType : uint8_t { Single, ThreeBet, FourBet };

// One heads-up flop spot: the last raiser and the caller, by seat, in a pot of the given type.
struct RealizationEntry {
    PotType pot = PotType::Single;
    Pos raiser = UTG, caller = BB;
    double potBB = 0, stackBB = 0;          // at the flop
    int flops = 0;                          // flops aggregated
    std::array<float, NBUCKETS> rRaiser{}, rCaller{};  // R per class (bucket()); 0 = class absent
    std::array<float, NBUCKETS> nRaiser{}, nCaller{};  // aggregated weight per class, for shrinkage
};

struct RealizationTable {
    uint32_t version = 1;
    std::string source;                     // "solver" or "heuristic", plus settings
    std::vector<RealizationEntry> entries;
};

bool saveRealizationTable(const std::string& path, const RealizationTable& t, std::string* err = nullptr);
bool loadRealizationTable(const std::string& path, RealizationTable& t, std::string* err = nullptr);

}  // namespace pf
