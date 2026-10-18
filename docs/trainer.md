# Trainer

The trainer has two layers: a **headless core** (spot generation, bots, rollouts, stats) with no raylib dependency, so it is unit-testable, and a **raylib GUI** on top.

## Core

- Each hand: deal 6 hole cards, simulate bots up to the user's decision point, show the spot, and take the user's choice (F/C/R).
- **Spot filtering:** `deal()` reject-samples hands until the user's seat reaches a decision of the requested type and position. Bots, and the user's own earlier decisions in the hand, play from the strategy. Spot types are by what the user faces: open (unopened pot), vs open, squeeze (an open plus callers), vs 3-bet, vs squeeze (the user cold-called and someone 3-bet), and vs 4-bet+. A type/seat that can never occur (e.g. vs squeeze at UTG or BB) returns nothing after the try limit.
- Bots sample their actions from the solved average strategy for their (history, hand). This keeps them mixed and lets them respond sensibly to off-equilibrium user lines.
- **Feedback:** do not read action values from sampled regrets (too noisy). Instead, for the user's actual hand, roll out each legal action about 20k times with all bots following their average strategies, and report EV in bb per action plus EV loss of the user's choice versus the best action.
  - Each rollout **re-deals the opponents' hands** from the deck minus the user's cards. Do not use the hands the bots actually hold, since that leaks hidden information and adds variance.
  - Weight each rollout by the probability that the bots would have taken the observed history with those hands: the product, over prior bot actions, of `sigma(history_k, bucket(hand))[action_k]`. Ignoring the history gives wrong EVs because the ranges are wrong. Weighting (importance sampling) is simpler and lower-variance than rejection.
  - Terminal payoffs use the same utility code as the solver.
- Also show the solved mixed strategy for the user's hand (e.g. "3-bet 35% / call 65%") and the bot range for context.
- Track stats per spot type and position: average EV loss, accuracy, and most-leaked spots.
- Accept as "correct" any action with strategy frequency or EV loss below a configurable threshold (mixed strategies mean several actions can be fine).
- **Chart modes (GTO, PTO):** alternative strategies built by overwriting every spot that has a reference chart in the mode's directory (`ref/`, `ref/pto/`) with that chart (`applyCharts`, `src/solver/strategy.h`; `scope: listed` charts overwrite only the classes they name). The base is the mode's chart-locked solve (`data/strategy_gto.bin`, `data/strategy_pto.bin`: `solve --lock`, so uncharted spots best-respond to the charts) if it exists, else the plain solve. The charts are re-applied over a locked solve anyway, which gives the charted-entry mask and picks up painter edits. Bots, rollouts and grading all use this strategy, so the user trains against chart-playing opponents. Where the chart decides the user's hand, grading ignores EV: an action is correct iff the chart plays it at least 10% of the time. Rollout EVs are still shown, valued against the chart-playing bots. Where the solve decides, the usual grading applies. Stats are kept per mode.

## GUI (raylib)

- A table view: seats, stacks, positions, bets in front of players, pot, the user's hole cards, and fold/call/raise buttons labelled with amounts. Keyboard shortcuts too (F/C/R, space for next hand).
- A feedback panel after each decision: EV per action, EV loss, the solved mix for the hand, and a 13×13 range grid for the relevant bot or for the user's spot.
- A spot picker (spot type, position) and a stats screen.
- `T` cycles Solver / GTO / PTO, skipping chart modes with no charts. It takes effect at the next hand, because rollouts on the worker thread read the current hand's strategy. A badge shows the mode (and the pending one), the mix is labelled with where it comes from, and an uncharted decision in a chart mode is marked "no chart" plus its source (locked solve or solver fallback). The painter edits the GTO charts; closing it reloads the GTO mode, after any running rollouts finish.
- Use **raygui** (header-only, from the raylib author) for buttons and panels. Cards can be drawn as rectangles with text/suit glyphs. Add a sprite sheet later if wanted.
- Rollouts take noticeable time. Run them on a worker thread and keep the UI loop responsive.
