#!/usr/bin/env python3
"""Fits the realization factors (utility.h RealizationConfig) to the reference charts in ref/
(docs/validation.md "Tune", docs/math.md §4) by coordinate search.

Each candidate R is a fresh 6-max solve (no warm start: short warm-started runs ranked candidates
by how fast they move away from the old solution, not by where they end up), --threads per solve,
--jobs solves at a time (default one solve on every core, sequentially). It is scored with
`validate` (category-balanced mean distance over ref/, lower is better) plus a penalty for each
sanity-check band the opens fall outside.

Each round scores +/- one step on every parameter (plus the current point when its score is not
known at this iteration count). Every parameter whose step beats the current point by more than --z
standard errors moves together; the combined point is checked as the next round's current point and
replaced by the best single move if it turns out worse. With no improvement the steps halve.
Iterations follow --schedule; each halving moves one entry on. Each candidate is the mean of --seeds
runs, the same seeds for every candidate. The standard error of a difference comes from the run-to-run
spread pooled over all candidates in the round (with one seed: --noise), because a few paired
differences give a poor estimate of their own spread, and multithreaded runs aren't reproducible
anyway. At the end the winner and the start point are re-scored at the last schedule entry.

Usage: src/tools/tune.py [--schedule 5e7,1e8] [--seeds 2] [--rounds 10] [--z 2] [--threads N] [--jobs 1]
                         [--boards 100] [--holdout text] [--ref dir] [--noise 0.002]
--holdout leaves reference files whose name contains `text` out of the fit and scores them separately
at the end, to check the fit generalizes (e.g. vs_5bet, _sb). --ref picks the chart directory.
"""
import argparse
import concurrent.futures as cf
import os
import re
import subprocess
import tempfile
import statistics
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.join(ROOT, "build")
REF = os.path.join(ROOT, "ref")  # --ref
# Start point = current defaults in utility.h; step = initial search step per parameter.
START = {"oop": 0.650545, "flush": 0.22685, "conn": 0.08041, "set": 0.2131, "dom": 0.0487, "low": 0.062154, "spr": 0.3, "init": 0.304638, "pairLow": 0.005671, "oopCall": 1.3225, "callerShrink": 0.0, "ipCall": 0.703737, "domPot": 0.183245}
# Sanity bands on open %: the reference open width +/- 2.5 points (ref/gto_open_lj.txt 16.3%,
# gto_open_btn.txt 42.1% of combos); PENALTY per point outside. Re-derive if ref/ changes.
BANDS = {"UTG": (13.8, 18.8), "BTN": (39.6, 44.6)}
PENALTY = 0.005
STEP = {"oop": 0.05, "flush": 0.02, "conn": 0.01, "set": 0.03, "dom": 0.02, "low": 0.01, "spr": 1.0, "init": 0.1, "pairLow": 0.01, "oopCall": 0.1, "callerShrink": 0.1, "ipCall": 0.2, "domPot": 0.05}


def validate(out, flag, text):
    cmd = [os.path.join(BUILD, "validate"), "--strategy", out, "--ref", REF] + ([flag, text] if text else [])
    val = subprocess.run(cmd, capture_output=True, text=True).stdout
    m = re.search(r"^score ([0-9.]+)", val, re.M)
    if not m:
        raise RuntimeError("validate failed:\n" + val)
    return float(m.group(1))


def evaluate(r, iters, args, tmp, holdout=False, seed=None):
    """(score, info) for realization config r after a fresh solve of `iters` iterations."""
    tag = "_".join(f"{k}{v:.4f}" for k, v in sorted(r.items())) + f"_{iters}_{seed}"
    out = os.path.join(tmp, tag + ".bin")
    cmd = [os.path.join(BUILD, "solve"), "--seats", "6", "--iters", str(iters), "--chunk", str(iters),
           "--threads", str(args.threads), "--out", out, "--report", "--boards", str(args.boards)]
    for k, v in r.items():
        cmd += ["--r", f"{k}={v}"]
    if seed is not None:
        cmd += ["--seed", str(seed)]
    report = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    score = validate(out, "--exclude", args.holdout)
    held = validate(out, "--only", args.holdout) if holdout else None
    btn = re.search(r"^  BTN\s+[0-9.]+% \|\s+([0-9.]+)%", report, re.M)  # BB fold vs BTN open, for context
    opens = {s: float(v) for s, v in re.findall(r"^  (UTG|HJ|CO|BTN|SB)\s+([0-9.]+)%", report, re.M)}
    pen = PENALTY * sum(max(lo - opens[s], 0, opens[s] - hi) for s, (lo, hi) in BANDS.items())
    os.remove(out)
    info = f"BB folds {btn.group(1) if btn else '?'}% vs BTN, opens " + "/".join(f"{opens.get(s, 0):.0f}" for s in BANDS)
    if pen:
        info += f", sanity penalty {pen:.4f}"
    if held is not None:
        info += f", held-out ({args.holdout}) {held:.4f}"
    return score + pen, info


def score_all(cands, iters, args, tmp, holdout=False):
    """(mean score, info of the first seed, per-seed scores) per candidate, --jobs runs at a time."""
    jobs = [(c, 101 * (i + 1)) for c in cands for i in range(args.seeds)]
    with cf.ThreadPoolExecutor(args.jobs) as pool:
        res = list(pool.map(lambda j: evaluate(j[0], iters, args, tmp, holdout, j[1]), jobs))
    out = []
    for i in range(len(cands)):
        chunk = res[i * args.seeds:(i + 1) * args.seeds]
        out.append((sum(x[0] for x in chunk) / len(chunk), chunk[0][1], [x[0] for x in chunk]))
    return out


def noise(per_seed, default):
    """Run-to-run standard deviation of a score, pooled over candidates (each a list of per-seed scores)."""
    groups = [s for s in per_seed if len(s) > 1]
    if not groups:
        return default
    return statistics.fmean(statistics.variance(s) for s in groups) ** 0.5


def beats(cand, cur, z, sd):
    """True if cand's mean score beats cur's by more than z standard errors (run-to-run sd `sd`)."""
    d = statistics.fmean(cand) - statistics.fmean(cur)
    return d < 0 and d < -z * sd * (1 / len(cand) + 1 / len(cur)) ** 0.5


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--schedule", default="5e7,1e8",
                    help="iterations per candidate; moves one entry on at each step halving")
    ap.add_argument("--rounds", type=int, default=10)
    ap.add_argument("--z", type=float, default=2.0, help="accept a move only if it wins by z standard errors")
    ap.add_argument("--seeds", type=int, default=2, help="seeds averaged per candidate")
    ap.add_argument("--threads", type=int, default=os.cpu_count(), help="threads per solve")
    ap.add_argument("--jobs", type=int, default=1, help="solves at a time")
    ap.add_argument("--boards", type=int, default=100, help="multiway flop equity boards (solve --boards)")
    ap.add_argument("--noise", type=float, default=0.002,
                    help="run-to-run score sd assumed with --seeds 1 (measured: ~0.002 at 50M, 100 boards)")
    ap.add_argument("--holdout", default="")
    ap.add_argument("--ref", default=os.path.join(ROOT, "ref"), help="reference charts (top-level *.txt)")
    args = ap.parse_args()
    schedule = [int(float(x)) for x in args.schedule.split(",")]
    global REF
    REF = args.ref
    refs = [f for f in os.listdir(REF) if f.endswith(".txt")]
    held = [f for f in refs if args.holdout and args.holdout in f]
    if not refs or (args.holdout and not 0 < len(held) < len(refs)):
        ap.error(f"--holdout {args.holdout!r} matches {len(held)} of the {len(refs)} charts in {REF}")

    cur, step = dict(START), dict(STEP)
    fallback = None  # (config, score, info) of the best single move behind an unconfirmed combined point
    cur_score = cur_info = cur_seeds = None
    halvings = 0
    t_start = time.time()
    with tempfile.TemporaryDirectory() as tmp:
        for rnd in range(1, args.rounds + 1):
            iters = schedule[min(halvings, len(schedule) - 1)]
            scored_cur = cur_score is None
            cands = [dict(cur)] if scored_cur else []
            for k in cur:
                for sign in (1, -1):
                    c = dict(cur)
                    c[k] = round(max(0.0, c[k] + sign * step[k]), 4)
                    if c != cur and c not in cands:
                        cands.append(c)
            t0 = time.time()
            scores = score_all(cands, iters, args, tmp)
            sd = noise([ps for _, _, ps in scores], args.noise)
            if scored_cur:
                cur_score, cur_info, cur_seeds = scores[0]
                cands, scores = cands[1:], scores[1:]
            print(f"round {rnd} ({iters / 1e6:g}M iterations x {args.seeds} seeds, {len(cands) + scored_cur} candidates, "
                  f"{time.time() - t0:.0f}s, run sd {sd:.4f}): current {cur_score:.4f} ({cur_info})  {cur}")

            se = sd * (1 / args.seeds + 1 / len(cur_seeds)) ** 0.5
            for (s, _, _), c in sorted(zip(scores, cands), key=lambda x: x[0][0]):
                k = next(k for k in cur if c[k] != cur[k])
                print(f"    {k}={c[k]:<7g} {s:.4f}  {s - cur_score:+.4f} ({(s - cur_score) / se:+.1f} se)")
            if fallback and beats(fallback[3], cur_seeds, 0, sd):  # the combined move was worse than the best single
                print(f"  combined point lost to the best single move ({fallback[1]:.4f}); reverting\n", flush=True)
                cur, cur_score, cur_info, cur_seeds = fallback
                fallback = None
                continue
            fallback = None

            better = [(s, c) for (s, _, ps), c in zip(scores, cands) if beats(ps, cur_seeds, args.z, sd)]
            if not better:
                step = {k: v / 2 for k, v in step.items()}
                halvings += 1
                if schedule[min(halvings, len(schedule) - 1)] != iters:
                    cur_score = None  # re-score the current point at the new iteration count
                print(f"  no improvement, halving steps -> {step}\n", flush=True)
                continue
            # Best move per parameter, then all improving parameters together.
            best_per = {}
            for s, c in better:
                k = next(k for k in cur if c[k] != cur[k])
                if k not in best_per or s < best_per[k][0]:
                    best_per[k] = (s, c)
            s1, c1 = min(best_per.values(), key=lambda x: x[0])
            _, info1, seeds1 = scores[cands.index(c1)]
            if len(best_per) == 1:
                k = next(iter(best_per))
                print(f"  best {s1:.4f} ({info1}) by {{{k}: {c1[k]}}}")
                cur, cur_score, cur_info, cur_seeds = c1, s1, info1, seeds1
            else:
                combined = dict(cur)
                for k, (_, c) in best_per.items():
                    combined[k] = c[k]
                moves = ", ".join(f"{k}: {combined[k]} ({s:.4f})" for k, (s, _) in best_per.items())
                print(f"  {len(best_per)} improving moves combined: {moves}")
                fallback = (c1, s1, info1, seeds1)
                cur, cur_score = combined, None
            print(flush=True)

        # Final scores at the last schedule entry (and held-out scores): the winner against the start R.
        finals = [cur] + ([fallback[0]] if fallback else []) + ([START] if cur != START else [])
        res = score_all(finals, schedule[-1], args, tmp, holdout=bool(args.holdout))
        cur_score, cur_info, _ = res[0]
        if fallback and res[1][0] < cur_score:
            cur, (cur_score, cur_info, _) = fallback[0], res[1]
        if finals[-1] is START:
            print(f"start R at {schedule[-1] / 1e6:g}M: {res[-1][0]:.4f} ({res[-1][1]})")
        print(f"final {cur_score:.4f} ({cur_info}) after {(time.time() - t_start) / 60:.1f} min")
    print("final R:", " ".join(f"--r {k}={v}" for k, v in cur.items()))


if __name__ == "__main__":
    main()
