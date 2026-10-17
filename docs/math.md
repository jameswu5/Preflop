# The Math

This page sets out the model and the method used to solve it, as implemented. Code references: `src/solver/mccfr.*` (CFR), `src/solver/utility.*` (payoffs and realization), `src/solver/equity.*` (equity), `src/tools/tune.py` (fitting), `src/trainer/trainer.*` (rollout EVs). [solver.md](solver.md) covers data layout and threading. Current parameter values are not quoted here, because they are still being tuned. The source of truth is the code defaults.

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

**Average strategy.** Whenever a non-traverser's information set is visited, $\sigma^t(I)$ is added to its strategy sum $S(I)$, and the output strategy is $\bar\sigma(I) = S(I)/\lVert S(I) \rVert_1$. The expected contribution of iteration $t$ is proportional to $\pi^{\sigma^t}(I)\,\sigma^t(I)$, which uses the full reach of $I$. The textbook average uses only the owner's reach, $\pi^{\sigma^t}_p(I)$. The difference is the factor $\pi^{\sigma^t}_{-p}(I)$: the other players' and chance's part of the reach, which drifts only slowly once their strategies settle. This "opponent-node averaging" is the standard choice with external sampling. Information sets that are never reached keep the uniform strategy.

### Linear averaging by block discounting

Every $N$ iterations ($N$ = `SolverConfig::discountEvery`), at $t = kN$, every regret and every strategy sum is multiplied by $k/(k+1)$. Increments from block $j$ (iterations $(j-1)N < t \le jN$) are scaled by every later discount, so after $K$ blocks they carry

$$\prod_{k=j}^{K} \frac{k}{k+1} = \frac{j}{K+1} \;\propto\; j.$$

Both regrets and the average strategy therefore weight block $j$ linearly, which is Linear CFR (Brown & Sandholm, 2019) applied a block at a time: early, poorly informed iterations fade out. One multiply per table entry every $N$ iterations costs almost nothing.

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
- **Bias from finite $K$.** The share is a ratio of noisy estimates, so $\mathbb E[s_i(\widehat{\mathrm{eq}})] \ne s_i(\mathrm{eq})$ in general (Jensen), with bias $O(1/K)$. Heads-up uses the exact table, so this bias is absent there. Choose $K$ by checking that the solution stops moving as $K$ grows. Tuning runs use a smaller $K$ than the final solve, after checking that the difference this makes is small compared with the distance to the reference charts.
- **Scale.** Multiplying every $R_j$ by the same constant leaves $s_i$ unchanged, so only ratios matter. The SPR term and the floor below do depend on the absolute level, though, so the scale is fixed by convention at $R = 1$ for a baseline hand in position.

### 3.3 The realization model

$R_i$ is built from **features of the hand**, the player's **role** (in or out of position, caller or last raiser) and the **depth of the pot**. Fold equity and blockers are *not* modelled here: the solver deals real cards and samples opponents from the remaining deck, so both are already exact, and a blocker term would count them twice.

**Depth.** With $S = \text{stack behind} / \text{pot}$ at the flop (all live stacks are equal) and a parameter $\kappa \ge 0$, let

$$\gamma = \frac{S}{S + \kappa} \in (0, 1] \qquad (\kappa = 0:\ \gamma = 1).$$

$\gamma$ is near 1 in deep, single-raised pots and falls in 3-bet and 4-bet pots. At $S = \kappa$, $\gamma = \tfrac12$, so $\kappa$ reads as the "half-life" SPR. Terms scaled by $\gamma$ are those whose value comes from playing future streets, so they fade when little money is left behind.

**Position.**

$$\mathrm{pos}_i = \begin{cases} \alpha_{\text{ip}} & i \text{ acts last postflop among } L, \\ 1 - (1 - \alpha_{\text{oop}})\,\gamma & \text{otherwise.} \end{cases}$$

Postflop order is SB, BB, UTG, HJ, CO, BTN. The out-of-position penalty $1 - \alpha_{\text{oop}}$ shrinks with depth: with little behind, there is little to be out-played on.

**Hand features.** For a hand with ranks $r_1 \ge r_2$ (2 = 0, …, A = 12):

- $\mathrm{st}(r_1,r_2)$: the number of 5-card straights, from A-2-3-4-5 (the ace plays low) up to T-J-Q-K-A, that contain both ranks. It is 0 for pairs. For example 98 has 4, KQ 2, AK and A5 1, and K4 0.
- $\mathrm{dom}(r_1,r_2)$: for offsuit non-pairs with $r_1 \ge \text{T}$, $\min(r_1 - 1 - r_2,\ 4)$, and 0 otherwise. It measures how often the hand makes top pair with a weak kicker against better versions of itself. For example KQo has 0, KJo 1, AJo 2, and K6o is capped at 4.
- Indicators $\mathbb 1_{\text{suited}}$, $\mathbb 1_{\text{pair}}$, $\mathbb 1_{\text{off}}$ (offsuit non-pair), and $\mathbb 1_{\text{raiser}}$ (player $i$ made the last raise).

$$H_i = 1 + \phi\, \mathbb 1_{\text{suited}} + \eta\, \mathrm{st}(r_1,r_2)\,\gamma + \varsigma\, \mathbb 1_{\text{pair}}\,\gamma - \delta\, \mathrm{dom}(r_1,r_2)\,(1 - \mathbb 1_{\text{raiser}}) - \lambda\,(12 - r_1)\,\mathbb 1_{\text{off}} - \beta\,(12 - r_1)\,\mathbb 1_{\text{pair}}.$$

Reading the terms:

- $\phi$ (flush potential) lifts every suited hand, at any depth.
- $\eta$ (straight potential) and $\varsigma$ (set mining) pay off only when there is money left to win, so both scale with $\gamma$.
- $\delta$ (domination) applies only when **calling**. A dominated hand that calls ends up in pots with ranges that crush it, while as the aggressor it often takes the pot or is the one doing the dominating.
- $\lambda$ (low cards) charges offsuit non-pairs for each rank their top card sits below an ace.
- $\beta$ (low pairs) charges pairs for each rank below aces, at any depth: overcards come on most flops and small pairs realize badly against them in every pot. Their upside, flopping a set, is already in $\varsigma\,\gamma$. The term is there so small pairs prefer flatting (set mining) to 3-betting.

**Combined, with a floor.**

$$R_i = \max\!\big( \mathrm{pos}_i \cdot H_i \cdot (1 + \iota\, \mathbb 1_{\text{raiser}}),\ 0.05 \big),$$

where $\iota$ rewards the last aggressor. The floor keeps every $R_i$ positive, so the share denominator never vanishes.

**Why this shape.** The intended outcome is *polarized* re-raising ranges. Hands that realize badly when calling, such as dominated offsuit broadways, prefer to raise or fold. Hands whose value is postflop playability, such as suited aces, suited connectors and small pairs, get that value in deep pots, which mostly means calling. Suited wheel aces like A5s keep their straight and flush terms even as the aggressor, which is what makes them natural bluffs. An earlier model, $R$ = position × hand class with a shrink of the whole factor toward 1 in shallow pots, barely depended on role, and in 3-bet pots it pulled everything toward raw equity. The solver then built linear re-raising ranges: small pairs looked like good 3-bets, and A5s and 76s lost their edge.

**Neutral setting.** With $\phi = \eta = \varsigma = \delta = \lambda = \beta = \iota = 0$ and $\kappa = 0$, $H_i = 1$ and $R_i = \alpha_{\text{ip}}$ or $\alpha_{\text{oop}}$: a position-only model.

The fitted parameters are $\theta = (\alpha_{\text{oop}}, \phi, \eta, \varsigma, \delta, \lambda, \kappa, \iota, \beta)$, with $\alpha_{\text{ip}} = 1$ fixing the scale. In code they are the `RealizationConfig` fields `oop, flush, conn, set, dom, low, spr, init, pairLow`, with `ip` = $\alpha_{\text{ip}}$. The functions `straights()` and `domination()` in `utility.h` compute the features.

## 4. Fitting the realization parameters

The parameters are chosen so the solved strategy matches reference charts, subject to sanity constraints.

**Distance to one reference spot.** At the node $h_s$ of a reference spot $s$, give each class $k$ the weight

$$w_k = n_k\, \rho_k, \qquad \rho_k = \prod_{\substack{(h',a') \prec h_s \\ P(h') = P(h_s)}} \bar\sigma(h', k)(a'),$$

where $n_k \in \lbrace 6, 4, 12 \rbrace$ is the class's combo count and $\rho_k$ is how often our strategy brings class $k$ to the spot through the actor's own earlier actions. The distance is the weighted mean total-variation distance between our mix and the reference mix:

$$D_s(\bar\sigma) = \frac{\sum_k w_k \cdot \tfrac12 \big\lVert \bar\sigma(h_s,k) - \sigma^{\text{ref}}_s(k) \big\rVert_1}{\sum_k w_k} \in [0,1].$$

**Objective.** Average over the reference spots $\mathcal S$, plus a penalty when the open frequencies $o_p$ leave the sanity bands $[l_p, u_p]$ ([validation.md](validation.md)):

$$J(\theta) = \frac{1}{|\mathcal S|} \sum_{s \in \mathcal S} D_s\big(\bar\sigma_\theta\big) + \mu \sum_{p \in \lbrace \text{UTG}, \text{BTN} \rbrace} \operatorname{dist}\big(o_p(\bar\sigma_\theta),\ [l_p, u_p]\big),$$

with $\mu$ in score per percentage point. The bands and $\mu$ are constants in `tune.py`.

**Evaluating $\bar\sigma_\theta$.** A full solve per candidate is too slow, so each one is a short warm-started solve. Load the current 6-max checkpoint, keep its regrets, reset the strategy average and the iteration counter (`--reset-average`), run $M$ iterations with the candidate $\theta$, and use that run's average strategy. Because the average restarts, $\bar\sigma_\theta$ reflects only $\theta$, while the kept regrets start the search near equilibrium. $J$ is therefore a noisy, slightly biased function of $\theta$.

**Search.** Coordinate pattern search: from $\theta$, evaluate $\theta \pm \delta_j e_j$ for every coordinate $j$ (in parallel), move to the best candidate if it beats $J(\theta)$, and otherwise halve every $\delta_j$. This needs no gradients, tolerates noise reasonably well, and has few moving parts. That matters when each evaluation is a solve.

**Overfitting.** There are few parameters and they are interpretable. The reference set is small, though, so treat a parameter the data barely constrains (such as $\iota$) with caution: the search will move it, but the references cannot pin it down. More references for the spots it affects matter more than further fitting.

## 5. Convergence and checks

**Strategy change between checkpoints.** For successive average strategies $\bar\sigma, \bar\sigma'$, walk the tree carrying each seat's class range $r_p(k)$: the product of its own action probabilities so far. At node $h$ with actor $p$, the range-weighted change is

$$\Delta(h) = \frac{\sum_k n_k\, r_p(k)\, \tfrac12 \lVert \bar\sigma(h,k) - \bar\sigma'(h,k) \rVert_1}{\sum_k n_k\, r_p(k)}.$$

The node is weighted by its approximate reach $\hat\pi(h)$: the product along the path of each action's range-weighted frequency, $\sum_k n_k r_p(k)\,\bar\sigma'(h,k)(a) / \sum_k n_k r_p(k)$. The reported number is

$$\text{change} = \frac{\sum_h \hat\pi(h)\, \Delta(h)}{\sum_h \hat\pi(h)}.$$

This is a reach- and range-weighted total-variation distance: frequent spots and frequent hands count most. Without a convergence guarantee, the practical stopping rule is that this number is small and no longer falling.

**Indifference via rollouts.** At an equilibrium of the model, every action a hand mixes between has equal value, and the solved mix loses nothing against the best action. The trainer estimates action values for a concrete spot. It re-deals the opponents $N$ times from the deck minus the user's cards, and weights deal $n$ by the probability that the bots would have produced the observed history with those cards:

$$\widehat{\mathrm{EV}}(a) = \frac{\sum_{n} \omega_n\, u^{(n)}(a)}{\sum_n \omega_n}, \qquad \omega_n = \prod_{k\,:\,P(h_k) \ne \text{user}} \bar\sigma\big(h_k,\ b(\omega^{(n)}_{P(h_k)})\big)(a_k).$$

This is self-normalized importance sampling: consistent as $N$ grows, with $O(1/N)$ bias. Every legal action is played out on the same deals (common random numbers), so differences between actions have lower variance than their levels. Checking that $\max_a \widehat{\mathrm{EV}}(a) - \sum_a \bar\sigma(a)\,\widehat{\mathrm{EV}}(a) \approx 0$ across many dealt spots is a direct test that the solve is at an equilibrium of the model. The heads-up game (`--seats 2`) with rake off is the clean case for this test, since it is two-player zero-sum and CFR is guaranteed to converge there. With rake, heads-up is general-sum and the guarantee no longer applies, although the test still makes sense.

## 6. The pipeline that produces the shipped strategy

1. **Equity table.** `gen_equity` writes the exact heads-up table (§3.1). It never changes.
2. **Validate the solver on a small game.** Solve SB vs BB with rake off (a config with `rake = 0`), where CFR has its guarantee, and run the indifference check (§5).
3. **Fit $\theta$.** Run `tune.py` against the reference charts in `ref/` (§4).
4. **Re-solve.** Copy the fitted values into the `RealizationConfig` defaults and run a long 6-max solve from scratch at the full board count $K$ (the command is in the README). The strategy file records the $\theta$ it was solved with, so the trainer's rollouts use the same payoff model.
5. **Check.** Watch the strategy change (§5), run `solve --report` for the sanity metrics, and run `validate` for the per-spot distances to the references ([validation.md](validation.md#tools)).
