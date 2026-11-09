#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Entry point of the Resistance 3 launcher (see launcher/r3_launcher/cli.py).

    python3 r3.py --game <PS3_GAME>      first run: the PS3_GAME folder of your disc dump
    python3 r3.py                        later runs

Kept free of newer syntax so that an old interpreter reaches the version check below.
"""
import os
import sys

MIN_PYTHON = (3, 10)

if sys.version_info < MIN_PYTHON:
    sys.exit("r3: Python %d.%d or newer is needed (this is %d.%d); macOS: brew install python"
             % (MIN_PYTHON + sys.version_info[:2]))

# realpath: a symlink to r3.py (in ~/bin, say) still finds the launcher beside the real file.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.realpath(__file__)), "launcher"))

from r3_launcher.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
