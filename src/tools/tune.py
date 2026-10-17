#!/usr/bin/env python3
"""Fits the realization factors (utility.h RealizationConfig) to the reference charts in ref/
(docs/validation.md "Tuning knobs") by coordinate search.

Each candidate R warm-starts a 6-max solve from a checkpoint (regrets kept, averaging restarted),
runs some iterations, and is scored with `validate` (category-balanced mean distance over ref/,
lower is better) plus a penalty for each sanity-check band (validation.md) the opens fall outside.

Each round scores +/- one step on every parameter (plus the current point when its score is not
known at this iteration count), all candidates at once. Every parameter whose step improved on the
current point moves together; the combined point is checked as the next round's current point and
replaced by the best single move if it turns out worse. With no improvement the steps halve.
Iterations follow --schedule: coarse rounds use few iterations, each halving moves one entry on.

Usage: src/tools/tune.py [--checkpoint data/6max.ck] [--schedule 5e6,10e6,20e6] [--rounds 8]
                         [--jobs N] [--holdout text]
--holdout leaves reference files whose name contains `text` out of the fit and scores them separately
at the end, to check the fit generalizes.
"""
import argparse
import concurrent.futures as cf
import os
import re
import subprocess
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.join(ROOT, "build")
# Start point = current defaults in utility.h; step = initial search step per parameter.
START = {"oop": 0.7, "flush": 0.15, "conn": 0.05, "set": 0.1, "dom": 0.1, "low": 0.03, "spr": 3.0, "init": 0.2, "pairLow": 0.02}
# Sanity bands on open % (validation.md: UTG mid-teens, BTN in the 40s); PENALTY per point outside.
BANDS = {"UTG": (13.0, 18.0), "BTN": (40.0, 50.0)}
PENALTY = 0.005
STEP = {"oop": 0.05, "flush": 0.02, "conn": 0.01, "set": 0.03, "dom": 0.02, "low": 0.01, "spr": 1.0, "init": 0.05, "pairLow": 0.01}
BOARDS = 25  # multiway equity boards per estimate; K=25 is within noise of K=100 (validation.md)


def validate(out, flag, text):
    cmd = [os.path.join(BUILD, "validate"), "--strategy", out] + ([flag, text] if text else [])
    val = subprocess.run(cmd, capture_output=True, text=True).stdout
    m = re.search(r"^score ([0-9.]+)", val, re.M)
    if not m:
        raise RuntimeError("validate failed:\n" + val)
    return float(m.group(1))


def evaluate(r, iters, args, tmp, holdout=False):
    """(score, info) for realization config r after `iters` warm-started iterations."""
    tag = "_".join(f"{k}{v:.4f}" for k, v in sorted(r.items())) + f"_{iters}"
    out = os.path.join(tmp, tag + ".bin")
    cmd = [os.path.join(BUILD, "solve"), "--seats", "6", "--iters", str(iters), "--chunk", str(iters),
           "--warm", args.checkpoint, "--out", out, "--report", "--reset-average", "--boards", str(BOARDS)]
    for k, v in r.items():
        cmd += ["--r", f"{k}={v}"]
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--checkpoint", default=os.path.join(ROOT, "data", "6max.ck"))
    ap.add_argument("--schedule", default="5e6,10e6,20e6",
                    help="iterations per candidate; moves one entry on at each step halving")
    ap.add_argument("--rounds", type=int, default=8)
    ap.add_argument("--jobs", type=int, default=0, help="parallel solves (default: all candidates at once)")
    ap.add_argument("--holdout", default="")
    args = ap.parse_args()
    schedule = [int(float(x)) for x in args.schedule.split(",")]

    cur, step = dict(START), dict(STEP)
    fallback = None  # (config, score, info) of the best single move behind an unconfirmed combined point
    cur_score = cur_info = None
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
            with cf.ThreadPoolExecutor(args.jobs or len(cands)) as pool:
                scores = list(pool.map(lambda c: evaluate(c, iters, args, tmp), cands))
            if scored_cur:
                cur_score, cur_info = scores[0]
                cands, scores = cands[1:], scores[1:]
            print(f"round {rnd} ({iters / 1e6:g}M iterations, {len(cands) + scored_cur} solves, "
                  f"{time.time() - t0:.0f}s): current {cur_score:.4f} ({cur_info})  {cur}")

            if fallback and fallback[1] < cur_score:  # the combined move was worse than the best single
                print(f"  combined point lost to the best single move ({fallback[1]:.4f}); reverting\n", flush=True)
                cur, cur_score, cur_info = fallback
                fallback = None
                continue
            fallback = None

            better = [(s, c) for (s, _), c in zip(scores, cands) if s < cur_score]
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
            info1 = scores[cands.index(c1)][1]
            if len(best_per) == 1:
                k = next(iter(best_per))
                print(f"  best {s1:.4f} ({info1}) by {{{k}: {c1[k]}}}")
                cur, cur_score, cur_info = c1, s1, info1
            else:
                combined = dict(cur)
                for k, (_, c) in best_per.items():
                    combined[k] = c[k]
                moves = ", ".join(f"{k}: {combined[k]} ({s:.4f})" for k, (s, _) in best_per.items())
                print(f"  {len(best_per)} improving moves combined: {moves}")
                fallback = (c1, s1, info1)
                cur, cur_score = combined, None
            print(flush=True)

        # Final score at the finest iteration count (and held-out score), start R for comparison.
        finals = [cur] + ([fallback[0]] if fallback else []) + ([START] if args.holdout else [])
        with cf.ThreadPoolExecutor(len(finals)) as pool:
            res = list(pool.map(lambda c: evaluate(c, schedule[-1], args, tmp, holdout=bool(args.holdout)), finals))
        cur_score, cur_info = res[0]
        if fallback and res[1][0] < cur_score:
            cur, (cur_score, cur_info) = fallback[0], res[1]
        if args.holdout:
            print(f"start R at {schedule[-1] / 1e6:g}M: {res[-1][1]}")
        print(f"final {cur_score:.4f} ({cur_info}) after {(time.time() - t_start) / 60:.1f} min")
    print("final R:", " ".join(f"--r {k}={v}" for k, v in cur.items()))


if __name__ == "__main__":
    main()
