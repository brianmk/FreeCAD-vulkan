#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Verify the committed Vulkan SPIR-V headers are not stale.

Every generated header starts with a self-describing stamp::

    // coin-spirv source=<rel> source-sha256=<hex> generator-sha256=<hex>

where ``<rel>`` is the GLSL path relative to ``data/shaders/vulkan`` and the
source hash is the SHA-256 of the concatenated SHA-256 digests of the shader
and its same-directory ``#include`` closure (exactly what
``data/GenerateVulkanSpirvHeader.cmake`` computes).

This checker re-derives the source hash from the GLSL in the tree and fails
when it differs from the committed stamp, i.e. when a shader was edited but its
``.spv.h`` was not regenerated with the ``coin_regenerate_vulkan_spirv``
target.  It deliberately does NOT run glslangValidator, so it is
toolchain-independent and safe to run in CI where the glslang version need not
match the one that produced the committed SPIR-V bytes.

Usage::

    python3 tools/rendering/check_spirv_stamps.py [--coin-root PATH]

Exit status is 0 when every header is current, 1 otherwise.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

STAMP_RE = re.compile(
    r"^//\s*coin-spirv\s+source=(?P<source>\S+)\s+"
    r"source-sha256=(?P<source_hash>[0-9a-f]{64})\s+"
    r"generator-sha256=(?P<generator_hash>[0-9a-f]{64})\s*$"
)
INCLUDE_RE = re.compile(r'#[ \t]*include[ \t]+"([^"]+)"')


def source_hash(glsl: Path) -> str:
    """Replicate GenerateVulkanSpirvHeader.cmake's source-hash algorithm."""
    files = [glsl]
    for match in INCLUDE_RE.finditer(glsl.read_text(encoding="utf-8")):
        include = glsl.parent / match.group(1)
        if include.is_file():
            files.append(include)
    concat = "".join(
        hashlib.sha256(path.read_bytes()).hexdigest() for path in files
    )
    return hashlib.sha256(concat.encode("ascii")).hexdigest()


def default_coin_root() -> Path:
    # tools/rendering/check_spirv_stamps.py -> repo root -> src/3rdParty/coin
    return Path(__file__).resolve().parents[2] / "src" / "3rdParty" / "coin"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--coin-root",
        type=Path,
        default=default_coin_root(),
        help="Path to the bundled Coin submodule (default: repo-relative).",
    )
    args = parser.parse_args()

    header_root = args.coin_root / "src" / "rendering" / "vulkan"
    shader_root = args.coin_root / "data" / "shaders" / "vulkan"
    if not header_root.is_dir():
        print(f"error: header root not found: {header_root}", file=sys.stderr)
        return 1

    headers = sorted(header_root.rglob("*.spv.h"))
    if not headers:
        print(f"error: no .spv.h headers under {header_root}", file=sys.stderr)
        return 1

    failures: list[str] = []
    for header in headers:
        first_line = header.read_text(encoding="utf-8").splitlines()[:1]
        stamp = STAMP_RE.match(first_line[0]) if first_line else None
        if stamp is None:
            failures.append(f"{header}: missing or malformed coin-spirv stamp")
            continue
        glsl = shader_root / stamp.group("source")
        if not glsl.is_file():
            failures.append(f"{header}: source not found: {glsl}")
            continue
        actual = source_hash(glsl)
        expected = stamp.group("source_hash")
        if actual != expected:
            failures.append(
                f"{header}:\n"
                f"    source:   {glsl}\n"
                f"    stamped:  {expected}\n"
                f"    computed: {actual}"
            )

    if failures:
        print(
            "Stale Vulkan SPIR-V header(s) detected. A shader changed without "
            "regenerating its .spv.h.\nRun the coin_regenerate_vulkan_spirv "
            "target and commit the result:\n",
            file=sys.stderr,
        )
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1

    print(f"OK: {len(headers)} Vulkan SPIR-V header stamps are current")
    return 0


if __name__ == "__main__":
    sys.exit(main())
