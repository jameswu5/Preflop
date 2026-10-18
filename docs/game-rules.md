# Game Abstraction (v1 defaults, all configurable)

Put all of these in one config struct, loadable from a config file. Do not hardcode them.

| Item | v1 default |
|---|---|
| Players | 6 (UTG, HJ, CO, BTN, SB, BB) |
| Blinds / stacks | 0.5 / 1, all stacks 100bb, reset every hand |
| Open size | 2.5bb (SB opens to 3bb) |
| 3-bet size | 3x the open IP, 5x OOP (from the blinds), plus 1 open-size per extra caller (squeeze) |
| 4-bet size | 2x the 3-bet IP, 2.55x OOP |
| 5-bet | all-in. There is no 6-bet. Facing all-in is call/fold only |
| Limping | None, including SB complete. Add later as a flag |
| Cold calling | Allowed only while the pot holds a single open: players may call the open, and later players may overcall the open plus earlier callers. Once a 3-bet or larger is on the table, players who have not yet raised may only fold or raise, except that a player who called the open may also call a squeeze (one 3-bet) when `squeezeCall = 1` (the default). The original raiser facing a 3-bet may fold/call/raise |
| Rake | 5% of the pot, capped at 4bb (`rake`, `rakeCap`), to match the raked online games the reference ranges come from. Taken only when a flop is dealt or an all-in is called: no flop, no drop. `rake = 0` turns it off. It doesn't change the betting tree |
| Tick | Every raise total rounds to the nearest 0.1bb (1 cent at 10NL), halves away from zero, capped at the stack |

Actions per node are a subset of `{FOLD, CALL, RAISE}`, with the raise amount determined by the config and the current raise level. Because all stacks are equal, an all-in is always a plain showdown with no side pots.

The sizes match the reference charts in `ref/` (the pokertrainer GTO range viewer): 3-bets to 7.5bb in position and 12.5bb from the blinds against a 2.5bb open, 9bb for BB against SB's 3bb open; 4-bets to 19bb against an in-position 7.5bb 3-bet, 25bb against a blind's 12.5bb, and 23bb for SB against BB's 9bb; 5-bets all-in. One OOP 4-bet factor cannot give both 19 and 23 after rounding, so 2.55 gives 19.1 (+0.1bb) and 23 exactly.

**Sizing examples** (2.5bb open, 0.1bb tick):

- BTN 3-bets a CO open (IP): 3 × 2.5 = 7.5bb.
- BB 3-bets a BTN open (OOP): 5 × 2.5 = 12.5bb. BTN 4-bets it (IP): 2 × 12.5 = 25bb.
- BTN squeezes HJ open + CO call (IP): 3 × 2.5 + 2.5 = 10bb.
- BB squeezes CO open + BTN call + SB call (OOP): 5 × 2.5 + 2 × 2.5 = 17.5bb.
- BB 3-bets an SB open (3bb, IP): 3 × 3 = 9bb. SB 4-bets it (OOP): 2.55 × 9 = 22.95 → 23bb.
- CO 4-bets a 7.5bb BTN 3-bet (OOP): 2.55 × 7.5 = 19.125 → 19.1bb. 4-bets size off the rounded 3-bet actually on the table.

**Squeeze calls:** with `squeezeCall = 1` (default) a cold caller facing a squeeze may fold, call or 4-bet; facing a 4-bet it may still only fold or raise. `squeezeCall = 0` restores the old fold-or-4-bet rule. Allowing the call doubles the tree (3,751 to 7,499 decision nodes). It was added because forbidding it made every flat expensive (blinds squeeze, the caller must fold or 4-bet), so the solver 3-bet far more and flatted far less than reference charts.

If validating against a specific reference solution, copy its sizes and rules into the config. Matching the setup matters more than any other choice.

The tree is at most about 4 raise levels deep with up to 3 actions per node, so it is small enough to solve on a laptop.
