# The Math

## Counterfactual regret minimization (CFR)

Preflop is a large imperfect-information extensive-form game. The standard solution method is CFR.

- An infoset `I` is a player's hand plus the action history.
- On each iteration, compute the counterfactual value of each action `v(I,a)` and of the current strategy `v(I)`.
- Accumulate regret: `R(I,a) += v(I,a) - v(I)`.
- Play with **regret matching**: probability proportional to positive regret (uniform if none are positive).
- The **average** strategy over iterations (not the current one) converges to Nash equilibrium in two-player zero-sum games. Exploitability falls roughly like `1/sqrt(T)`.
- Use **Linear CFR** (or Discounted CFR): periodically discount accumulated regrets and weight the strategy sum by iteration `t`. Do **not** use CFR+ regret flooring. It is known to behave badly combined with Monte Carlo sampling.

## Caveats for 6 players

- Six-player games are not two-player zero-sum, so CFR has **no convergence guarantee**. In practice it produces strong strategies (Pluribus used this family of approaches).
- Rake (when added) breaks zero-sum slightly. Model it by taking rake at terminals where a flop is seen or an all-in occurs.
- Treat the output as an equilibrium of the model. Close agreement with reference charts is a good sign, not a proof of GTO.

## Terminal payoffs (no postflop)

Every terminal needs a value:

- **Everyone folds:** the last player standing wins the pot. Trivial.
- **All-in called:** sample a single board and evaluate the actual hole cards (unbiased; ties split the pot). Use a fast existing hand evaluator.
- **Action closes and a flop would be dealt:** use realization-adjusted equity. To keep the game zero-sum, normalize across the players still in the hand:

```
  share_i   = R_i * eq_i / sum_j(R_j * eq_j)
  payoff_i  = share_i * pot - contributed_i
```

  `R_i` is the equity realization factor. It is about 1 for the player who will act last postflop *among those still in the hand*, and lower for the others. It also varies by hand type (suited/connected hands realize more, offsuit trash less). **This is the biggest source of model error and the main tuning knob.**

  `eq_i` must be a **real equity estimate**, not a single sampled board. With one board each `eq_i` is 0 or 1, so the `R` factors cancel and the formula collapses to winner-takes-all.

  - Heads-up: precompute a combo-vs-combo equity table once (exact enumeration using suit isomorphism, or high-sample Monte Carlo per canonical matchup), save it to `data/`, and look it up in the hot loop.
  - Multiway: estimate with K sampled boards (e.g. K = 50–200) inside the loop, optionally cached by canonical hand set. The ratio is nonlinear, so small K adds a small bias. Pick K by checking that strategies stop moving as K grows.
