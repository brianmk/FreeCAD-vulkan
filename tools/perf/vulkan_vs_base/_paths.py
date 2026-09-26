"""Shared path resolution for the Vulkan-vs-base benchmark scripts.

These scripts originally hard-coded the author's ``/tmp/opencode/perf_compare``
workspace and absolute build paths.  Binaries are now overridable with the
``PC_BASE_BIN`` / ``PC_FORK_BIN`` environment variables (and ``PC_VORON`` for
the vorontest document); probe scripts and result directories resolve relative
to this file.
"""

import os

HERE = os.path.dirname(os.path.abspath(__file__))
# .../FreeCAD/tools/perf/vulkan_vs_base -> .../FreeCAD
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))


def _default_bin(*parts):
    return os.path.join(REPO, *parts)


BASE = os.environ.get("PC_BASE_BIN") or _default_bin(
    "FreeCAD_base", "FreeCAD", "build", "release", "bin", "FreeCAD"
)
FORK = os.environ.get("PC_FORK_BIN") or _default_bin(
    "build", "release-vulkan", "bin", "FreeCAD"
)
VORON = os.environ.get("PC_VORON") or os.path.expanduser(
    "~/Documents/vorontest.FCStd"
)


def here(name):
    return os.path.join(HERE, name)


def out_dir(name):
    return os.path.join(HERE, name)
