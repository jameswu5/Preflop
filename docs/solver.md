# Solver

The algorithm and payoff model are in [math.md](math.md). This page covers the implementation: `src/solver/mccfr.*`, `src/solver/strategy.*`, and the `solve` tool.

## Two solves

- **Scratch** (`data/strategy_6max.bin`, checkpoint `data/6max.ck`): MCCFR from scratch over the whole tree, with the payoff model's fitted parameters. It is what `validate` and `tune.py` score against the reference charts.
- **Anchored** (`data/strategy_anchored.bin`): the GTO reference charts in `ref/` frozen, and everything else solved by the same method around them ([chart-locked solving](#chart-locked-solving)), warm-started from Scratch:

  ```
  build/solve --warm data/6max.ck --reset-average --iters 300000000 --chunk 100000000 --threads 8 \
              --lock ref --out data/strategy_anchored.bin
  ```

  The trainer's GTO mode plays Anchored; its PTO mode overlays the PTO charts on Anchored. Validating Anchored against `ref/` is trivially 0 at the frozen spots, so the comparison with the charts is Scratch's.

## Hand representation

- **169 strategic hand classes** as infoset buckets. Strategies are suit-symmetric at equilibrium preflop.
- **Real 2-card combos** are dealt to all six players each iteration from one shared deck. This models card removal for free (e.g. A5s blocks aces, AKo makes AA/KK less likely in opponents' ranges), which matters a lot in 4-bet and squeeze spots.

## Loop

```python
def traverse(hand, node, traverser):
    if hand.done():
        return utility(hand)[traverser]              # math.md, terminal payoffs
    p = actor[node]
    w = (t - 1) // 10_000 + 1                         # linear weight: the iteration's block number
    if p < root_folds:                               # short-handed solve: seat forced to fold
        return traverse(hand.apply(F), next[node][F], traverser)
    b = bucket(hand.cards[p])                        # 0..168
    sigma = regret_match(regret[node][b], legal[node])
    if p == traverser:
        v = {a: traverse(hand.apply(a), next[node][a], traverser) for a in legal[node]}
        ev = sum(sigma[a] * v[a] for a in legal[node])
        for a in legal[node]:
            regret[node][b][a] += w * (v[a] - ev)
        return ev
    strat_sum[node][b] += w * sigma
    a = sample(sigma)
    return traverse(hand.apply(a), next[node][a], traverser)

for t in 1..T:
    traverse(deal_six_hands(), root, traverser=root_folds + t % (6 - root_folds))
```

- Linear CFR (math.md, block discounting) is applied lazily: instead of multiplying both tables by $k/(k+1)$ every 10,000 iterations, each increment is multiplied by its block number. The tables then hold the discounted sums times (blocks done + 1), and since regret matching and the average are both scale-free, the strategies are the same up to float rounding, with no pass over the tables.
- In the real code, `Hand` is only used to deal. Each terminal's payoff inputs (contributions, pot after rake, SPR, who is live, last to act, last raiser, all-in) are fixed by the betting history, so the solver precomputes them per terminal (`Terminal` in `utility.h`) and traverses the tree by node index.
- Multiway equity evaluates 7-card hands with an additive key per card (rank digits and suit counts in one integer), so the board's key is summed once and shared by every player; non-flush hands are a table lookup built from PokerHandEvaluator at startup, flushes fall back to it directly. Results are identical to calling the evaluator.

- The tree is built once by walking `Hand` (`Tree` in `strategy.h`): per node, the actor, a legal-action mask and a child index per action (-1 at terminals). `regret` and `strat_sum` are flat `float` arrays of shape `[nodes][169][3]`. Traversal is recursive; the tree is only a few raise levels deep.
- The final strategy is `strat_sum` normalized per (node, class). `Strategy::save` writes it with the `Config` and `RealizationConfig` it was solved with, so the trainer uses the same rules and payoff model.
- `--seats 2|3` solves SB vs BB or BTN/SB/BB on the same tree by forcing the earlier seats to fold at the root. Those nodes are stored as fold = 1.

## Threads

`solve --threads N` runs iterations in parallel, Hogwild-style: N threads share the tables and update them with relaxed atomic adds. Each thread has its own RNG, seeded from the solver's RNG, and runs its share of the chunk with no synchronization (the lazy linear weights need no discount pass). With one thread a run is bit-reproducible for a fixed `--seed`; with more it is not.

## Checkpoints

`--checkpoint file` is resumed if it exists and is rewritten after every chunk. It stores the full state: Config, SolverConfig, iteration count, RNG state, regrets and strategy sums (at the lazy scale above; older versions stored them rescaled and are converted on load). Writes go to a temp file that is then renamed, so a crash never leaves a half-written file. A checkpoint made with a different Config or SolverConfig is refused, with one exception: a different `RealizationConfig` is accepted as a warm start (regrets kept, strategy average and iteration count restarted). `tune.py` uses this, and `--reset-average` forces the same restart explicitly.

## Chart-locked solving

`solve --lock dir` reads every chart in `dir` (the `ref/` format, via `applyCharts`) and fixes those (spot, class) entries: MCCFR plays the chart's probabilities there and never adds to their strategy sums, and the output keeps the chart values. Everything else solves normally, so the uncharted spots become a best response to players who follow the charts. With `scope: listed`, only the listed classes are fixed. The lock is not saved in checkpoints, so pass `--lock` again when resuming.

Warm-start a locked solve from the Scratch checkpoint, as the Anchored recipe above does (`--warm data/6max.ck --reset-average --lock ref`). After an action a chart never takes (a 3-bet-or-fold chart's call, say), the next players reach that spot only when the locked player deviates. That happens only while the locked player is the traverser, so they never accumulate regret there, and on a fresh solve they stay uniform. Warm-started, they keep the unlocked solve's regrets, and `average()` plays those where no strategy sum builds up. Bots and rollouts then respond to deviations from the chart with the model's play. `solve` warns when `--lock` starts fresh.

Regrets at locked entries are still accumulated, as a diagnostic only. They are never played. At the end `solve` ranks the charted spots by how much the chart gives up against the rest of the solve. For each locked class this is the best action's mean regret against the chart's mix, in bb per hand dealt, so frequent spots and classes weigh more. The per-class values are summed over the spot, and the top `--top N` spots (default 10) are printed with their largest classes. Those are the spots where the chart and our payoff model disagree most. Only actions the chart takes at the spot count toward the ranking. The gain from actions it never takes is printed apart (`off`): it is valued against responders that learned only from off-path play, so it reflects how they were trained (uniform on a fresh solve, so flatting aces "wins") more than the chart.
