# Game Abstraction (v1 defaults, all configurable)

Put all of these in one config struct, loadable from a config file. Do not hardcode them.

| Item | v1 default |
|---|---|
| Players | 6 (UTG, HJ, CO, BTN, SB, BB) |
| Blinds / stacks | 0.5 / 1, all stacks 100bb, reset every hand |
| Open size | 2.5bb (SB opens to 3bb) |
| 3-bet size | 3.5x the open IP, 4.5x from the blinds, plus 1 open-size per extra caller (squeeze) |
| 4-bet size | about 2.3x the 3-bet IP, about 2.75x OOP |
| 5-bet | all-in. There is no 6-bet. Facing all-in is call/fold only |
| Limping | None, including SB complete. Add later as a flag |
| Cold calling | Allowed only while the pot holds a single open: players may call the open, and later players may overcall the open plus earlier callers. Once a 3-bet or larger is on the table, players who have not yet raised may only fold or raise, except that a player who called the open may also call a squeeze (one 3-bet) when `squeezeCall = 1` (the default). The original raiser facing a 3-bet may fold/call/raise |
| Rake | Off in v1. Add later as a flag |
| Tick | Every raise total rounds to the nearest 0.1bb (1 cent at 10NL), halves away from zero, capped at the stack |

Actions per node are a subset of `{FOLD, CALL, RAISE}`, with the raise amount determined by the config and the current raise level. Because all stacks are equal, an all-in is always a plain showdown with no side pots.

**Sizing examples** (2.5bb open, 0.1bb tick):

- BTN 3-bets a CO open (IP): 3.5 × 2.5 = 8.75 → 8.8bb.
- BB 3-bets a BTN open (OOP): 4.5 × 2.5 = 11.25 → 11.3bb.
- BTN squeezes HJ open + CO call (IP): 3.5 × 2.5 + 2.5 = 11.25 → 11.3bb.
- BB squeezes CO open + BTN call + SB call (OOP): 4.5 × 2.5 + 2 × 2.5 = 16.25 → 16.3bb.
- BB 3-bets an SB open (3bb, IP): 3.5 × 3 = 10.5bb.
- CO 4-bets that 8.8bb BTN 3-bet (OOP): 2.75 × 8.8 = 24.2bb. 4-bets size off the rounded 3-bet actually on the table.

**Squeeze calls:** with `squeezeCall = 1` (default) a cold caller facing a squeeze may fold, call or 4-bet; facing a 4-bet it may still only fold or raise. `squeezeCall = 0` restores the old fold-or-4-bet rule. Allowing the call doubles the tree (3,751 to 7,499 decision nodes). It was added because forbidding it made every flat expensive (blinds squeeze, the caller must fold or 4-bet), so the solver 3-bet far more and flatted far less than reference charts.

If validating against a specific reference solution, copy its sizes and rules into the config. Matching the setup matters more than any other choice.

The tree is at most about 4 raise levels deep with up to 3 actions per node, so it is small enough to solve on a laptop.
