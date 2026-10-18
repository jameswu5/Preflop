#!/usr/bin/env python3
"""Read every 6-max 100bb cash range from app.pokertrainer.se/range-viewer (65 spots, GTO and PTO)
into reference charts with mixed frequencies (docs/validation.md "Reference charts"). Personal use
only: the output stays in the git-ignored ref/.

    pip install playwright && playwright install chromium
    src/tools/scrape_ranges.py [--out ref] [--modes gto,pto]   # -> ref/gto_*.txt, ref/pto/pto_*.txt

Each cell's raise/call frequencies are read from its --raise-percentage / --call-percentage style
variables; the card's sizing line (e.g. "3-bet 7.5bb") goes into the header comment. Each page's
title is checked against the spot so a URL the app ignores fails loudly.
"""
import argparse
import pathlib

BASE = "https://app.pokertrainer.se/range-viewer?format=cash&players=6&stack=100"
SCENARIO = {"open": "open", "vs_open": "vs-raise", "vs_3bet": "vs-3bet", "vs_4bet": "vs-4bet", "vs_5bet": "vs-5bet"}
POS = {"LJ": 0, "HJ": 1, "CO": 2, "BTN": 3, "SB": 4, "BB": 5}  # LJ = UTG
CELLS = ".range-chart-card ion-grid.rangeGrid ion-button"
READ = """els => els.map(e => [e.innerText.trim(), ...['--raise-percentage', '--call-percentage'].map(v => {
    const x = parseFloat(getComputedStyle(e).getPropertyValue(v)) || 0;
    return Math.min(x, 1);  // the site writes the odd 100% cell as Infinity
})])"""


def history(cat, h, v):
    """F/C/R letters from UTG to hero's decision; everyone not named folds."""
    if cat == "open":
        return "F" * h
    if cat == "vs_open":  # V opened, H faces it
        return "F" * v + "R" + "F" * (h - v - 1)
    if cat == "vs_3bet":  # H opened, V 3-bet
        return "F" * h + "R" + "F" * (v - h - 1) + "R" + "F" * (5 - v)
    if cat == "vs_4bet":  # V opened, H 3-bet, V 4-bet
        return "F" * v + "R" + "F" * (h - v - 1) + "R" + "F" * (5 - h) + "R"
    if cat == "vs_5bet":  # H opened, V 3-bet, H 4-bet, V shoved
        return "F" * h + "R" + "F" * (v - h - 1) + "R" + "F" * (5 - v) + "RR"
    raise ValueError(cat)


def spots():
    """(category, hero, villain or None); the villain acts before the hero facing a raise or 4-bet."""
    names = list(POS)
    for h in names[:5]:
        yield "open", h, None
    for cat, hero_later in (("vs_open", True), ("vs_3bet", False), ("vs_4bet", True), ("vs_5bet", False)):
        for h in names:
            for v in names:
                if (POS[v] < POS[h]) == hero_later and v != h:
                    yield cat, h, v


def fmt(cells, i):
    return ",".join(f"{h}:{f[i]:.3f}".replace(":1.000", "") for h, *f in cells if f[i] >= 0.0005)


def main():
    from playwright.sync_api import expect, sync_playwright

    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="ref")
    ap.add_argument("--modes", default="gto,pto")
    ap.add_argument("--headed", action="store_true", help="show the browser (e.g. to log in)")
    args = ap.parse_args()
    with sync_playwright() as p:
        page = p.chromium.launch(headless=not args.headed).new_page(viewport={"width": 720, "height": 1400})
        for mode in args.modes.split(","):
            out = pathlib.Path(args.out) / ("" if mode == "gto" else mode)  # GTO is the default target
            out.mkdir(parents=True, exist_ok=True)
            n = 0
            for cat, hero, vil in spots():
                page.goto(f"{BASE}&rangeType={mode}&scenario={SCENARIO[cat]}&hero={hero}" + (f"&villain={vil}" if vil else ""))
                title = page.locator(".range-chart-card .range-context-title")
                expect(title).to_contain_text(hero, timeout=20000)
                if vil:
                    expect(title).to_contain_text(vil)
                page.wait_for_function(f"document.querySelectorAll('{CELLS}').length == 169")
                cells = page.locator(CELLS).evaluate_all(READ)
                info = page.locator(".range-chart-card").inner_text().split("\n")[:2]  # title, sizing
                for cell in cells:  # the site has the odd cell summing past 1: rescale it
                    if (s := cell[1] + cell[2]) > 1.0005:
                        print(f"  {mode} {info[0]}: {cell[0]} raise {cell[1]:.4f} + call {cell[2]:.4f} > 1, rescaled")
                        cell[1], cell[2] = cell[1] / s, cell[2] / s
                name = f"{mode}_open_{hero}" if cat == "open" else f"{mode}_{cat}_{hero}_vs_{vil}"
                (out / f"{name.lower()}.txt").write_text(
                    f"# pokertrainer.se range viewer {mode.upper()}, cash 6-max 100bb: {' | '.join(info)}\n"
                    f"category: {cat}\nspot: {history(cat, POS[hero], POS.get(vil))}\n"
                    + "".join(f"{a}: {r}\n" for a, r in (("raise", fmt(cells, 0)), ("call", fmt(cells, 1))) if r))
                n += 1
                print(f"[{mode} {n:2d}] {' | '.join(info)}")
            print(f"wrote {n} charts to {out}/")


if __name__ == "__main__":
    main()
