# The Math

This page sets out the model and the method used to solve it, as implemented. Code references: `src/solver/mccfr.*` (CFR), `src/solver/utility.*` (payoffs and realization), `src/solver/equity.*` (equity), `src/tools/tune.py` (fitting), `src/trainer/trainer.*` (rollout EVs). [solver.md](solver.md) covers data layout and threading. Parameter values are not quoted here: the source of truth is the code defaults, which hold the values fitted by `bo_tune.py` (§4).

In outline:

1. Define a finite preflop game whose terminal payoffs approximate the postflop game through **realization-adjusted equity**.
2. Solve it with **external-sampling Monte Carlo CFR** with linear averaging.
3. Fit the realization parameters so the solution matches reference charts, then re-solve with the fitted values.

## 1. The game

**Players and actions.** Seats $p \in \lbrace \text{UTG}, \text{HJ}, \text{CO}, \text{BTN}, \text{SB}, \text{BB} \rbrace$, all with 100bb, blinds 0.5/1. Every decision is a subset of $\lbrace F, C, R \rbrace$, and the raise size is a function of the history ([game-rules.md](game-rules.md)). The game tree is finite and fixed. Write $H$ for its decision nodes (histories), $Z$ for its terminals, $P(h)$ for the player to act at $h$ and $A(h)$ for the legal actions.

**Chance.** A deal $\omega = (\omega_p)_p$ assigns six disjoint two-card combos from one 52-card deck, uniformly at random. Boards are dealt only inside the payoff function (§3).

**Information sets.** A player sees the public history and their own cards. We abstract the cards to one of the 169 strategic classes $b(\omega_p)$ (pairs, suited and offsuit non-pairs), so an information set is

$$I = \big(h,\ b(\omega_{P(h)})\big), \qquad |\mathcal I| = |H| \times 169.$$

The class is fixed for the whole hand, so the abstraction keeps perfect recall. Strategies are stored per class, but the solver always deals real combos from one deck, so card removal (blockers) still shapes the sampled opponents. A behavioural strategy is $\sigma_p(I) \in \Delta(A(h))$ at each of player $p$'s information sets.

**Payoffs.** At a terminal $z$ with deal $\omega$, player $i$ gets $u_i(z,\omega) = s_i \cdot (\text{pot} - \varrho) - c_i$, where $c_i$ is what $i$ contributed, $s_i$ is $i$'s share of the pot and $\varrho$ is the rake (§3).

## 2. Solving: external-sampling MCCFR

### Counterfactual regret

Write $\pi^\sigma(h)$ for the probability of reaching $h$ under $\sigma$, chance included. It factors as $\pi^\sigma_i(h)\,\pi^\sigma_{-i}(h)$: player $i$'s own action probabilities times everyone else's, chance included. The **counterfactual value** of an information set $I$ of player $i$ is the expected payoff from $I$, weighted by the probability that *the others* bring play there:

$$v_i^\sigma(I) = \sum_{h \in I} \pi^\sigma_{-i}(h) \sum_{z \sqsupseteq h} \pi^\sigma(h, z)\, u_i(z),$$

and $v_i^\sigma(I,a)$ is the same with $a$ forced at $I$. After $T$ iterations the cumulative regret is

$$R^T(I,a) = \sum_{t=1}^{T} w_t \big( v_i^{\sigma^t}(I,a) - v_i^{\sigma^t}(I) \big).$$

The weights $w_t$ are set by the discounting described below.

### Regret matching

The next strategy plays in proportion to positive regret:

$$\sigma^{T+1}(I,a) = \begin{cases} \dfrac{R^T_+(I,a)}{\sum_{a'} R^T_+(I,a')} & \text{if } \sum_{a'} R^T_+(I,a') > 0, \\[2ex] \dfrac{1}{|A(I)|} & \text{otherwise,} \end{cases} \qquad x_+ = \max(x,0).$$

The stored regrets are *not* floored at zero, so negative regret can build up and later be paid back. The CFR+ variant floors them, and that is known to interact badly with Monte Carlo sampling.

### Sampling one iteration

Exact counterfactual values need a sum over every deal and every opponent action, which is far too expensive. External sampling estimates them from one sampled trajectory per iteration:

1. Draw a deal $\omega$.
2. Pick the **traverser** $i = t \bmod n$ cyclically over the $n$ seats in the game.
3. Walk the tree from the root:
   - at the traverser's nodes, **enumerate** every $a \in A(I)$ and recurse;
   - at everyone else's nodes, **sample one** action $a \sim \sigma^t(I)$;
   - at terminals, return a sampled payoff (§3).
4. At each traverser node, with sampled action values $\tilde v(I,a)$ and $\tilde v(I) = \sum_a \sigma^t(I,a)\,\tilde v(I,a)$, add $\tilde v(I,a) - \tilde v(I)$ to the regret.

Opponents and chance are sampled with exactly their probabilities $\pi_{-i}$. The importance weight $\pi_{-i}/q$ is therefore 1, and the sampled regret increment is an unbiased estimate of the true counterfactual regret increment (Lanctot et al., 2009). The one caveat is the multiway flop payoff, whose estimator has a small bias (§3.2).

**Average strategy.** Whenever a non-traverser's information set is visited, $\sigma^t(I)$ is added to its strategy sum $S(I)$, and the output strategy is $\bar\sigma(I) = S(I)/\lVert S(I) \rVert_1$. The expected contribution of iteration $t$ is proportional to $\pi^{\sigma^t}(I)\,\sigma^t(I)$, which uses the full reach of $I$. The textbook average uses only the owner's reach, $\pi^{\sigma^t}_p(I)$. The difference is the factor $\pi^{\sigma^t}_{-p}(I)$: the other players' and chance's part of the reach, which drifts only slowly once their strategies settle. This "opponent-node averaging" is the standard choice with external sampling. An information set whose strategy sum stays zero is reached only through its owner's own enumeration, after an action that player never takes. Its output is the regret-matched strategy $\sigma^{T+1}(I)$ instead, which is uniform only if it was never reached at all.

**Chart-locked entries.** For a solve anchored to reference charts ([solver.md](solver.md#chart-locked-solving)), chosen $(h, k)$ entries play fixed probabilities: they are sampled and enumerated as usual, but their strategy sums are never updated and the output keeps the fixed values. Their regrets still accumulate, never played, as a measure of how far the fixed play is from a best response to everything else.

### Linear averaging by block discounting

Every $N$ iterations ($N$ = `SolverConfig::discountEvery`), at $t = kN$, every regret and every strategy sum is multiplied by $k/(k+1)$. Increments from block $j$ (iterations $(j-1)N < t \le jN$) are scaled by every later discount, so after $K$ blocks they carry

$$\prod_{k=j}^{K} \frac{k}{k+1} = \frac{j}{K+1} \;\propto\; j.$$

Both regrets and the average strategy therefore weight block $j$ linearly, which is Linear CFR (Brown & Sandholm, 2019) applied a block at a time: early, poorly informed iterations fade out. The code never rescales the tables: it adds each increment of block $j$ multiplied by $j$, so after $K$ blocks they hold $(K+1)$ times the discounted sums. Regret matching and $\bar\sigma$ are invariant to that common factor.

### Caveats for six players

- CFR's guarantee is that average regret goes to 0, so the average strategy becomes an $\varepsilon$-equilibrium. That conclusion holds only in **two-player zero-sum** games. Our game is six-player and, with rake, not even zero-sum among the players (§3), so there is no convergence guarantee, and a limit point need not be a Nash equilibrium. In practice this family of methods produces strong strategies (Pluribus is an example). We check stability empirically instead (§5).
- Everything is relative to the model: an equilibrium of *our* abstraction and payoff model. Close agreement with reference charts is evidence, not proof of GTO.
- Rake makes the game negative-sum for the players. One can view the house as a seventh, passive player who collects $\varrho$ and never acts, which restores zero-sum but adds nothing to the strategic picture. Exact checks that rely on zero-sum, such as the push/fold exploitability test in `solver_test`, run with rake off.

## 3. Terminal payoffs

Let $L$ be the set of players still in the hand at a terminal. Then

$$u_i = s_i \cdot (\text{pot} - \varrho) - c_i, \qquad \sum_{i \in L} s_i = 1, \qquad s_i = 0 \text{ for } i \notin L.$$

**Rake** (no flop, no drop). With rate $r$ and cap $\bar\varrho$ (Config `rake`, `rakeCap`),

$$\varrho = \begin{cases} \min\!\big(r \cdot \text{pot},\ \bar\varrho\big) & |L| \ge 2 \text{ (a flop is dealt, or an all-in is called)}, \\ 0 & |L| = 1 \text{ (everyone else folded preflop).} \end{cases}$$

The rake comes off the whole pot, dead money from folded players included, and the live players split what is left in the usual proportions $s_i$. Since $\sum_i c_i = \text{pot}$,

$$\sum_i u_i = (\text{pot} - \varrho) - \text{pot} = -\varrho$$

for **every** terminal and every sampled board, not just in expectation. With $r = 0$ the game is exactly zero-sum. Both identities are tested directly.

Strategically, rake is a tax on seeing a flop, and it does not depend on who wins. Because the cap binds only in big pots, it costs most, relative to the pot, in small and medium pots. That makes marginal flats and loose blind defence worse, while folds, uncontested raises and preflop all-ins large enough to hit the cap are taxed little or not at all.

| Terminal | Share $s_i$ |
|---|---|
| All others folded | $s_i = 1$ for the last player |
| All-in called | $s_i = \mathrm{eq}_i$, the showdown share on one sampled board (exact table equity if heads-up) |
| Action closed, flop would be dealt | $s_i = \dfrac{R_i\, \mathrm{eq}_i}{\sum_{j \in L} R_j\, \mathrm{eq}_j}$ |

### 3.1 Equity

For combos $x$ and $y$ and a board $B$, write $x \succ_B y$ when $x$ makes the better seven-card hand and $x \sim_B y$ when they tie. Heads-up equity is

$$\mathrm{eq}(x,y) = \frac{1}{\binom{48}{5}} \sum_{B} \Big( \mathbb 1[x \succ_B y] + \tfrac12\, \mathbb 1[x \sim_B y] \Big),$$

an exact sum over every board. `gen_equity` precomputes it for every pair of disjoint combos. Matchups are computed once per suit relabeling and swap of sides, and the result is stored as a $1326 \times 1326$ table. The table ignores the folded players' cards. Folders' hands skew weak, so this drops a small dead-card effect.

Multiway there is no table. With $K$ boards $B_1,\dots,B_K$ drawn from the deck minus all twelve hole cards, and $W_k \subseteq L$ the winners on $B_k$,

$$\widehat{\mathrm{eq}}_i = \frac{1}{K} \sum_{k=1}^{K} \frac{\mathbb 1[i \in W_k]}{|W_k|}.$$

All-in terminals use $K = 1$ with the *actual* pot share. That estimator is unbiased for the expected payoff, and averaging over iterations removes the noise.

### 3.2 Realization-adjusted shares

When a flop would be dealt, raw equity overstates what some hands actually win postflop: being out of position, or holding offsuit, unconnected cards, both cost money. $R_i > 0$ scales each player's equity, and the shares are renormalized over $L$.

- **Why one board is not enough.** With $K = 1$, every $\widehat{\mathrm{eq}}_j \in \lbrace 0, 1 \rbrace$ (ignoring ties), and $s_i$ collapses to $\mathbb 1[i \text{ wins}]$: the $R$ factors cancel. So the flop terminal needs a genuine equity estimate, not one board.
- **Bias from finite $K$.** The share is a ratio of noisy estimates, so $\mathbb E[s_i(\widehat{\mathrm{eq}})] \ne s_i(\mathrm{eq})$ in general (Jensen), with bias $O(1/K)$. Heads-up uses the exact table, so this bias is absent there. Choose $K$ by checking that the solution stops moving as $K$ grows. Tuning may use a smaller $K$ than the final solve, after checking that the difference this makes is small compared with the distance to the reference charts.
- **Scale.** Multiplying every $R_j$ by the same constant leaves $s_i$ unchanged, so only ratios matter. The SPR term and the floor below do depend on the absolute level, though, so the scale is fixed by convention at $R = 1$ for a baseline hand in position.

### 3.3 The realization model

$R_i$ is built from **features of the hand**, the player's **role** (in or out of position, caller or last raiser) and the **depth of the pot**. Fold equity and blockers are *not* modelled here: the solver deals real cards and samples opponents from the remaining deck, so both are already exact, and a blocker term would count them twice.

**Depth.** With $S = \text{stack behind} / \text{pot}$ at the flop (all live stacks are equal) and a parameter $\kappa \ge 0$, let

$$\gamma = \frac{S}{S + \kappa} \in (0, 1] \qquad (\kappa = 0:\ \gamma = 1).$$

$\gamma$ is near 1 in deep, single-raised pots and falls in 3-bet and 4-bet pots. At $S = \kappa$, $\gamma = \tfrac12$, so $\kappa$ reads as the "half-life" SPR. Terms scaled by $\gamma$ are those whose value comes from playing future streets, so they fade when little money is left behind.

**Position.**

Call player $i$ an **out-of-position caller** if $i$ neither acts last postflop among $L$ nor made the last raise. Such a player has neither position nor initiative. Call $i$ an **in-position caller** if $i$ acts last but did not make the last raise. Call the pot **re-raised** if it was 3-bet or more ($\mathbb 1_{\text{rr}} = 1$); this is read from the betting history (the number of raises), not from the SPR, so it does not depend on the sizes.

$$\mathrm{pos}_i = \begin{cases} \alpha_{\text{ip}}\,\chi_i & i \text{ acts last postflop among } L, \\ 1 - (1 - \alpha_{\text{oop}})\,\gamma' & \text{otherwise,} \end{cases} \qquad \chi_i = \begin{cases} \chi & \text{in-position caller in a re-raised pot,} \\ 1 & \text{otherwise,} \end{cases} \qquad \gamma' = \begin{cases} \xi\,\gamma + (1 - \xi) & \text{out-of-position caller,} \\ \gamma & \text{otherwise.} \end{cases}$$

Postflop order is SB, BB, UTG, HJ, CO, BTN. The out-of-position penalty $1 - \alpha_{\text{oop}}$ shrinks with depth: with little behind, there is little to be out-played on. For out-of-position callers, $\xi \in [0,1]$ controls how much of that shrink applies. With $\xi = 1$ they are treated like everyone else; with $\xi = 0$ they pay the full penalty at any depth. The idea is that calling a 3-bet out of position stays costly even in a shallow pot, because the caller still faces a c-bet with no initiative. An out-of-position *raiser* keeps the full shrink.

$\chi$ is the in-position counterpart for re-raised pots. Calling a 3-bet or 4-bet in position means continuing with a capped range into a smaller SPR against the aggressor's initiative, and position is worth less there than in a single-raised pot. With $\chi = 1$ position is valued the same in every pot. It does not touch single-raised pots, where in-position flats are the main way to play many hands.

**Hand features.** For a hand with ranks $r_1 \ge r_2$ (2 = 0, …, A = 12):

- $\mathrm{st}(r_1,r_2)$: the number of 5-card straights, from A-2-3-4-5 (the ace plays low) up to T-J-Q-K-A, that contain both ranks. It is 0 for pairs. For example 98 has 4, KQ 2, AK and A5 1, and K4 0.
- $\mathrm{dom}(r_1,r_2)$: for offsuit non-pairs with $r_1 \ge \text{T}$, $\min(r_1 - 1 - r_2,\ 4)$, and 0 otherwise. It measures how often the hand makes top pair with a weak kicker against better versions of itself. For example KQo has 0, KJo 1, AJo 2, and K6o is capped at 4.
- Indicators $\mathbb 1_{\text{suited}}$, $\mathbb 1_{\text{pair}}$, $\mathbb 1_{\text{off}}$ (offsuit non-pair), and $\mathbb 1_{\text{raiser}}$ (player $i$ made the last raise).

$$H_i = 1 + \big(\phi\, \mathbb 1_{\text{suited}} + \eta\, \mathrm{st}(r_1,r_2)\,\gamma + \varsigma\, \mathbb 1_{\text{pair}}\,\gamma\big)\,\psi_i - (\delta + \delta_{\text{rr}}\,\mathbb 1_{\text{rr}})\, \mathrm{dom}(r_1,r_2)\,(1 - \mathbb 1_{\text{raiser}}) - \lambda\,(12 - r_1)\,\mathbb 1_{\text{off}} - \beta\,(12 - r_1)\,\mathbb 1_{\text{pair}}.$$

where $\psi_i = \psi$ for an out-of-position caller and $\psi_i = 1$ otherwise.

Reading the terms:

- $\phi$ (flush potential) lifts every suited hand, at any depth.
- $\eta$ (straight potential) and $\varsigma$ (set mining) pay off only when there is money left to win, so both scale with $\gamma$.
- $\delta$ (domination) applies only when **calling**. A dominated hand that calls ends up in pots with ranges that crush it, while as the aggressor it often takes the pot or is the one doing the dominating. $\delta_{\text{rr}}$ adds to it in re-raised pots, where the raiser's range is stronger and narrower, so a dominated caller is up against its better versions more often.
- $\lambda$ (low cards) charges offsuit non-pairs for each rank their top card sits below an ace.
- $\beta$ (low pairs) charges pairs for each rank below aces, at any depth: overcards come on most flops and small pairs realize badly against them in every pot. Their upside, flopping a set, is already in $\varsigma\,\gamma$. The term is there so small pairs prefer flatting (set mining) to 3-betting.
- $\psi$ (out-of-position calling) scales the three playability bonuses for out-of-position callers. Flush, straight and set potential are worth less without position or initiative to play them. With $\psi < 1$, a hand like A5s gains less from calling a 3-bet out of position than from 4-betting, which is where its blockers and playability work together.

**Combined, with a floor.**

$$R_i = \max\!\big( \mathrm{pos}_i \cdot H_i \cdot (1 + \iota\, \mathbb 1_{\text{raiser}}),\ 0.05 \big),$$

where $\iota$ rewards the last aggressor. The floor keeps every $R_i$ positive, so the share denominator never vanishes.

**Why this shape.** The intended outcome is *polarized* re-raising ranges. Hands that realize badly when calling, such as dominated offsuit broadways, prefer to raise or fold. Hands whose value is postflop playability, such as suited aces, suited connectors and small pairs, get that value in deep pots, which mostly means calling. Suited wheel aces like A5s keep their straight and flush terms even as the aggressor, which is what makes them natural bluffs. An earlier model, $R$ = position × hand class with a shrink of the whole factor toward 1 in shallow pots, barely depended on role, and in 3-bet pots it pulled everything toward raw equity. The solver then built linear re-raising ranges: small pairs looked like good 3-bets, and A5s and 76s lost their edge.

**Neutral setting.** With $\phi = \eta = \varsigma = \delta = \delta_{\text{rr}} = \lambda = \beta = \iota = 0$, $\kappa = 0$ and $\psi = \xi = \chi = 1$, $H_i = 1$ and $R_i = \alpha_{\text{ip}}$ or $\alpha_{\text{oop}}$: a position-only model.

The fitted parameters are $\theta = (\alpha_{\text{oop}}, \phi, \eta, \varsigma, \delta, \lambda, \kappa, \iota, \beta, \psi, \xi, \chi, \delta_{\text{rr}})$, with $\alpha_{\text{ip}} = 1$ fixing the scale. In code they are the `RealizationConfig` fields `oop, flush, conn, set, dom, low, spr, init, pairLow, oopCall, callerShrink, ipCall, domPot`, with `ip` = $\alpha_{\text{ip}}$. The functions `straights()` and `domination()` in `utility.h` compute the features.

### 3.4 Measured realization (version 2)

Version 2 replaces the formula with a table measured from postflop solves where a table entry exists (`realization_table.h`, built by the `realization_table` tool).

**Coverage.** A flop terminal is covered when it is heads-up, not all-in, one to three raises deep, and no folded player put in more than their blind (`headsUpFlop()` in `utility.h`). These are the open, 3-bet and 4-bet lines, one per (pot type, last raiser, caller). Squeezes, cold calls of a 3-bet and cold 4-bets leave a folder's chips in the pot, so they are not covered. An entry also records the pot and stack at the flop, and it matches only a terminal with the same values, so a table built for other bet sizes never applies.

**Measurement.** For a covered spot, both players' flop ranges are read from a preflop strategy: each combo is weighted by the probability that its own actions along the line were taken. Each flop $f$ of a sample of canonical flops, with multiplicity $m_f$ (the number of flops it stands for up to suit isomorphism), is solved postflop. The solve gives each combo $x$ a weight $w_{f,x}$ (its reach after card removal against the flop and the opponent's range, normalised per player), its equity $\mathrm{eq}_{f,x}$ against the opponent's range, and its value $\mathrm{ev}_{f,x}$ from the flop on (its pot share plus postflop chips won minus postflop chips put in). For a class $k$ and a role (last raiser or caller),

$$R_k = \frac{\sum_f m_f \sum_{x \in k} w_{f,x}\, \mathrm{ev}_{f,x}}{\mathrm{pot} \cdot \sum_f m_f \sum_{x \in k} w_{f,x}\, \mathrm{eq}_{f,x}}, \qquad n_k = \sum_f m_f \sum_{x \in k} w_{f,x}.$$

It is a ratio of sums over flops, not an average of per-flop ratios. On a single flop a class with little equity has a tiny denominator, so its per-flop ratio is dominated by noise; summing first weights each flop by the equity actually at stake. $n_k$ is kept with $R_k$ as the evidence behind it, for shrinkage.

**Payoffs.** At a covered terminal, $R_i$ is the entry's value for player $i$'s role and class, and the shares are renormalised as before, $s_i = R_i\,\mathrm{eq}_i / \sum_j R_j\,\mathrm{eq}_j$. Everywhere else, and for a class the entry has no data for ($n_k = 0$), $R_i$ comes from the formula of §3.3. Without a table every payoff is exactly the formula's.

**A known approximation.** The table's $R$ is measured against equity versus the opponent's *range* on each flop. The payoff applies it to equity versus the opponent's *actual hand*, and renormalises per matchup. The renormalised share is not linear in equity, so its average over the opponent's range is not exactly $R$ times the range equity. And a hand's realization depends on what it is up against, a dependence the table averages away.

**Mixed scales.** A table $R$ is an absolute pot share per unit of equity, while the formula's $R$ is fixed only up to a common scale ($\alpha_{\text{ip}} = 1$). When one player's class falls back to the formula and the other's does not, the two are on different scales in the same pot. The fitted model of the next phase fills absent classes so that this does not happen.

**Strategy files.** A strategy solved with a table stores the whole table in the file, as it stores $\theta$, so trainer rollouts use the same payoff model as the solve.

### 3.5 Heuristic realization (version 2)

The `heuristic_table` tool builds the same table as §3.4 with the postflop solve replaced by a fixed threshold-bot rollout. It is a cheaper postflop model, built to be compared with the solver-built table entry by entry.

**Strength against a range.** A player's strength at a decision is the equity of their combo against the opponent's range *as narrowed by the opponent's actions so far*, exact over the remaining cards (`BoardEquity`). It is never measured against the opponent's actual cards. If it were, nobody would fold a winning hand, and every hand would collapse back to its raw equity.

**The rules.** Players act in postflop order. With $e$ the actor's equity and value, bluff and raise thresholds $v$, $b$ and $\rho$:

- *No bet faced:* bet when $e \ge v$ or $e \le b$, otherwise check. The out-of-position player bets only with the initiative or when nobody holds it, and otherwise checks to the aggressor. The initiative starts with the preflop raiser, passes to whoever bets or raises, and is cleared when a street checks through.
- *Facing a bet:* raise when $e \ge \rho$ (one raise per street). Otherwise call when $e \ge c / (P + c) + \mu$, where $c$ is the amount to call, $P$ the pot including the bet and $\mu$ a margin, and fold otherwise. Facing a raise: call or fold by the same test.
- Bets are a fixed fraction of the pot per street and raises a fixed multiple of the bet, both capped at the stack. The sizes match the solver path's tree.
- The hand settles at the true equity of the two holdings over the remaining cards. That happens after the last street played (one to three streets), or as soon as a player is all in.

**Exact, factorised settlement.** The rules are deterministic, and they read only a player's own combo and the public history. So whether a combo reaches a node depends on that combo alone, and the pairs reaching a node are a product $X_T \times Y_T$ of one set per player. Each node therefore needs one range-against-range pass per player rather than a sum over pairs:

- The actor's decision at a node partitions their range by action, and each part becomes the actor's range in the corresponding child.
- At a fold, combo $x$ of player $p$ collects its payoff times the opponent's reach weight disjoint from $x$.
- At a showdown, $x$ collects $\sum_y w_y\, \mathrm{eq}(x, y) \cdot \text{pot} - c_p \sum_y w_y$, where $c_p$ is $p$'s postflop investment.

Turn and river cards are enumerated. Combos holding the dealt card drop out of both ranges, and each pair of hands is weighted by one over the number of cards left to it. No hands are sampled, so a flop's result has no Monte Carlo noise.

**Aggregation.** From the rollout, each combo's ev, equity and weight are defined exactly as for a solve. Aggregation over flops then uses the same ratio of sums, the same flop sample and multiplicities, and the same code (`RealizationAccum`), so the two tables differ only in the postflop model. Two checks anchor it:

- A bot that never bets realizes exactly its equity, $R = 1$ for every class.
- On a single street, every combo's ev matches a replay of each pair of holdings through its own line.

## 4. Fitting the realization parameters

The parameters are chosen so the solved strategy matches reference charts, subject to sanity constraints.

**Distance to one reference spot.** At the node $h_s$ of a reference spot $s$, give each class $k$ the weight

$$w_k = n_k\, \rho_k, \qquad \rho_k = \prod_{\substack{(h',a') \prec h_s \\ P(h') = P(h_s)}} \bar\sigma(h', k)(a'),$$

where $n_k \in \lbrace 6, 4, 12 \rbrace$ is the class's combo count and $\rho_k$ is how often our strategy brings class $k$ to the spot through the actor's own earlier actions. The distance is the weighted mean total-variation distance between our mix and the reference mix:

$$D_s(\bar\sigma) = \frac{\sum_k w_k \cdot \tfrac12 \big\lVert \bar\sigma(h_s,k) - \sigma^{\text{ref}}_s(k) \big\rVert_1}{\sum_k w_k} \in [0,1].$$

**Objective.** Average over the reference spots $\mathcal S$, plus a penalty when the open frequencies $o_p$ leave the sanity bands $[l_p, u_p]$ ([validation.md](validation.md)):

$$J(\theta) = \frac{1}{|\mathcal S|} \sum_{s \in \mathcal S} D_s\big(\bar\sigma_\theta\big) + \mu \sum_{p \in \lbrace \text{UTG}, \text{BTN} \rbrace} \operatorname{dist}\big(o_p(\bar\sigma_\theta),\ [l_p, u_p]\big),$$

with $\mu$ in score per percentage point. The bands and $\mu$ are constants in `tune.py`.

**Evaluating $\bar\sigma_\theta$.** Each candidate is a fresh full solve of $M$ iterations with $\theta$, and $J$ is averaged over a few seeds. A fresh solve is slower than a short warm-started one (keep the regrets of a converged solve, restart the average, run a few iterations), but the warm-started average mostly measures how fast the solve moves away from the old solution, not where $\theta$ leads, and ranking on it stalled. $J$ at finite $M$ is still noisy and slightly biased (the solve is not fully converged), but the bias is shared by candidates and largely cancels in comparisons.

**Search.** Coordinate pattern search: from $\theta$, evaluate $\theta \pm \delta_j e_j$ for every coordinate $j$. Every coordinate whose better step beats $J(\theta)$ by more than $z$ standard errors moves, all together. If the combined point turns out worse than the best single move, it is replaced by that move. With no significant move, every $\delta_j$ halves (and $M$ may grow). The standard error of a difference uses the run-to-run spread pooled over all candidates of the round, since a couple of seeds per candidate cannot estimate their own. Every candidate is compared with the same estimate of $J(\theta)$, so a noisy one shifts them all. The winner is therefore re-scored against the start point at the end. This needs no gradients, tolerates noise reasonably well, and has few moving parts. That matters when each evaluation is a solve.

**Bayesian search** (`bo_tune.py`, which sets the current defaults). Model $J$ over the whole box with a Gaussian process $J(\theta) \sim \mathcal{GP}(m, k) + \varepsilon$, with a Matérn-5/2 kernel $k$ with one length scale per coordinate and Gaussian noise $\varepsilon$ for the run-to-run spread. After $n$ evaluations the posterior gives a mean $\mu_n(\theta)$ and standard deviation $s_n(\theta)$, and the next point maximises the expected improvement over the best score observed so far, $J^*$, by a margin $\xi$,
$$\mathrm{EI}(\theta) = \mathbb{E}\big[\max(J^* - \xi - J(\theta), 0)\big] = (J^* - \xi - \mu_n)\,\Phi(u) + s_n\,\varphi(u), \qquad u = \frac{J^* - \xi - \mu_n}{s_n},$$
which trades a low predicted score against high uncertainty. Each evaluation is one solve with one seed: the noise term pools the spread over nearby points, so repeat seeds are unnecessary, and all coordinates move together, so interactions are modelled rather than found one coordinate at a time. The result is the evaluated point with the lowest $\mu_n$, which is then confirmed with several seeds, paired with the start point, at full $M$.

**Overfitting.** There are few parameters and they are interpretable. The reference set is small, though, so treat a parameter the data barely constrains (such as $\iota$) with caution: the search will move it, but the references cannot pin it down. More references for the spots it affects matter more than further fitting.

## 5. Convergence and checks

**Strategy change between checkpoints.** For successive average strategies $\bar\sigma, \bar\sigma'$, walk the tree carrying each seat's class range $r_p(k)$: the product of its own action probabilities so far. At node $h$ with actor $p$, the range-weighted change is

$$\Delta(h) = \frac{\sum_k n_k\, r_p(k)\, \tfrac12 \lVert \bar\sigma(h,k) - \bar\sigma'(h,k) \rVert_1}{\sum_k n_k\, r_p(k)}.$$

The node is weighted by its approximate reach $\hat\pi(h)$: the product along the path of each action's range-weighted frequency, $\sum_k n_k r_p(k)\,\bar\sigma'(h,k)(a) / \sum_k n_k r_p(k)$. The reported number is

$$\text{change} = \frac{\sum_h \hat\pi(h)\, \Delta(h)}{\sum_h \hat\pi(h)}.$$

This is a reach- and range-weighted total-variation distance: frequent spots and frequent hands count most. Without a convergence guarantee, the practical stopping rule is that this number is small and no longer falling.

**Indifference via rollouts.** At an equilibrium of the model, every action a hand mixes between has equal value, and the solved mix loses nothing against the best action. The trainer estimates action values for a concrete spot. It re-deals the opponents $N$ times from the deck minus the user's cards, and weights deal $n$ by the probability that the bots would have produced the observed history with those cards:

$$\widehat{\mathrm{EV}}(a) = \frac{\sum_{n} \omega_n\, u^{(n)}(a)}{\sum_n \omega_n}, \qquad \omega_n = \prod_{k\,:\,P(h_k) \ne \text{user}} \bar\sigma\big(h_k,\ b(\omega^{(n)}_{P(h_k)})\big)(a_k).$$

This is self-normalized importance sampling: consistent as $N$ grows, with $O(1/N)$ bias. Its precision depends on the effective sample size $(\sum_n \omega_n)^2 / \sum_n \omega_n^2$, not on $N$: a rare history leaves few deals with weight. So the trainer keeps dealing until the effective sample size reaches a target, or a cap on $N$. It reports the delta-method standard error, $\sqrt{\sum_n \omega_n^2 (u^{(n)} - \widehat{\mathrm{EV}})^2} / \sum_n \omega_n$. Every legal action is played out on the same deals (common random numbers), so differences between actions have lower variance than their levels. Their standard errors include the covariance, and grading does not call a choice a mistake when its EV loss is within about two of them. Checking that $\max_a \widehat{\mathrm{EV}}(a) - \sum_a \bar\sigma(a)\,\widehat{\mathrm{EV}}(a) \approx 0$ across many dealt spots is a direct test that the solve is at an equilibrium of the model. The heads-up game (`--seats 2`) with rake off is the clean case for this test, since it is two-player zero-sum and CFR is guaranteed to converge there. With rake, heads-up is general-sum and the guarantee no longer applies, although the test still makes sense.

## 6. The pipeline that produces the shipped strategy

1. **Equity table.** `gen_equity` writes the exact heads-up table (§3.1). It never changes.
2. **Validate the solver on a small game.** Solve SB vs BB with rake off (a config with `rake = 0`), where CFR has its guarantee, and run the indifference check (§5).
3. **Fit $\theta$.** Run `bo_tune.py` (or `tune.py`) against the reference charts in `ref/` (§4).
4. **Re-solve.** Copy the fitted values into the `RealizationConfig` defaults and re-run the Scratch solve, then the Anchored one, at the full board count $K$ (commands in the README and solver.md). The strategy file records the $\theta$ it was solved with, so the trainer's rollouts use the same payoff model.
5. **Check.** Watch the strategy change (§5), run `solve --report` for the sanity metrics, and run `validate` for the per-spot distances to the references ([validation.md](validation.md#tools)).
