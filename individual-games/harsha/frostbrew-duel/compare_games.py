#!/usr/bin/env python3
"""Compare raw client-game source lines; this is not a measure of engine reuse."""

import difflib
import json
from pathlib import Path


def compare(before, after):
    common = sum(block.size for block in difflib.SequenceMatcher(
        None, before, after, autojunk=False).get_matching_blocks())
    total = len(before) + len(after)
    return {
        "apex_lines": len(before),
        "frostbrew_lines": len(after),
        "matching_lines": common,
        "apex_different_percent": round(100 * (len(before) - common) / max(1, len(before)), 2),
        "frostbrew_different_percent": round(100 * (len(after) - common) / max(1, len(after)), 2),
        "symmetric_different_percent": round(100 * (total - 2 * common) / max(1, total), 2),
    }


def lines(paths):
    return [line for path in sorted(paths) for line in path.read_text().splitlines()]


if __name__ == "__main__":
    assert compare(["a", "b"], ["a", "c"])["symmetric_different_percent"] == 50
    assert compare([], ["a"])["symmetric_different_percent"] == 100
    game = Path(__file__).resolve().parent
    root = game.parent
    apex = list(root.glob("include/Apex*.hpp"))
    apex += [p for p in root.glob("src/Apex*.cpp") if p.name != "ApexNetworkServerMain.cpp"]
    apex += list(root.glob("include/animation/*.hpp")) + list(root.glob("src/animation/*.cpp"))
    apex.append(root / "src/apexAscent.cpp")
    frostbrew = list(game.glob("include/Frostbrew*.hpp")) + list(game.glob("src/Frostbrew*.cpp"))
    frostbrew.append(game / "src/frostbrewDuel.cpp")
    report = compare(lines(apex), lines(frostbrew))
    report["method"] = "Raw lines, sorted client source files concatenated, SequenceMatcher with autojunk disabled."
    report["limitations"] = "Matching includes blanks/braces. Excludes engine, tests, assets, generated files and Apex headless server. Different gameplay and networking scope affect the comparison."
    report["apex_files"] = [str(p.relative_to(root)) for p in sorted(apex)]
    report["frostbrew_files"] = [str(p.relative_to(root)) for p in sorted(frostbrew)]
    print(json.dumps(report, indent=2))
