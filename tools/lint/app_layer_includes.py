#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Guard against Gui-layer includes in App-layer code.

FreeCAD's layers are Base < App < Gui.  An App-layer target (FreeCADApp,
FreeCADBase, or a workbench's ``*App`` module) that includes a ``Gui/`` header
inverts that order: it makes the non-GUI layer depend on the GUI layer, which
breaks headless builds and hides the real dependency.

This is intentionally a direct-include scan (cheap, high-signal); it does not
follow transitive includes.

Usage:
    python3 tools/lint/app_layer_includes.py [--root DIR]

Exit status is non-zero when any violation is found.
"""

import argparse
import os
import re
import sys

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]Gui/')
CODE_EXT = ('.h', '.hpp', '.hh', '.hxx', '.cpp', '.cxx', '.cc', '.c')


def app_layer_roots(root):
    """Directories that make up the App (non-GUI) layer."""
    roots = [os.path.join(root, 'src', 'App'), os.path.join(root, 'src', 'Base')]
    mod = os.path.join(root, 'src', 'Mod')
    if os.path.isdir(mod):
        for name in sorted(os.listdir(mod)):
            app = os.path.join(mod, name, 'App')
            if os.path.isdir(app):
                roots.append(app)
    return roots


def scan(root):
    violations = []
    for base in app_layer_roots(root):
        for dirpath, _dirnames, filenames in os.walk(base):
            if '3rdParty' in dirpath.split(os.sep):
                continue
            for filename in filenames:
                if not filename.endswith(CODE_EXT):
                    continue
                path = os.path.join(dirpath, filename)
                with open(path, encoding='utf-8', errors='replace') as fh:
                    for lineno, line in enumerate(fh, start=1):
                        if INCLUDE_RE.match(line):
                            violations.append(
                                (os.path.relpath(path, root), lineno, line.strip())
                            )
    return violations


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '--root',
        default=os.path.dirname(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        ),
        help='Repository root (default: the checkout containing this script).',
    )
    args = parser.parse_args()

    violations = scan(args.root)
    if violations:
        print(
            'App-layer sources must not include Gui/ headers (layer inversion):',
            file=sys.stderr,
        )
        for path, lineno, line in violations:
            print(f'  {path}:{lineno}: {line}', file=sys.stderr)
        print(f'\n{len(violations)} violation(s).', file=sys.stderr)
        return 1

    print('App-layer include check OK (no Gui/ includes).')
    return 0


if __name__ == '__main__':
    sys.exit(main())
