#!/usr/bin/env python3
"""Self-test for the freecad_probe Tier-1 core (no FreeCAD needed)."""
import argparse
import json
import os
import sys

sys.path.insert(0, "/tmp/opencode")
import freecad_probe as fp

PASS = True


def check(name, cond):
    global PASS
    if not cond:
        PASS = False
        print(f"FAIL: {name}")
    else:
        print(f"ok:   {name}")


# --- parse_event: each source ---
e = fp.parse_event('[PICKPROBE] event=hover pos=100,200 hit=1,2,3 obj=Box '
                   'bbox=0,0,0..10,10,10 local=5,5,5 sub=Face6')
check("PICKPROBE source", e and e["source"] == "PICKPROBE")
check("PICKPROBE kind", e and e["kind"] == "hover")
check("PICKPROBE fields obj", e and e["fields"].get("obj") == "Box")
check("PICKPROBE fields sub", e and e["fields"].get("sub") == "Face6")

check("VK-TRACE", fp.parse_event("[VK-TRACE] SoFCUnifiedSelection motion x=1")["source"] == "VK-TRACE")
check("VKBE", fp.parse_event("[VKBE] line cmd=5")["source"] == "VKBE")
h = fp.parse_event("[HARNESS] pick c=42 hit=1,2,3")
check("HARNESS", h and h["source"] == "HARNESS" and h["kind"] == "pick" and h["fields"]["c"] == "42")
v = fp.parse_event("[VERDICT] PICKPROBE PASS")
check("VERDICT", v and v["fields"] == {"name": "PICKPROBE", "result": "PASS"})
check("VERDICT result", fp.extract_verdict(["[VERDICT] PICKPROBE PASS\n"]) == "PASS")
check("unknown ignored", fp.parse_event("just a normal log line") is None)

# --- Khronos validation classification ---
vuid = '[VUID-vkCmdDraw-indexSize-00492] Validation Error: [ VUID-vkCmdDraw-indexSize-00492 ]'
ve = fp.parse_event(vuid)
check("VUID parsed", ve and ve["source"] == "VK-VALIDATION")
check("VUID id", ve and ve["fields"].get("vuid") == "VUID-vkCmdDraw-indexSize-00492")
check("VUID level", ve and ve["fields"].get("level") == "ERROR")
check("extract_validation", len(fp.extract_validation([vuid + "\n", "noise\n"])) == 1)

# --- RunReport + artifact writer ---
adir = fp.new_artifact_dir("/tmp/opencode", "selftest")
rep = fp.RunReport(name="selftest", artifact_dir=adir)
rep.log_event("PICKPROBE", "hover", obj="Box")
rep.add_error("boom")
rep.mark("FAIL")
p = rep.write()
with open(p) as f:
    data = json.load(f)
check("report.json exists", os.path.exists(p))
check("report verdict", data["verdict"] == "FAIL")
check("report event", data["events"][0]["source"] == "PICKPROBE")
check("report error", data["errors"] == ["boom"])
check("report artifacts", "report.json" in data["artifacts"])

# --- full run_case via a stub binary (python3 running a tiny script) ---
stub = "/tmp/opencode/_fc_probe_stub.py"
with open(stub, "w") as f:
    f.write(
        "import sys\n"
        "print('[HARNESS] phase=a x=1')\n"
        "print('[VERDICT] SAMPLE PASS')\n"
        "print('[VUID-vkCmdDraw-indexSize-00492] Validation Error: [ X ]')\n"
        "sys.exit(0)\n"
    )
report = fp.run_case(
    script=stub,
    binary="/usr/bin/python3",
    profile="vulkan",
    out_dir="/tmp/opencode/runs",
    timeout=30,
    report_name="stubcase",
    validation=True,
    fail_on_validation=False,
)
check("run verdict PASS", report.verdict == "PASS")
check("run session validation", report.session["validation"] is True)
check("run validation count", report.session["validation_count"] == 1)
sources = {ep["source"] for ep in report.events}
check("run has HARNESS event", "HARNESS" in sources)
check("run has VK-VALIDATION event", "VK-VALIDATION" in sources)
check("run session exit 0", report.session["exit_code"] == 0)

# --- CLI parser builds + help ---
try:
    fp.main(["run", "--help"])
except SystemExit:
    pass
check("CLI parser builds", True)
_run_sub = None
for action in fp._build_parser()._actions:
    if isinstance(action, argparse._SubParsersAction):
        _run_sub = action.choices.get("run")
check("CLI --validation flag present", "--validation" in _run_sub.format_help())

# --- Tier 2: drawlist hash + frame diff + snapshot ---
events = [{"source": "VKBE", "text": "[VKBE] line cmd=1"},
          {"source": "VKBE", "text": "[VKBE] line cmd=2"}]
check("drawlist digest stable", fp.drawlist_digest(events) == fp.drawlist_digest(list(reversed(events))))
check("drawlist digest len", len(fp.drawlist_digest(events)) == 64)
check("vkbe_lines", len(fp.vkbe_lines(["[VKBE] a b=1\n", "noise\n"])) == 1)

from PIL import Image
fdir = os.path.join(adir, "frames")
bdir = os.path.join(adir, "baseline")
os.makedirs(fdir, exist_ok=True)
os.makedirs(bdir, exist_ok=True)
Image.new("RGB", (4, 4), (255, 0, 0)).save(os.path.join(fdir, "frame_0.png"))
Image.new("RGB", (4, 4), (255, 0, 0)).save(os.path.join(fdir, "frame_1.png"))
Image.new("RGB", (4, 4), (255, 0, 0)).save(os.path.join(bdir, "frame_0.png"))
Image.new("RGB", (4, 4), (0, 0, 255)).save(os.path.join(bdir, "frame_1.png"))
m0 = fp.image_metrics(os.path.join(fdir, "frame_0.png"), os.path.join(bdir, "frame_0.png"))
m1 = fp.image_metrics(os.path.join(fdir, "frame_1.png"), os.path.join(bdir, "frame_1.png"))
check("image_metrics identical", m0["mean_abs"] == 0.0)
check("image_metrics diff", m1["mean_abs"] > 0.0)
errs = fp.compare_frames(bdir, fdir)
check("compare_frames catches diff", any("frame_1" in e for e in errs))
check("frame_hashes count", len(fp.frame_hashes(fdir)) == 2)
snap = fp.extract_snapshot(
    [{"source": "HARNESS", "kind": "snapshot",
      "fields": {"state": '{"viewport":{"w":2,"h":3,"dpr":1}}'}}]
)
check("extract_snapshot", snap and snap["viewport"]["w"] == 2)
check("extract_snapshot none", fp.extract_snapshot([]) is None)

print("=== RESULT:", "PASS" if PASS else "FAIL", "===")
sys.exit(0 if PASS else 1)
