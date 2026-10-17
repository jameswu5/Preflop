# Solver

The algorithm and payoff model are in [math.md](math.md). This page covers the implementation: `src/solver/mccfr.*`, `src/solver/strategy.*`, and the `solve` tool.

## Hand representation

- **169 strategic hand classes** as infoset buckets. Strategies are suit-symmetric at equilibrium preflop.
- **Real 2-card combos** are dealt to all six players each iteration from one shared deck. This models card removal for free (e.g. A5s blocks aces, AKo makes AA/KK less likely in opponents' ranges), which matters a lot in 4-bet and squeeze spots.

## Loop

```python
def traverse(hand, node, traverser):
    if hand.done():
        return utility(hand)[traverser]              # math.md, terminal payoffs
    p = actor[node]
    if p < root_folds:                               # short-handed solve: seat forced to fold
        return traverse(hand.apply(F), next[node][F], traverser)
    b = bucket(hand.cards[p])                        # 0..168
    sigma = regret_match(regret[node][b], legal[node])
    if p == traverser:
        v = {a: traverse(hand.apply(a), next[node][a], traverser) for a in legal[node]}
        ev = sum(sigma[a] * v[a] for a in legal[node])
        for a in legal[node]:
            regret[node][b][a] += v[a] - ev
        return ev
    strat_sum[node][b] += sigma                      # unweighted; the discount below makes it linear
    a = sample(sigma)
    return traverse(hand.apply(a), next[node][a], traverser)

for t in 1..T:
    traverse(deal_six_hands(), root, traverser=root_folds + t % (6 - root_folds))
    if t % 10_000 == 0:                              # Linear CFR by block discounting
        k = t // 10_000
        regret *= k / (k + 1); strat_sum *= k / (k + 1)
```

- The tree is built once by walking `Hand` (`Tree` in `strategy.h`): per node, the actor, a legal-action mask and a child index per action (-1 at terminals). `regret` and `strat_sum` are flat `float` arrays of shape `[nodes][169][3]`. Traversal is recursive; the tree is only a few raise levels deep.
- The final strategy is `strat_sum` normalized per (node, class). `Strategy::save` writes it with the `Config` and `RealizationConfig` it was solved with, so the trainer uses the same rules and payoff model.
- `--seats 2|3` solves SB vs BB or BTN/SB/BB on the same tree by forcing the earlier seats to fold at the root. Those nodes are stored as fold = 1.

## Threads

`solve --threads N` runs iterations in parallel, Hogwild-style: N threads share the tables and update them with relaxed atomic adds. Each thread has its own RNG, seeded from the solver's RNG. Parallel blocks end on discount boundaries, so the discount itself runs between blocks with no concurrent writers. With one thread a run is bit-reproducible for a fixed `--seed`; with more it is not.

## Checkpoints

`--checkpoint file` is resumed if it exists and is rewritten after every chunk. It stores the full state: Config, SolverConfig, iteration count, RNG state, regrets and strategy sums. Writes go to a temp file that is then renamed, so a crash never leaves a half-written file. A checkpoint made with a different Config or SolverConfig is refused, with one exception: a different `RealizationConfig` is accepted as a warm start (regrets kept, strategy average and iteration count restarted). `tune.py` uses this, and `--reset-average` forces the same restart explicitly.
