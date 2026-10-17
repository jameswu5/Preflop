# Solver

## Hand representation

- **169 strategic hand classes** for infoset buckets (suit-symmetric at equilibrium preflop).
- **Deal real 2-card combos** to all six players each iteration from one shared deck. This gives card removal effects for free (e.g. A5s blocks aces, AKo reduces AA/KK in opponents' ranges), which matters a lot in 4-bet and squeeze spots.

## Algorithm: external-sampling MCCFR

Pseudocode:

```python
def traverse(state, hands, traverser, t):
    if state.is_terminal():
        return utility(state, hands)[traverser]      # see math.md, terminal payoffs

    p = state.to_act
    legal = state.legal_actions()                    # subset of {F, C, R}
    node = nodes[state.key]                          # per-history arrays
    b = bucket(hands[p])                             # 0..168
    sigma = regret_match(node.regret[b], legal)

    if p == traverser:
        vals = {a: traverse(state.apply(a), hands, traverser, t) for a in legal}
        ev = sum(sigma[a] * vals[a] for a in legal)
        for a in legal:
            node.regret[b][a] += vals[a] - ev
        return ev
    else:
        node.strat_sum[b] += t * sigma               # linear averaging
        a = sample(sigma)
        return traverse(state.apply(a), hands, traverser, t)

for t in range(1, T + 1):
    hands = deal_six_hands()
    traverse(root, hands, traverser=t % 6, t=t)
    # every N iterations: discount regrets (Linear/Discounted CFR)
```

- The final bot strategy is `strat_sum` normalized per (history, bucket), **not** the current regret-matched strategy.
- Expect to need hundreds of millions of iterations. The whole hot loop is C++, built with optimizations on.
- Build the tree once into flat arrays (per node: actor, legal-action mask, child indices, pot/contributions, terminal type). Store `regret` and `strat_sum` as flat `float` arrays of shape `[n_nodes][169][3]`. Traversal can stay recursive since depth is small.
- Start single-threaded. To parallelize later, run independent traversals on several threads with per-thread RNGs, sharing tables via relaxed atomics (Hogwild-style) or per-thread tables merged periodically.
- Make runs checkpointable and resumable. Write `regret`, `strat_sum` and the iteration count to a binary file periodically.
- Seed all RNGs and support deterministic runs for testing.
