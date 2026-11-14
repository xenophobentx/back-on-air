#!/usr/bin/env python3
"""Launcher for the rdvd package beside this file (works with python3 -I, which ignores the current directory)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from rdvd.__main__ import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
