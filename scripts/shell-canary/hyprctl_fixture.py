#!/usr/bin/env python3
"""Empty compositor data and neutral night-light state used under Sway.

This is test state, not a Hyprland integration test. Unknown queries fail and
are recorded so expanding the shell's dependency surface needs review.
"""
import json
from pathlib import Path
import sys

QUERIES = {
    ('-j', 'clients'): [],
    ('-j', 'devices'): {'keyboards': []},
    ('devices', '-j'): {'keyboards': []},
    ('-j', 'monitors'): [],
    ('monitors', '-j'): [],
    ('monitors', 'all', '-j'): [],
    ('hyprsunset', 'temperature'): 6500,
    ('-j', 'getoption', 'decoration:rounding'): {'int': 0},
    ('-j', 'getoption', 'general:gaps_out'): {'custom': '0 0 0 0'},
    # Omarchy 4.5: reduced-motion check and the workspace layout indicator.
    ('-j', 'getoption', 'animations:enabled'): {'bool': True, 'set': True},
    ('getoption', 'general.layout', '-j'): {'str': 'dwindle', 'set': True},
    ('activeworkspace', '-j'): {'id': 1, 'name': '1', 'tiledLayout': 'dwindle'},
    # The background intro hands the cursor back when it ends (fire and forget).
    ('eval', 'if omarchy_startup_cursor_restore then omarchy_startup_cursor_restore() end'): 'ok',
}


def main():
    args = tuple(sys.argv[1:])
    with Path('/results/hyprctl-fixture.log').open('a') as log:
        log.write(json.dumps(args) + '\n')
    if args not in QUERIES:
        print('Unsupported headless hyprctl query: ' + repr(args), file=sys.stderr)
        return 1
    print(json.dumps(QUERIES[args]))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
