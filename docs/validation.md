# Validation and Tuning

## Process

1. **Sanity checks** (cheap, run first):
   - UTG opens a mid-teens percent of hands, BTN somewhere in the 40s.
   - Premium hands 4-bet. 3-bet bluffs skew to low suited aces/suited connectors.
   - Strategy sums are valid distributions, and no illegal actions have probability.
2. **Distance metric** against reference charts, per spot:
   `sum_hand weight * |sigma_ours - sigma_ref| / sum_hand weight`, plus range width per action (open %, 3-bet %, fold %).
3. **Tuning knobs** (few parameters to limit overfitting): R by position, small R adjustments by hand class (suited, pair, connectedness), and optionally sizes. Fit with grid search or Nelder-Mead against the reference charts. Each evaluation is a full re-solve, so warm-start from the previous checkpoint and tune on shorter runs first.
4. **De-risk by scaling up:** parameterize the tree on seat count. Get a short-handed version working first (e.g. BTN/SB/BB, then 4-handed), then go to 6-max.
5. **Convergence check:** measure how much the average strategy changes between checkpoints. Optionally compute best-response exploitability in a reduced game.

## Reference data

No free bulk export of solved ranges exists, and most sellers' terms likely prohibit scraping or redistribution. Use these as **comparison/validation sources for personal use**:

- **Simple Preflop:** free viewer for browsing solved ranges. Several sellers' range sets can be opened in it. Verify the export format before building a loader around it.
- **Solved Poker (Gumroad):** paid 6-max 100bb NLHE sets at various rake levels (the no-rake set was $199 when checked). They use simplified trees: 2.5bb opens, no limping, 3-bet-or-fold from most positions. Matching their sizing rules in our config makes comparison easier.
- **Cash Game Hero, Upswing Lab (Advanced Solver Ranges), Poker Academy:** other range/chart sources, mostly sold as charts rather than data exports.
- **Existing trainers** for manual spot-checking: Poker Solver Pro, Poker Preflop Trainer PRO, Poker Study AI, pokertrainer.se.

If the project is ever released publicly, use only our own solved output. Do not ship reference ranges.
