#!/usr/bin/env python3
"""Fits the realization factors (utility.h RealizationConfig) to the ref/ charts from a sample
directory written by build/sample_evs (docs/fit-spec.md), without a solve per candidate.

Every strategy is held fixed, so only flop-terminal payoffs move with theta:
    EV_theta(s, k, a) = (sk_const + sum_r w_r * pay_theta(r)) / sk_W
    pay_theta(r) = net * R_h eq_h / sum_{j live} R_j eq_j - committed_h
The loss is the chart mix's KL divergence from softmax(EV_theta / tau) (cross-entropy minus the
chart's own entropy: same gradient, 0 = the chart is exactly the softmax), weighted by sk_weight,
averaged per spot, then per category, then over categories (like validate), plus
l2 * sum ((theta - start) / step)^2. Regret = best EV minus the chart mix's EV, in bb, averaged the
same way. L-BFGS-B with an analytic gradient: R depends on a record only through (terminal role,
class), so R and dR/dtheta are a small table and the records only see it through gathers.

Usage: src/tools/fit_realization.py DIR [--tau 1] [--l2 1e-3] [--fit oop,flush,...] [--holdout text]
                                        [--maxiter 200]
       src/tools/fit_realization.py --selftest   plant a theta in a synthetic sample, recover it
"""
import argparse
import os
import re
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

import numpy as np
from scipy.optimize import minimize

# RealizationConfig field order (utility.h), defaults, tune.py search steps (the L2 and optimiser
# scale), and bounds. ip is fixed by default: R only matters up to a common factor.
FIELDS = ["ip", "oop", "flush", "conn", "set", "dom", "low", "spr", "init", "pairLow", "oopCall",
          "callerShrink", "ipCall", "domPot"]
DEFAULT = dict(ip=1, oop=0.650545, flush=0.22685, conn=0.08041, set=0.2131, dom=0.0487, low=0.062154,
               spr=0.3, init=0.304638, pairLow=0.005671, oopCall=1.3225, callerShrink=0, ipCall=0.703737, domPot=0.183245)
STEP = dict(ip=0.05, oop=0.05, flush=0.02, conn=0.01, set=0.03, dom=0.02, low=0.01, spr=1.0, init=0.1,
            pairLow=0.01, oopCall=0.1, callerShrink=0.1, ipCall=0.2, domPot=0.05)
BOUNDS = dict(ip=(0.5, 1.5), oop=(0.2, 1.2), flush=(-0.2, 0.6), conn=(-0.1, 0.3), set=(-0.2, 0.6),
              dom=(-0.1, 0.4), low=(-0.05, 0.1), spr=(0.05, 50), init=(-0.5, 1.5), pairLow=(-0.05, 0.08),
              oopCall=(0, 3), callerShrink=(0, 1), ipCall=(0.2, 1.5), domPot=(-0.2, 0.4))
FIT = [f for f in FIELDS if f != "ip"]
NCLS = 169


# ---- port of utility.cpp: straights(), domination(), realization() over classes ----

def class_features():
    """Per class (bucket() grid, row/col 0 = A): hi rank, pair, suited, straights, domination."""
    k = np.arange(NCLS)
    row, col = k // 13, k % 13
    hi, lo = 12 - np.minimum(row, col), 12 - np.maximum(row, col)  # ranks, 12 = A
    pair, suited = row == col, row < col
    st = np.zeros(NCLS, int)
    for s in range(-1, 9):  # ranks s..s+4; s = -1 is the wheel, where the ace plays low
        inn = lambda r: ((r >= s) & (r <= s + 4)) | ((s == -1) & (r == 12))
        st += inn(hi) & inn(lo)
    st[pair] = 0
    dom = np.where(pair | suited | (hi < 8), 0, np.minimum(hi - 1 - lo, 4))
    return hi, pair, suited, st, dom


def realization(t, last, raiser, spr, reraised, hi, pair, suited, st, dom, grad=False):
    """realization() for theta t (FIELDS order), broadcasting over the role and class arrays.
    grad: also {field: dR/dfield} (0 where R is floored), each broadcastable to R's shape."""
    ip, oop, flush, conn, set_, dm, low, sprc, init, pairLow, oopCall, shrink, ipCall, domPot = t
    last, raiser, rr, pair, suited = (np.asarray(x, bool) for x in (last, raiser, reraised, pair, suited))
    spr = np.asarray(spr, float)
    g = spr / (spr + sprc) if sprc > 0 else np.ones_like(spr)
    oopc = ~last & ~raiser
    fade = np.where(oopc, shrink * g + 1 - shrink, g)
    ipc = np.where(~raiser & rr, ipCall, 1)
    pos = np.where(last, ip * ipc, 1 - (1 - oop) * fade)
    play = flush * suited + conn * st * g + set_ * pair * g
    m = np.where(oopc, oopCall, 1)
    offsuit = ~pair & ~suited
    h = 1 + play * m - (dm + domPot * rr) * dom * ~raiser - low * (12 - hi) * offsuit - pairLow * (12 - hi) * pair
    ini = np.where(raiser, 1 + init, 1)
    R0 = pos * h * ini
    if not grad:
        return np.maximum(R0, 0.05)
    on = R0 > 0.05
    dh, dpos = pos * ini * on, h * ini * on  # dR/dh, dR/dpos
    dg = -spr / (spr + sprc) ** 2 if sprc > 0 else 0 * spr  # dg/dspr(field)
    d = dict(ip=dpos * last * ipc, oop=dpos * ~last * fade, ipCall=dpos * (last & ~raiser & rr) * ip,
             callerShrink=-dpos * (1 - oop) * oopc * (g - 1),
             spr=dg * (-dpos * (1 - oop) * ~last * np.where(oopc, shrink, 1) + dh * m * (conn * st + set_ * pair)),
             flush=dh * m * suited, conn=dh * m * st * g, set=dh * m * pair * g, oopCall=dh * oopc * play,
             dom=-dh * dom * ~raiser, domPot=-dh * (rr & ~raiser) * dom, low=-dh * (12 - hi) * offsuit,
             pairLow=-dh * (12 - hi) * pair, init=pos * h * raiser * on)
    return np.maximum(R0, 0.05), d


# ---- sample directory ----

REC = ["rec_sk", "rec_a", "rec_w", "rec_term", "rec_cls", "rec_eq", "rec_pay"]
TERM = ["term_edge", "term_net", "term_spr", "term_live", "term_last", "term_raiser", "term_reraised",
        "term_committed"]
SK = ["sk_W", "sk_W2", "sk_const", "sk_constsq", "sk_chart", "sk_weight", "sk_legal"]


def read_meta_theta(path):
    """The sampling RealizationConfig: meta.txt's line `realization ip=1 oop=0.7 ...`."""
    text = "".join(l for l in open(path) if l.startswith("realization "))
    found = dict(re.findall(r"(\w+)=([-+0-9.eE]+)", text))
    missing = [f for f in FIELDS if f not in found]
    if missing:
        sys.exit(f"{path}: no value for {' '.join(missing)}")
    return np.array([float(found[f]) for f in FIELDS])


def load(d):
    data = {n: np.load(os.path.join(d, n + ".npy"), mmap_mode="r") for n in REC + TERM + SK}
    spots = []
    for line in open(os.path.join(d, "spots.tsv")):
        f = line.rstrip("\n").split("\t")
        if f[0].strip().isdigit():  # skips a header
            assert int(f[0]) == len(spots), "spots.tsv: indices not 0..S-1 in order"
            spots.append(dict(name=f[1], category=f[2], hero=int(f[4])))
    data["spots"] = spots
    data["theta"] = read_meta_theta(os.path.join(d, "meta.txt"))
    return data


def save(d, data):
    os.makedirs(d, exist_ok=True)
    for n in REC + TERM + SK:
        np.save(os.path.join(d, n + ".npy"), data[n])
    with open(os.path.join(d, "spots.tsv"), "w") as f:
        f.write("index\tname\tcategory\tnode\thero_seat\thistory\n")
        for i, s in enumerate(data["spots"]):
            f.write(f"{i}\t{s['name']}\t{s['category']}\t0\t{s['hero']}\t\n")
    with open(os.path.join(d, "meta.txt"), "w") as f:
        f.write("realization " + " ".join(f"{k}={v:g}" for k, v in zip(FIELDS, data["theta"])) + "\n")


# ---- precompute: everything that does not depend on theta ----

def prepare(data):
    S = len(data["spots"])
    W = np.asarray(data["sk_W"], float)
    weight = np.asarray(data["sk_weight"], float)
    valid = (W > 0) & (weight > 0)
    hero = np.array([s["hero"] for s in data["spots"]])

    # Terminal roles: R depends on (term, seat) only through (spr, last, raiser, reraised).
    T = len(data["term_net"])
    seat = np.tile(np.arange(6), T)
    tt = np.repeat(np.arange(T), 6)
    role = np.column_stack([np.asarray(data["term_spr"], float)[tt], seat == np.asarray(data["term_last"])[tt],
                            seat == np.asarray(data["term_raiser"])[tt], np.asarray(data["term_reraised"])[tt] > 0])
    roles, role_of = np.unique(role, axis=0, return_inverse=True)  # role_of[term * 6 + seat]
    role_of = role_of.ravel()
    feat = class_features()
    table_args = (roles[:, 1:2] > 0, roles[:, 2:3] > 0, roles[:, 0:1], roles[:, 3:4] > 0, *(f[None, :] for f in feat))

    # Records sorted by (s, k, a), so each EV sum is a contiguous segment, then cut on segment
    # boundaries into 4 chunks per thread (numpy's gathers, reduceat and add.at run without the GIL).
    # Per record and per live seat everything is stored as float32 / int32 / uint8 (~33 B a record
    # plus 8 B a live seat); arithmetic runs in float64, a chunk at a time.
    ska = np.asarray(data["rec_sk"]).astype(np.int32) * 3 + np.asarray(data["rec_a"])
    order = np.nonzero(valid[ska // 3])[0]
    order = order[np.argsort(ska[order], kind="stable")]
    ska = ska[order]
    n = len(ska)
    seg = np.flatnonzero(np.diff(ska, prepend=-1))
    threads = os.cpu_count()
    cuts = np.unique(np.append(seg[np.searchsorted(seg, np.arange(4 * threads) * n // (4 * threads))], n)) if n else [0]
    term_net, term_com = np.asarray(data["term_net"], np.float32), np.asarray(data["term_committed"], np.float32)
    R0 = realization(data["theta"], *table_args).ravel()
    chunks, nent, port_err = [], 0, 0.0
    for r0, r1 in zip(cuts[:-1], cuts[1:]):
        o = order[r0:r1]
        sk = ska[r0:r1] // 3
        term = np.asarray(data["rec_term"])[o].astype(np.intp)
        cls = np.asarray(data["rec_cls"])[o]
        h = hero[sk // NCLS]
        ar = np.arange(len(o))
        mask = cls != 255
        counts = mask.sum(1, dtype=np.uint8)
        assert counts.min() >= 2, "a record with fewer than two live seats"
        assert mask[ar, h].all(), "a record where the hero has folded"
        offs = np.zeros(len(o), np.int32)
        np.cumsum(counts[:-1], out=offs[1:])
        key = (role_of[term[:, None] * 6 + np.arange(6)] * NCLS + cls).astype(np.int32)[mask]
        s = seg[(seg >= r0) & (seg < r1)]
        chunks.append(dict(key=key, eq=np.asarray(data["rec_eq"])[o][mask], offs=offs, counts=counts,
                           hpos=offs + mask.cumsum(1, dtype=np.int8)[ar, h] - 1, net=term_net[term],
                           com=term_com[term, h], wn=(np.asarray(data["rec_w"])[o] / W[sk]).astype(np.float32),
                           seg=s - r0, segid=ska[s], segn=np.diff(np.append(s, r1))))
        nent += len(key)
        c = chunks[-1]
        port_err = max(port_err, (np.abs(_forward(c, R0)[0] - np.asarray(data["rec_pay"])[o]) / c["net"]).max())
    return dict(S=S, W=W, valid=valid, legal=np.asarray(data["sk_legal"]), chart=np.asarray(data["sk_chart"], float),
                weight=weight, const=np.asarray(data["sk_const"], float), table_args=table_args,
                nkey=len(roles) * NCLS, nent=nent, nrec=n, chunks=chunks, pool=ThreadPoolExecutor(threads),
                spots=data["spots"], theta0=data["theta"], port_err=port_err)


def _forward(c, R):
    x = R[c["key"]] * c["eq"]
    S = np.add.reduceat(x, c["offs"])
    xh = x[c["hpos"]]
    pay = c["net"] * xh / S - c["com"]
    return pay, S, xh, np.add.reduceat(c["wn"] * pay, c["seg"])


def _backward(c, dEV, S, xh, nkey):
    coef = np.repeat(dEV[c["segid"]], c["segn"]) * c["wn"] * c["net"] / S  # dL/dpay * net / S
    dx = np.repeat(-coef * xh / S, c["counts"])
    dx[c["hpos"]] += coef
    out = np.zeros(nkey)
    np.add.at(out, c["key"], dx * c["eq"])
    return out


def forward(P, R):
    """EV per (s, k) and action given the R table, plus what backward() needs."""
    def run(c):  # keeps S and the hero's R * eq for backward(), in float32
        _, S, xh, segsum = _forward(c, R)
        return S.astype(np.float32), xh.astype(np.float32), segsum
    res = list(P["pool"].map(run, P["chunks"]))
    num = np.zeros(len(P["W"]) * 3)
    for c, r in zip(P["chunks"], res):
        num[c["segid"]] = r[2]
    W = P["W"][:, None]
    return np.where(W > 0, P["const"] / np.where(W > 0, W, 1), 0) + num.reshape(-1, 3), res


def backward(P, dEV, res):
    """dLoss/dR per table entry, given dLoss/dEV."""
    d = dEV.ravel()
    return sum(P["pool"].map(lambda cr: _backward(cr[0], d, cr[1][0], cr[1][1], P["nkey"]), zip(P["chunks"], res)))


def pays(P, t):
    """Per-record hero payoff at theta t, in (s, k, a) order, records of invalid (s, k) left out."""
    R = realization(t, *P["table_args"]).ravel()
    return np.concatenate(list(P["pool"].map(lambda c: _forward(c, R)[0], P["chunks"])))


# ---- loss and report ----

def spot_weights(P, spots):
    """Per (s, k): sk_weight normalised within its spot, then 1 / spots in category, then 1 / categories."""
    w = np.where(P["valid"], P["weight"], 0).reshape(P["S"], NCLS)
    tot = w.sum(1)
    spots = [s for s in spots if tot[s] > 0]
    cats = sorted({P["spots"][s]["category"] for s in spots})
    out = np.zeros_like(w)
    for c in cats:
        cs = [s for s in spots if P["spots"][s]["category"] == c]
        for s in cs:
            out[s] = w[s] / tot[s] / len(cs) / len(cats)
    return out.ravel()


def per_sk(P, EV, tau):
    """KL(chart || softmax(EV / tau)), its gradient over EV, and the chart's regret, per (s, k)."""
    legal = (P["legal"][:, None] >> np.arange(3) & 1).astype(bool).repeat(NCLS, 0)
    p = np.where(legal, P["chart"], 0)
    p /= np.maximum(p.sum(1, keepdims=True), 1e-300)
    z = np.where(legal, EV / tau, -np.inf)
    z -= z.max(1, keepdims=True)
    logq = z - np.log(np.exp(z).sum(1, keepdims=True))
    q = np.exp(logq)
    kl = np.where(p > 0, p * (np.log(np.where(p > 0, p, 1)) - np.where(legal, logq, 0)), 0).sum(1)
    regret = np.where(legal, EV, -np.inf).max(1) - (p * np.where(legal, EV, 0)).sum(1)
    return np.where(P["valid"], kl, 0), (q - p) / tau, np.where(P["valid"], regret, 0)


def report(P, EV, tau, spots):
    kl, _, rg = per_sk(P, EV, tau)
    cw = spot_weights(P, spots)
    rows = {}
    for s in spots:
        sl = slice(s * NCLS, (s + 1) * NCLS)
        if cw[sl].sum() > 0:
            c = P["spots"][s]["category"]
            w = cw[sl] / cw[sl].sum()
            rows.setdefault(c, []).append(((w * kl[sl]).sum(), (w * rg[sl]).sum()))
    out = {c: tuple(np.mean(v, 0)) + (len(v),) for c, v in rows.items()}
    out["(balanced)"] = ((cw * kl).sum(), (cw * rg).sum(), sum(len(v) for v in rows.values()))
    return out


def print_reports(before, after, label):
    print(f"{label}: category        spots   KL before  KL after  regret before (bb)  after")
    for c in sorted(before, key=lambda c: (c == "(balanced)", c)):
        b, a = before[c], after[c]
        print(f"  {c:<22} {b[2]:4d}   {b[0]:9.4f} {a[0]:9.4f}   {b[1]:9.4f}          {a[1]:9.4f}")


def make_objective(P, theta0, fit, tau, l2, cw):
    idx = [FIELDS.index(f) for f in fit]
    step = np.array([STEP[f] for f in fit])

    def theta(x):
        t = theta0.copy()
        t[idx] = theta0[idx] + x * step
        return t

    def f(x):
        R, dR = realization(theta(x), *P["table_args"], grad=True)
        EV, res = forward(P, R.ravel())
        kl, dEV, _ = per_sk(P, EV, tau)
        dRt = backward(P, dEV * cw[:, None], res).reshape(R.shape)
        g = np.array([(dR[k] * dRt).sum() for k in fit]) * step + 2 * l2 * x
        return (cw * kl).sum() + l2 * (x * x).sum(), g

    bounds = [((BOUNDS[k][0] - theta0[i]) / STEP[k], (BOUNDS[k][1] - theta0[i]) / STEP[k]) for k, i in zip(fit, idx)]
    return f, theta, bounds


def ev_at(P, t):
    return forward(P, realization(t, *P["table_args"]).ravel())[0]


def fit(P, fit_fields, tau, l2, holdout, maxiter, quiet=False):
    names = [s["name"] for s in P["spots"]]
    fit_spots = [s for s, n in enumerate(names) if not (holdout and holdout in n)]
    held = [s for s, n in enumerate(names) if holdout and holdout in n]
    theta0 = P["theta0"].copy()
    f, theta, bounds = make_objective(P, theta0, fit_fields, tau, l2, spot_weights(P, fit_spots))
    t0 = time.time()
    res = minimize(f, np.zeros(len(fit_fields)), jac=True, method="L-BFGS-B", bounds=bounds,
                   options=dict(maxiter=maxiter, ftol=1e-12, gtol=1e-9))
    th = theta(res.x)
    if not quiet:
        dt = time.time() - t0
        print(f"L-BFGS-B: {res.message} after {res.nit} iterations, {res.nfev} evaluations, {dt:.1f}s "
              f"({dt / res.nfev:.2f}s per loss + gradient)")
        EV0, EV1 = ev_at(P, theta0), ev_at(P, th)
        print_reports(report(P, EV0, tau, fit_spots), report(P, EV1, tau, fit_spots), "fit")
        if held:
            print_reports(report(P, EV0, tau, held), report(P, EV1, tau, held), f"held out ({holdout})")
        print("start  " + " ".join(f"{k}={v:.4g}" for k, v in zip(FIELDS, theta0)))
        print("fitted " + " ".join(f"{k}={v:.4g}" for k, v in zip(FIELDS, th)))
        print("solve flags: " + " ".join(f"--r {k}={v:.4g}" for k, v in zip(FIELDS, th)))
        print("RealizationConfig{" + ", ".join(f"{v:.4g}" for v in th) + "}")
    return th


def check_port(P):
    """pay at the sampling theta must reproduce rec_pay (written by the C++ sampler); prepare() measures it."""
    print(f"port check: {P['nrec']} records, max |pay - rec_pay| / net {P['port_err']:.2e}")
    if P["port_err"] > 1e-5:
        sys.exit("port check failed: the numpy realization() does not reproduce rec_pay")


# ---- synthetic sample (self-test) ----

def synthetic(rng, recs_per_sk_action, true_theta, sample_theta, tau, S=8, T=300):
    """A sample directory's arrays in the spec format, with chart = softmax(EV_true / tau)."""
    cats = ["open", "vs_open", "vs_3bet", "squeeze"]
    spots = [dict(name=f"spot{s}_{cats[s % 4]}", category=cats[s % 4], hero=int(rng.integers(6))) for s in range(S)]
    legal = np.array([7 if s % 3 else 5 for s in range(S)], np.uint8)  # some spots without a call
    live = np.zeros(T, np.uint8)
    while (bad := np.array([bin(v).count("1") < 2 for v in live])).any():
        live[bad] = rng.integers(1, 64, bad.sum())
    order = [4, 5, 0, 1, 2, 3]  # postflop: SB, BB, UTG, HJ, CO, BTN
    last = np.array([[p for p in order if v >> p & 1][-1] for v in live], np.int8)
    raiser = np.array([rng.choice([p for p in range(6) if v >> p & 1] + [-1]) for v in live], np.int8)
    com = np.where((live[:, None] >> np.arange(6) & 1) > 0, rng.uniform(1, 20, (T, 6)), rng.uniform(0, 1, (T, 6)))
    d = dict(term_edge=np.arange(T, dtype=np.uint32), term_net=(com.sum(1) * 0.97).astype(np.float32),
             term_spr=rng.uniform(0.3, 30, T).astype(np.float32), term_live=live, term_last=last,
             term_raiser=raiser, term_reraised=rng.integers(0, 2, T).astype(np.uint8),
             term_committed=com.astype(np.float32))
    n = recs_per_sk_action
    sk_list, a_list = [], []
    for s in range(S):  # sorted by (s, k, a), so pays() comes back in file order
        acts = [a for a in (1, 2) if legal[s] >> a & 1]
        sk_list.append(np.repeat(np.arange(s * NCLS, (s + 1) * NCLS), len(acts) * n))
        a_list.append(np.tile(np.repeat(acts, n), NCLS))
    sk = np.concatenate(sk_list).astype(np.uint32)
    a = np.concatenate(a_list).astype(np.uint8)
    N = len(sk)
    hero = np.array([sp["hero"] for sp in spots])[sk // NCLS]
    term = np.empty(N, np.uint32)
    for hs in range(6):  # a terminal the hero is live at
        pool = np.nonzero(live >> hs & 1)[0]
        m = hero == hs
        term[m] = pool[rng.integers(len(pool), size=m.sum())]
    lm = (live[term][:, None] >> np.arange(6) & 1) > 0
    cls = np.where(lm, rng.integers(NCLS, size=(N, 6), dtype=np.uint8), np.uint8(255))
    cls[np.arange(N), hero] = sk % NCLS
    eq = np.where(lm, rng.standard_gamma(1.0, size=(N, 6), dtype=np.float32), np.float32(0))
    eq /= eq.sum(1, keepdims=True)
    deal = np.arange(N) % n  # the same n deals (weights) play out every action
    wdeal = rng.uniform(0.2, 1.8, (S * NCLS, n))
    d.update(rec_sk=sk, rec_a=a, rec_w=wdeal[sk, deal].astype(np.float32), rec_term=term, rec_cls=cls, rec_eq=eq,
             rec_pay=np.zeros(N, np.float32))
    W = wdeal.sum(1)
    weight = (np.array([6.0 if r == c else 4.0 if r < c else 12.0 for r in range(13) for c in range(13)])[None, :]
              * rng.uniform(0, 1, (S, NCLS))).ravel()
    d.update(sk_W=W, sk_W2=(wdeal ** 2).sum(1), sk_const=np.zeros((S * NCLS, 3)), sk_constsq=np.zeros((S * NCLS, 3)),
             sk_chart=np.zeros((S * NCLS, 3), np.float32), sk_weight=weight.astype(np.float32), sk_legal=legal)
    d["spots"], d["theta"] = spots, np.asarray(true_theta, float)
    P = prepare(d)
    # const sets the theta-independent part so EV_true is a mixed spread around 0, chart = its softmax
    target = rng.normal(0, 1.5 * tau, (S * NCLS, 3))
    d["sk_const"] = (target - ev_at(P, P["theta0"])) * W[:, None]
    P["const"] = d["sk_const"]
    lg = (legal[:, None] >> np.arange(3) & 1).astype(bool).repeat(NCLS, 0)
    z = np.where(lg, ev_at(P, P["theta0"]) / tau, -np.inf)
    q = np.exp(z - z.max(1, keepdims=True))
    d["sk_chart"] = (q / q.sum(1, keepdims=True)).astype(np.float32)
    # the "sampler" ran at sample_theta: rec_pay there, meta.txt names it
    P["theta0"] = d["theta"] = np.asarray(sample_theta, float)
    d["rec_pay"] = pays(P, d["theta"]).astype(np.float32)
    d["sk_weight"][rng.random(len(weight)) < 0.1] = 0  # classes that never reach the spot
    return d


def selftest():
    c = lambda **kw: np.array([kw.get(f, v) for f, v in zip(FIELDS, [1.0, 0.8] + [0.0] * 8 + [1, 0, 1, 0])])
    hi, pair, suited, st, dom = class_features()
    names = ["AKQJT98765432"[min(r, q)] + "AKQJT98765432"[max(r, q)] + ("" if r == q else "s" if r < q else "o")
             for r in range(13) for q in range(13)]  # bucketName()
    K = names.index
    R = lambda t, last, raiser, spr, name, rr=False: float(realization(t, last, raiser, spr, rr, hi[K(name)],
                                                           pair[K(name)], suited[K(name)], st[K(name)], dom[K(name)]))
    near = lambda x, y: abs(x - y) < 1e-9
    # straights() / domination() (tests/utility_test.cpp)
    assert [st[K(n)] for n in ["98o", "KQo", "AKo", "A5o", "A4o", "K4o", "72o", "T6o", "99"]] == [4, 2, 1, 1, 1, 0, 0, 1, 0]
    assert [dom[K(n)] for n in ["KQo", "KJo", "AJo", "K6o", "76o", "AJs", "JJ"]] == [0, 1, 2, 4, 0, 0, 0]
    # realization(), hand-checked values from tests/utility_test.cpp
    hc = c(conn=0.03, dom=0.05, low=0.03, spr=3, callerShrink=1)
    g = 5 / 8
    assert near(R(hc, False, False, 5, "KJo"), (1 - 0.2 * g) * (1 + 0.03 * 2 * g - 0.05 - 0.03))
    for name in ["AA", "72o", "54s"]:
        for raiser in (False, True):
            assert near(R(c(), True, raiser, 1.3, name), 1) and near(R(c(), False, raiser, 18, name), 0.8)
    assert near(R(c(dom=0.05), True, False, 15, "K4o"), 0.8) and near(R(c(dom=0.05), True, True, 15, "K4o"), 1)
    assert near(R(c(set=0.1, conn=0.03, spr=3), True, False, 15, "33"), 1 + 0.1 * 15 / 18)
    assert near(R(c(set=0.1, conn=0.03, spr=3), True, False, 1.3, "98o"), 1 + 0.03 * 4 * 1.3 / 4.3)
    assert near(R(c(flush=0.08, low=0.03, spr=3), True, False, 18, "54s"), 1.08)
    assert near(R(c(flush=0.08, low=0.03, spr=3), True, False, 1.3, "72o"), 1 - 0.03 * 7)
    assert near(R(c(pairLow=0.02, spr=3), True, False, 1.3, "22"), 1 - 12 * 0.02)
    assert near(R(c(low=0.5), True, False, 15, "72o"), 0.05)  # floored
    d = DEFAULT
    deflt = np.array([d[f] for f in FIELDS], float)
    d1 = c(**{**d, "oopCall": 1})  # isolate the position term: the hand terms then cancel in the ratio
    assert near(R(d1, False, False, 1.3, "KJo") / R(d1, True, False, 1.3, "KJo"),
                R(d1, False, False, 18, "KJo") / R(d1, True, False, 18, "KJo"))  # callerShrink 0: no fade
    print("port: straights, domination, realization match tests/utility_test.cpp")

    # planted theta: sample at the defaults, chart = softmax(EV / tau) at a theta 1-2 steps away
    rng = np.random.default_rng(1)
    true = deflt + rng.choice([-2, -1, 1, 2], len(FIELDS)) * np.array([STEP[f] for f in FIELDS])
    true[0] = 1  # ip
    true[FIELDS.index("callerShrink")] = 0.3
    with tempfile.TemporaryDirectory() as tmp:
        save(tmp, synthetic(rng, 12, true, deflt, tau=1.0))
        P = prepare(load(tmp))
    check_port(P)
    f, _, _ = make_objective(P, P["theta0"], FIT, 1.0, 0.0, spot_weights(P, range(P["S"])))
    x = rng.normal(0, 0.5, len(FIT))
    _, gr = f(x)
    fd = np.array([(f(x + e)[0] - f(x - e)[0]) / 2e-5 for e in np.eye(len(FIT)) * 1e-5])
    assert np.allclose(gr, fd, rtol=1e-4, atol=1e-9), (gr, fd)
    print(f"gradient matches finite differences (max rel diff {np.max(np.abs(gr - fd) / np.abs(fd)):.1e})")
    th = fit(P, FIT, 1.0, 0.0, "_squeeze", 500)
    errs = {k: (th[i] - true[i]) / STEP[k] for i, k in enumerate(FIELDS)}
    print("recovered - planted, in steps: " + " ".join(f"{k} {v:+.1e}" for k, v in errs.items()))
    assert max(abs(v) for v in errs.values()) < 1e-2, "fit did not recover the planted theta"
    print("selftest passed")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dir", nargs="?")
    ap.add_argument("--tau", type=float, default=1.0, help="softmax temperature in bb")
    ap.add_argument("--l2", type=float, default=1e-3, help="penalty per squared step away from the start theta")
    ap.add_argument("--fit", default=",".join(FIT), help="comma-separated fields to fit; the rest stay at meta.txt")
    ap.add_argument("--holdout", default="", help="leave spots whose name contains this out of the fit")
    ap.add_argument("--maxiter", type=int, default=200)
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.dir:
        ap.error("need a sample directory")
    fields = args.fit.split(",")
    if bad := [f for f in fields if f not in FIELDS]:
        ap.error(f"unknown fields {bad}; choose from {' '.join(FIELDS)}")
    t = time.time()
    P = prepare(load(args.dir))
    print(f"loaded {P['nrec']} records, {P['nent']} live entries, {P['S']} spots in {time.time() - t:.1f}s")
    check_port(P)
    fit(P, fields, args.tau, args.l2, args.holdout, args.maxiter)


if __name__ == "__main__":
    main()
