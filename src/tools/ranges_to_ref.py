#!/usr/bin/env python3
"""Convert ref/simple/ranges.md (pokertrainer.se "Cash 100 Simple" charts) into ref/simple/pt_*.txt
(docs/validation.md "Reference charts"). These pure charts are kept for comparison, outside the scored
top level of ref/: pass `--ref ref/simple` to validate to score against them. Re-running regenerates them.

Usage: src/tools/ranges_to_ref.py [ranges.md] [out dir]   (defaults: ref/simple/ranges.md, ref/simple/)

Each spot's hand list is checked against the combo count stated in the markdown, so a
transcription slip fails loudly instead of skewing the fit.
"""
import pathlib
import re
import sys

POS = {"LJ": 0, "HJ": 1, "CO": 2, "BTN": 3, "SB": 4, "BB": 5}  # LJ = UTG
CATEGORY = {
    "Unopened pot": "open",
    "Facing a raise": "vs_open",
    "Facing a 3-bet": "vs_3bet",
    "Facing a 4-bet": "vs_4bet",
    "Facing an all-in 5-bet": "vs_5bet",
}
HEADER = "# pokertrainer.se Cash 100 Simple (pure chart, raked game)"


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


def combos(hand):
    return 6 if len(hand) == 2 else 4 if hand.endswith("s") else 12


def parse(text):
    """Yields (category, hero, villain or None, {"raise"/"call": (hands, stated combos)})."""
    cat, spot = None, None
    for line in text.splitlines() + ["### end"]:
        if line.startswith("### "):
            if spot and spot[3]:
                yield spot
            m = re.match(r"### (\w+)(?: vs (\w+))?", line)
            spot = (cat, m.group(1), m.group(2), {}) if cat and m.group(1) in POS else None
        elif line.startswith("## "):
            cat = CATEGORY.get(line[3:].strip())
        elif spot and (m := re.match(r"- \*\*(Raise|Call)\*\* \((\d+) combos, [\d.]+%\): (.*)", line)):
            hands = [] if m.group(3).strip() == "None" else [x.strip() for x in m.group(3).split(",")]
            spot[3][m.group(1).lower()] = (hands, int(m.group(2)))


def main():
    src = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "ref/simple/ranges.md")
    out = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else "ref/simple")
    n, bad = 0, 0
    for cat, hero, villain, acts in parse(src.read_text()):
        name = f"pt_open_{hero}" if cat == "open" else f"pt_{cat}_{hero}_vs_{villain}"
        lines = [HEADER, f"category: {cat}", f"spot: {history(cat, POS[hero], POS.get(villain))}"]
        for act in ("raise", "call"):  # an action missing from the markdown isn't legal there
            if act in acts:
                hands, stated = acts[act]
                if sum(map(combos, hands)) != stated:
                    print(f"{name}: {act} lists {sum(map(combos, hands))} combos, markdown says {stated}")
                    bad += 1
                lines.append(f"{act}: {','.join(hands)}")
        (out / f"{name.lower()}.txt").write_text("\n".join(lines) + "\n")
        n += 1
    print(f"wrote {n} spots to {out}/" + (f", {bad} combo-count mismatches" if bad else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
