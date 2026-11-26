#!/usr/bin/env python3
"""Pairwise raw client-source comparison; not an engine-reuse percentage."""
import importlib.util
import itertools
import json
import sys
from pathlib import Path


def compare_games():
    game = Path(__file__).resolve().parent
    root = game.parent
    spec = importlib.util.spec_from_file_location('frostbrew_compare', root / 'frostbrew-duel/compare_games.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    assert helper.compare(['a', 'b'], ['a', 'c'])['symmetric_different_percent'] == 50
    if '--self-test' in sys.argv:
        return
    apex = [*root.glob('include/Apex*.hpp'), *root.glob('include/animation/*.hpp'),
            *root.glob('src/animation/*.cpp'), root / 'src/apexAscent.cpp']
    apex += [p for p in root.glob('src/Apex*.cpp') if p.name != 'ApexNetworkServerMain.cpp']
    games = {'apex': apex}
    for name, folder in [('frostbrew', root / 'frostbrew-duel'), ('neon', game)]:
        games[name] = [*folder.glob('include/*.hpp'), *folder.glob('src/*.cpp')]
    report = {'method': 'Sorted raw client-source lines, SequenceMatcher without autojunk. Includes comments, braces and blanks.',
              'limitations': 'Excludes engine, tests, benchmark harnesses, assets and servers. This measures source similarity, not engine reuse.', 'pairs': []}
    for left, right in itertools.combinations(games, 2):
        before, after = helper.lines(games[left]), helper.lines(games[right])
        if not before or not after:
            raise ValueError('Each game must have source files')
        result = helper.compare(before, after)
        common = result['matching_lines']
        report['pairs'].append({'games': [left, right], 'lines': [len(before), len(after)],
                                'matching_lines': common, 'unchanged_percent': [round(100 * common / len(before), 2), round(100 * common / len(after), 2)],
                                'symmetric_different_percent': result['symmetric_different_percent']})
    report['files'] = {name: [str(p.relative_to(root)) for p in sorted(files)] for name, files in games.items()}
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    compare_games()
