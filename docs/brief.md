# Preflop 6-Max Solver + Trainer: Project Brief

## Goal

Build a preflop-only NLHE 6-max (100bb cash) solver and an interactive trainer on top of it, in **C++** with a **raylib** frontend.

- The solver computes approximate equilibrium strategies for a **deliberately small action tree** (fold / call / raise, with one raise size per level) using Monte Carlo CFR.
- The trainer deals the user a spot, simulates the 5 other players as bots playing the solved strategies (or reference charts), and after the user's decision shows EV feedback before the next hand.
- The game never goes postflop. Every hand resets all stacks to 100bb.
- Focus spots: **3-bet, 4-bet and squeeze** (but the solver should solve the whole preflop tree).
- Output is validated and tuned against externally solved charts (see [validation.md](validation.md)).

## Non-goals (v1)

- No postflop solving or abstraction.
- No rake (add later as a flag). *Since added: `Config::rake`, on by default.*
- No limping, no multiple sizings, no stack depths other than 100bb. *Other stack depths have since been added through config files (`solve --config`).*
- Not intended to match commercial solvers exactly. It is an equilibrium of *our* simplified game.

Also since implemented: calling squeezes, bet sizes matched to the reference site, and a second solve with the reference charts locked (Anchored, [solver.md](solver.md)), which the trainer can play alongside the charts themselves.

## Stack

- **C++20** throughout, built with **CMake**.
- **raylib** for the frontend, pulled in via `FetchContent` (buttons and panels are drawn directly; no raygui).
- An existing hand evaluator, PokerHandEvaluator (also via `FetchContent`), rather than writing one.
- Tests are plain test executables run by CTest. Add a framework (e.g. doctest) only if plain asserts get painful.

## Difficulty and risks

- Short-handed push/fold with 169 hands: a weekend project and a good CFR warm-up.
- Full 6-max with a limited tree and an equity-realization model: weeks, mostly spent on tuning, validation and speed.
- Matching commercial solvers: very hard (needs real postflop modeling and large compute). Out of scope.

Main risks:

1. The realization factors (R) dominate the quality of flatting ranges and 3-bet bluff frequencies. Budget time for tuning.
2. Multiplayer CFR has no convergence guarantee. Monitor strategy stability across checkpoints.
3. Sizing and rule mismatches between our tree and the reference will look like solver errors. Align the setup first.
4. Pure-strategy simplifications in references can mark correct mixed plays as wrong. Keep mixed frequencies in all data models.
5. Multiway equity estimation in the hot loop is the likely speed bottleneck. Profile it early, and cache or lower K if needed.

## Docs

- [game-rules.md](game-rules.md): game abstraction, action rules, bet sizing.
- [math.md](math.md): CFR background, 6-player caveats, terminal payoffs.
- [solver.md](solver.md): hand representation and the MCCFR algorithm.
- [validation.md](validation.md): sanity checks, tuning, reference data.
- [trainer.md](trainer.md): trainer core and raylib GUI.
- [architecture.md](architecture.md): repo layout and milestones.
