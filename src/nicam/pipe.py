from __future__ import annotations

import os
import sys


def suppress_stdout_broken_pipe() -> None:
    """Avoid Python's final stdout flush traceback after a downstream pipe closes."""
    try:
        devnull = os.open(os.devnull, os.O_WRONLY)
        os.dup2(devnull, sys.stdout.fileno())
        os.close(devnull)
    except OSError:
        pass
