#!/usr/bin/env python3
"""Assert a shader's std140 block layouts match the C++ struct mirrors.

SPIRV-Reflect is not packaged for this repo's toolchains, so this reads the
same decorations SPIRV-Reflect exposes through the `spirv-dis` text form:
each `layout(...) uniform` block's member names and offsets, and the block's
total size (recovered from the member type sizes).

The expected layouts in spirv_layouts.json mirror the C++ structs that carry
the matching `static_assert`s:

  src/3rdParty/coin/src/rendering/SoVulkanRenderBackend/SoVulkanRenderBackendP.h
      VulkanPushConstants, VulkanBackgroundPush, VulkanLightingUbo, VulkanDrawUbo

Usage:
  spirv_layout_check.py <module.spv> [--layouts spirv_layouts.json]
                        [--spirv-dis /usr/bin/spirv-dis]

Exit status: 0 = all blocks in the module match, 1 = mismatch, 2 = tool/usage
error.  A module with no recognised block is a pass (shaders without uniform
blocks are unaffected).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_DEFAULT_LAYOUTS = os.path.join(_HERE, "spirv_layouts.json")

# `%12 = OpTypeVector %float 4`, `%3 = OpName %5 "name"`, etc.  The leading id
# is optional in the disassembly for forward-referenced result ids.
_LINE = re.compile(r"^\s*(?:%(\d+)\s*=\s*)?(Op\w+)\s*(.*)$")


def _parse_disassembly(text: str):
    """Return (names, member_names, member_offsets, types) from spirv-dis text.

    types maps result-id -> (opcode, operands...) for the OpType* instructions
    needed to size block members.
    """
    names: dict[int, str] = {}
    member_names: dict[tuple[int, int], str] = {}
    member_offsets: dict[tuple[int, int], int] = {}
    types: dict[int, tuple[str, list[str]]] = {}
    structs: dict[int, list[int]] = {}
    constants: dict[int, int] = {}

    for raw in text.splitlines():
        line = raw.split(";", 1)[0].strip()
        if not line:
            continue
        m = _LINE.match(line)
        if not m:
            continue
        result, op, rest = m.group(1), m.group(2), m.group(3)
        parts = rest.split()
        if op == "OpName" and len(parts) >= 2:
            names[int(parts[0].lstrip("%"))] = parts[1].strip('"')
        elif op == "OpMemberName" and len(parts) >= 3:
            member_names[(int(parts[0].lstrip("%")), int(parts[1]))] = \
                parts[2].strip('"')
        elif op == "OpMemberDecorate" and len(parts) >= 3 and parts[2] == "Offset":
            member_offsets[(int(parts[0].lstrip("%")), int(parts[1]))] = \
                int(parts[3])
        elif op in ("OpConstant", "OpSpecConstant") and result is not None \
                and len(parts) >= 2:
            # `%id = OpConstant %type <value>`; arrays use the id as length.
            try:
                constants[int(result)] = int(parts[1])
            except ValueError:
                pass
        elif op == "OpTypeStruct" and result is not None:
            structs[int(result)] = [int(p.lstrip("%")) for p in parts]
        elif op.startswith("OpType") and result is not None:
            types[int(result)] = (op, parts)

    return names, member_names, member_offsets, types, structs, constants


def _type_size(type_id: int, types, structs, constants, depth: int = 0) -> int:
    """Size in bytes of a (scalar/vector/matrix/array/struct) type id."""
    if depth > 16:
        return 0
    if type_id in types:
        op, parts = types[type_id]
        if op in ("OpTypeFloat", "OpTypeInt"):
            return 4
        if op == "OpTypeVector":
            return 4 * int(parts[1])
        if op == "OpTypeMatrix":
            return _type_size(int(parts[0].lstrip("%")), types, structs,
                              constants, depth + 1) * int(parts[1])
        if op == "OpTypeArray":
            # Array length is a constant id, not a literal.
            length = constants.get(int(parts[1].lstrip("%")))
            if length is None:
                return 0
            return _type_size(int(parts[0].lstrip("%")), types, structs,
                              constants, depth + 1) * length
        if op == "OpTypePointer":
            return 8
    if type_id in structs:
        total = 0
        for member in structs[type_id]:
            total = max(total, _type_size(member, types, structs, constants,
                                          depth + 1))
        return total
    return 0


def check(module: str, layouts: dict, spirv_dis: str) -> int:
    # --raw-id keeps result ids numeric; without it spirv-dis prints friendly
    # (symbolic) names, which the decoration parser cannot key on.
    proc = subprocess.run([spirv_dis, "--raw-id", module],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(f"[reflect] spirv-dis failed on {module}:\n"
                         f"{proc.stderr}\n")
        return 2

    names, member_names, member_offsets, types, structs, constants = \
        _parse_disassembly(proc.stdout)

    expected_blocks: dict = layouts.get("blocks", {})
    failures: list[str] = []
    checked = 0

    # Group decorated member offsets by struct id.
    by_struct: dict[int, list[tuple[int, int]]] = {}
    for (struct_id, index), offset in member_offsets.items():
        by_struct.setdefault(struct_id, []).append((index, offset))

    for struct_id, decorated in by_struct.items():
        block_name = names.get(struct_id)
        if block_name not in expected_blocks:
            continue
        spec = expected_blocks[block_name]
        want_members: dict = spec.get("members", {})
        want_size: int = int(spec.get("size", 0))
        checked += 1
        end = 0
        members = structs.get(struct_id, [])
        for index, offset in sorted(decorated):
            name = member_names.get((struct_id, index), f"#{index}")
            if name in want_members:
                if offset != want_members[name]:
                    failures.append(
                        f"{block_name}.{name}: offset {offset} != C++ "
                        f"{want_members[name]}")
            else:
                failures.append(
                    f"{block_name}.{name}: member not present in the C++ "
                    f"mirror (offset {offset})")
            if index < len(members):
                end = max(end, offset + _type_size(members[index], types,
                                                   structs, constants))
        if want_size and end > want_size:
            failures.append(
                f"{block_name}: shader block ends at {end} > C++ sizeof "
                f"{want_size}")

    if checked == 0:
        # A shader with no uniform block (or only blocks we do not model) is
        # not a failure; report it so the caller can see coverage.
        sys.stderr.write(f"[reflect] {os.path.basename(module)}: no modelled "
                         "blocks\n")
        return 0

    if failures:
        for f in failures:
            sys.stderr.write(f"[reflect] FAIL {os.path.basename(module)}: {f}\n")
        return 1

    sys.stderr.write(f"[reflect] OK {os.path.basename(module)}: "
                     f"{checked} block(s) match the C++ mirrors\n")
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("module", help="SPIR-V module (.spv)")
    parser.add_argument("--layouts", default=_DEFAULT_LAYOUTS,
                        help="expected-layout JSON (default: spirv_layouts.json)")
    parser.add_argument("--spirv-dis", default=None,
                        help="path to spirv-dis (default: search PATH)")
    args = parser.parse_args(argv)

    if not os.path.isfile(args.module):
        sys.stderr.write(f"[reflect] no such module: {args.module}\n")
        return 2
    with open(args.layouts, encoding="utf-8") as f:
        layouts = json.load(f)
    dis = args.spirv_dis or shutil.which("spirv-dis")
    if not dis:
        sys.stderr.write("[reflect] spirv-dis not found on PATH\n")
        return 2
    return check(args.module, layouts, dis)


if __name__ == "__main__":
    sys.exit(main())
