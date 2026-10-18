#!/usr/bin/env python3
"""Fits the realization factors (utility.h RealizationConfig) to the reference charts by Bayesian optimisation
(docs/validation.md "Tune").

Each evaluation is one fresh solve with a new seed, scored exactly as in tune.py (`validate` plus the open-width
sanity penalty). A Gaussian process with a noise term models the score over all 13 parameters, and expected
improvement picks the next point, so run-to-run noise is averaged by the model instead of by repeat seeds. The search
box is the tune.py start point +/- 3 steps (floored at 0), with spr on a log scale. Every evaluation is appended to
--log as a JSON line and reloaded on restart (entries at other --iters are ignored). The answer is the evaluated point
with the lowest GP mean, not the best single score, which is partly luck.

Usage: .venv/bin/python src/tools/bo_tune.py [--evals 200] [--iters 1e8] [--init 20] [--log data/bo/log.jsonl]
                                             [--threads N] [--boards 100]
       .venv/bin/python src/tools/bo_tune.py --selftest
"""
import argparse
import json
import os
import sys
import tempfile
import time

import numpy as np
from skopt import Optimizer
from skopt.space import Real

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tune import ROOT, START, STEP, evaluate  # noqa: E402

KEYS = list(START)
SPACE = [Real(0.3, 8.0, prior="log-uniform", name=k) if k == "spr" else
         Real(round(max(0.0, START[k] - 3 * STEP[k]), 6), round(START[k] + 3 * STEP[k], 6), name=k)
         for k in KEYS]  # bounds on the same 6-decimal grid as the logged points, so a rounded point stays inside


def optimizer(init, seed=0):
    # xi is in score units: skopt's default 0.01 dwarfs score differences here (~0.002) and keeps it exploring.
    return Optimizer(SPACE, base_estimator="GP", acq_func="EI", acq_func_kwargs={"xi": 0.0005}, n_initial_points=init,
                     initial_point_generator="lhs" if init else "random", random_state=seed)  # lhs fails on 0 points


def ranked(opt):
    """Evaluated points sorted by GP mean: [(mean, sd, observed score, x)]."""
    if not opt.models:
        return sorted((y, 0.0, y, x) for x, y in zip(opt.Xi, opt.yi))
    mu, sd = opt.models[-1].predict(opt.space.transform(opt.Xi), return_std=True)
    return sorted(zip(mu, sd, opt.yi, opt.Xi), key=lambda t: t[0])


def selftest():
    """A noisy quadratic over the same box with a known optimum: the GP-best point must beat START clearly."""
    rng = np.random.default_rng(1)
    lo, hi = np.array([d.low for d in SPACE]), np.array([d.high for d in SPACE])
    opt_x = lo + (hi - lo) * rng.uniform(0.25, 0.75, len(SPACE))
    f = lambda x: 0.13 + 0.02 * float(np.sum(((np.array(x) - opt_x) / (hi - lo)) ** 2))
    opt = optimizer(15)
    opt.tell([START[k] for k in KEYS], f([START[k] for k in KEYS]) + rng.normal(0, 0.0015))
    for _ in range(85):
        x = opt.ask()
        opt.tell(x, f(x) + rng.normal(0, 0.0015))
    best = ranked(opt)[0][3]
    gap0, gap = f([START[k] for k in KEYS]) - 0.13, f(best) - 0.13
    print(f"selftest: excess over optimum {gap0:.4f} at START -> {gap:.4f} at the GP-best point")
    assert gap < 0.25 * gap0, "BO did not cut the excess to a quarter"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--evals", type=int, default=200, help="total evaluations, including those already logged")
    ap.add_argument("--iters", type=float, default=1e8, help="iterations per solve")
    ap.add_argument("--init", type=int, default=20, help="space-filling points before the GP takes over")
    ap.add_argument("--log", default=os.path.join(ROOT, "data", "bo", "log.jsonl"))
    ap.add_argument("--threads", type=int, default=os.cpu_count())
    ap.add_argument("--boards", type=int, default=100)
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    iters = int(args.iters)
    args.holdout = ""  # evaluate() reads it

    done = []
    if os.path.exists(args.log):
        with open(args.log) as fh:
            done = [e for e in map(json.loads, fh) if e["iters"] == iters]
    opt = optimizer(max(0, args.init - len(done)))
    if done:
        opt.tell([[e["x"][k] for k in KEYS] for e in done], [e["y"] for e in done])
    print(f"{len(done)} logged evaluations at {iters / 1e6:g}M reloaded from {args.log}", flush=True)
    os.makedirs(os.path.dirname(args.log), exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp, open(args.log, "a") as log:
        for n in range(len(done), args.evals):
            x = [START[k] for k in KEYS] if n == 0 else opt.ask()
            r = {k: round(float(v), 6) for k, v in zip(KEYS, x)}
            t0 = time.time()
            y, info = evaluate(r, iters, args, tmp, seed=1000 + n)
            opt.tell([r[k] for k in KEYS], y)
            log.write(json.dumps({"n": n, "iters": iters, "seed": 1000 + n, "x": r, "y": y, "info": info}) + "\n")
            log.flush()
            best = ranked(opt)[0]
            print(f"{n + 1}/{args.evals} {y:.4f} ({info}, {time.time() - t0:.0f}s)  GP best {best[0]:.4f} +/- {best[1]:.4f}",
                  flush=True)

    print("\ntop points by GP mean (mean +/- sd, observed):")
    for mu, sd, y, x in ranked(opt)[:5]:
        print(f"  {mu:.4f} +/- {sd:.4f} ({y:.4f})  " + " ".join(f"--r {k}={v:g}" for k, v in zip(KEYS, x)))


if __name__ == "__main__":
    main()
