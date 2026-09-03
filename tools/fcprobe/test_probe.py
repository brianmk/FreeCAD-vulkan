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

# --- Tier 3: pick-trace compare + run diff + matrix ---
check("_floats", fp._floats("1,2,3", 3) == [1.0, 2.0, 3.0])
check("_floats pad", fp._floats("1", 3) == [1.0, 0.0, 0.0])
A = [{"event": "hover", "pos": "100,200", "hit": "1,2,3", "obj": "Box"},
     {"event": "click", "pos": "300,400", "hit": "4,5,6", "obj": "Box"}]
check("pick identical", fp.diff_pick_traces([dict(x) for x in A], [dict(x) for x in A]) == [])
B = [{"event": "hover", "pos": "105,200", "hit": "1,2,3", "obj": "Box"}, dict(A[1])]
check("pick pos drift", any("pos" in e for e in fp.diff_pick_traces(A, B)))
C = [dict(A[0]), {"event": "click", "pos": "300,400", "hit": "4,5,7", "obj": "Box"}]
check("pick hit drift", any("hit" in e for e in fp.diff_pick_traces(A, C)))
check("pick count", any("count" in e for e in fp.diff_pick_traces(A, A[:1])))
D = [{"event": "hover", "pos": "100,200", "hit": "1,2,3", "obj": "Other"}, dict(A[1])]
check("pick object", any("object" in e for e in fp.diff_pick_traces(A, D)))

# run two stubs (emit pick lines) and diff_runs them -> MATCH
stubp = "/tmp/opencode/_fc_stub_pick.py"
with open(stubp, "w") as f:
    f.write(
        "import sys\n"
        "print('[PICKPROBE] event=hover pos=100,200 hit=1,2,3 obj=Box sub=Face1')\n"
        "print('[PICKPROBE] event=click pos=100,200 hit=1,2,3 obj=Box sub=Face1')\n"
        "print('[VERDICT] PICK PASS')\n"
        "sys.exit(0)\n"
    )
r1 = fp.run_case(stubp, binary="/usr/bin/python3", profile="vulkan",
                 out_dir="/tmp/opencode/runs", report_name="diffa")
r2 = fp.run_case(stubp, binary="/usr/bin/python3", profile="gl",
                 out_dir="/tmp/opencode/runs", report_name="diffb")
check("diff_runs match", fp.diff_runs(r1.artifact_dir, r2.artifact_dir) == [])
check("pick_trace_from_log", len(fp.pick_trace_from_log(
    ["[PICKPROBE] event=hover pos=1,2\n", "[PICKPROBE] event=click pos=3,4\n"])) == 2)

# matrix (stub binary) -> MATCH
mtx = fp.run_matrix(stubp, profiles=("vulkan", "gl"), out_dir="/tmp/opencode/matrix",
                    binary="/usr/bin/python3", report_name="mtx")
check("run_matrix pairs", len(mtx["pairs"]) == 1)
check("run_matrix match", mtx["pairs"][0][2] == [])

# --- crash/signal detection + tally + soak ---
check("tally_events", fp.tally_events([
    {"source": "PICKPROBE", "kind": "hover"},
    {"source": "PICKPROBE", "kind": "hover"},
    {"source": "VERDICT", "kind": "verdict"},
])["PICKPROBE:hover"] == 2)

# A stub that exits by signal to exercise crash detection.
stubsig = "/tmp/opencode/_fc_stub_sig.py"
with open(stubsig, "w") as f:
    f.write(
        "import os, signal, sys\n"
        "print('[HARNESS] before crash x=1')\n"
        "sys.stdout.flush()\n"
        "os.kill(os.getpid(), signal.SIGSEGV)\n"
    )
sigrep = fp.run_case(stubsig, binary="/usr/bin/python3", profile="vulkan",
                     out_dir="/tmp/opencode/runs", report_name="sigcase")
check("signal detected", sigrep.session.get("exit_signal") is not None)
check("crash -> ERROR", sigrep.verdict == "ERROR")
check("crash tail captured", sigrep.session.get("stdout_tail", "") != "")

# soak (stub PASS) -> ok
soak = fp.run_to_fail(stubp, max_runs=3, binary="/usr/bin/python3",
                      out_dir="/tmp/opencode/soak")
check("soak ok", soak["ok"] is True and soak["total_runs"] == 3)

# --- console error capture (probe exception) ---
console_lines = [
    "Exception while processing file: probe.py [Unknown file: probe.py]\n",
    "  File \"probe.py\", line 3, in <module>\n",
    "    import nope\n",
    "ModuleNotFoundError: No module named 'nope'\n",
    "\n",
    "some later line\n",
]
ce = fp._console_errors(console_lines)
check("console marker", any("Exception while processing file" in e for e in ce))
check("console traceback frame", any("ModuleNotFoundError" in e for e in ce))
check("console stops after blank", len(ce) < 5)

# a real-ish stub that throws -> run_case surfaces the exception as an error
stubexc = "/tmp/opencode/_fc_stub_exc.py"
with open(stubexc, "w") as f:
    f.write("raise RuntimeError('boom from probe')\n")
excrep = fp.run_case(stubexc, binary="/usr/bin/python3", profile="vulkan",
                     out_dir="/tmp/opencode/runs", report_name="exccase")
check("exception -> FAIL", excrep.verdict == "FAIL")
check("exception captured", any("boom from probe" in e for e in excrep.errors))

# --- legacy verdict format (PICKHARNESS VERDICT NAME PASS|FAIL) ---
check("legacy verdict PASS", fp.extract_verdict(
    ["[PICKPROBE] event=hover pos=1,2\n", "PICKHARNESS VERDICT PICKPROBE PASS\n"]) == "PASS")
check("legacy verdict FAIL", fp.extract_verdict(
    ["PICKHARNESS VERDICT PICKPROBE FAIL\n"]) == "FAIL")
check("modern verdict still works", fp.extract_verdict(
    ["[VERDICT] pick PASS\n"]) == "PASS")

# --- harness marker gates harness-only behavior ---------------------------
env = fp._merge_env("vulkan", {}, None)
check("harness marker set on runs", env.get("FC_HARNESS") == "1")
env_no_profile = fp._merge_env("gl", {}, None)
check("harness marker set on gl too", env_no_profile.get("FC_HARNESS") == "1")

# A probe that prints a terminal error and then idles (simulated dead probe/
# idle GUI).  The runner must close it promptly, NOT wait out the timeout.
stubhang = "/tmp/opencode/_fc_stub_hang.py"
with open(stubhang, "w") as f:
    f.write(
        "import sys, time\n"
        "print('Exception while processing file: probe.py [Unknown file: probe.py]')\n"
        "sys.stdout.flush()\n"
        "time.sleep(120)\n"
    )
hangrep = fp.run_case(stubhang, binary="/usr/bin/python3", profile="vulkan",
                      out_dir="/tmp/opencode/runs", report_name="hangcase",
                      timeout=30)
check("terminal closes run early (not TIMEOUT)", hangrep.verdict != "TIMEOUT")
check("terminal error captured", any(
    "Exception while processing file" in e for e in hangrep.errors))
check("terminal not a native crash", hangrep.session.get("exit_signal") is None)
check("probe-die run actually ends fast", hangrep.verdict in ("FAIL", "ERROR"))

# --- new: [VK-SET]/[OVL]/[PUSH]/[UBO] events + color-pixel counting ----
evs = list(fp.iter_events([
    "[VK-SET] pushSettings edges=1 points=0 edgeColor=(1.00,0.00,0.00,1.00)\n",
    "[OVL] wireframe=1 points=0 fillMode=1 edgeColor=(1.00,0.00,0.00,1.00)\n",
    "[PUSH] srcDiffuse=1 override=1 fillModeOverride=1\n",
    "[UBO] lighting=1 material=1\n",
]))
check("parse VK-SET", any(e["source"] == "VK-SET" and e["fields"].get("edges") == "1"
                          for e in evs))
check("parse OVL/PUSH/UBO", {e["source"] for e in evs} >= {"OVL", "PUSH", "UBO"})
vks = fp.extract_vksett(evs)
check("extract_vksett", len(vks) == 1 and vks[0]["kind"] == "pushSettings")

# tiny synthetic PNG -> count_color_pixels
try:
    from PIL import Image
    png = "/tmp/opencode/_px.png"
    im = Image.new("RGB", (10, 10), (0, 0, 0))
    for y in range(3):
        for x in range(4):
            im.putpixel((x, y), (255, 0, 0))
    im.save(png)
    red = fp.count_color_pixels(png, (255, 0, 0))
    check("count_color_pixels red", red == 12)
    check("count_color_pixels black", fp.count_color_pixels(png, (0, 0, 0)) == 88)
except ImportError:
    check("count_color_pixels (PIL missing)", True)

# --- Khronos validation handling (severity + per-VUID summary) ---
vl = [
    "The Vulkan spec states: each layout must be... (https://docs.vulkan.org/#VUID-VkImageMemoryBarrier-oldLayout-01197)\n",
    "The Vulkan spec states: present image... (https://docs.vulkan.org/#VUID-VkPresentInfoKHR-pImageIndices-01430)\n",
    "The Vulkan spec states: some barrier... (https://docs.vulkan.org/#VUID-VkImageMemoryBarrier-oldLayout-01197)\n",
]
vlev = list(fp.iter_events(vl))
check("vuid as WARN (not INFO)", all(
    e["fields"].get("level") == "WARN" for e in vlev if e.get("source") == "VK-VALIDATION"))
vs = fp.validation_summary(vlev)
check("validation_summary buckets", vs["VUID-VkImageMemoryBarrier-oldLayout-01197"]["count"] == 2)
check("validation_summary present", vs.get("VUID-VkPresentInfoKHR-pImageIndices-01430", {}).get("count") == 1)

# --- check_preferences (code-path + rendered-result assertion) ---
import os as _os
fdir = "/tmp/opencode/_pref_frames"
_os.makedirs(fdir, exist_ok=True)
from PIL import Image as _I
_I.new("RGB", (8, 8), (200, 200, 200)).save(f"{fdir}/frame_1.png")   # baseline, no red
_I.new("RGB", (8, 8), (200, 200, 200)).save(f"{fdir}/frame_2.png")
redim = _I.new("RGB", (8, 8), (200, 200, 200))
for x in range(3, 6):
    for y in range(3, 6):
        redim.putpixel((x, y), (255, 0, 0))
redim.save(f"{fdir}/frame_3.png")                                    # edges on -> red
evs2 = [
    {"source": "VK-TRACE", "text": "View3DInventorViewer::applyVulkanSettings edges=0 points=0"},
    {"source": "VK-TRACE", "text": "View3DInventorViewer::applyVulkanSettings edges=1 points=1"},
    {"source": "HARNESS", "kind": "frame_phase", "fields": {"phase": "edges"}},
]
check("check_preferences PASS", fp.check_preferences(evs2, fdir, min_px=3) == [])
# baseline frame exists (frame_1 has no red) and edges frame has red -> pass
check("check_preferences baseline present", "no frame with 0 edge pixels" not in fp.check_preferences(evs2, fdir))
# a run where applyVulkanSettings never saw edges=1 -> error
errs = fp.check_preferences([{"source": "VK-TRACE", "text": "applyVulkanSettings edges=0 points=0"}], fdir)
check("check_preferences missing edges=1", any("edges=1" in e for e in errs))
# a run targeting a color that is never rendered -> error
novis = fp.check_preferences(evs2, fdir, min_px=3, edge_rgb=(0, 255, 0))
check("check_preferences detects missing render",
      any("no frame renders edges" in e for e in novis))

# --- pre-flight lint: static scan (no FreeCAD required) ---------------------
_bad = "/tmp/opencode/_lint_bad.py"
with open(_bad, "w") as fl:
    fl.write("import FreeCAD\nx = 1\nprint(y + z)\n")
st = fp.static_check(_bad)
check("lint static undefined name ERROR",
      any(lv == "ERROR" and "undefined name" in m for lv, _, m in st))

_bad2 = "/tmp/opencode/_lint_unused.py"
with open(_bad2, "w") as fl:
    fl.write("import time\ndef f():\n    return 1\n")
st2 = fp.static_check(_bad2)
check("lint static unused import WARN",
      any(lv == "WARN" and "never used" in m for lv, _, m in st2))

_clean = "/tmp/opencode/_lint_clean.py"
with open(_clean, "w") as fl:
    fl.write("import os\nimport sys\nx = os.path.join('a')\nprint(sys.maxsize)\n")
check("lint static clean has no findings", fp.static_check(_clean) == [])

# --- smoke: skipped for scripts that import no FreeCAD/PySide/pivy ---------
check("lint smoke skipped for non-FreeCAD script", fp.import_smoke(_clean) == [])

# lint_script with smoke disabled is the static result alone
check("lint_script smoke=off equals static",
      fp.lint_script(_clean, smoke=False) == fp.static_check(_clean))

# a syntax error surfaces as a single ERROR offline
_syn = "/tmp/opencode/_lint_syn.py"
with open(_syn, "w") as fl:
    fl.write("def ok(:\n    pass\n")
sst = fp.static_check(_syn)
check("lint static syntax ERROR",
      any(lv == "ERROR" and "syntax error" in m for lv, _, m in sst))

# --- interactive-command dialog watchdog (host-visible helpers) -------------
check("user-input command set includes Std_Open",
      "Std_Open" in fp._USER_INPUT_COMMANDS)
check("dialog timeout default is sane",
      1000 <= fp._cmd_dialog_timeout_ms() <= 60000)

print("=== RESULT:", "PASS" if PASS else "FAIL", "===")
sys.exit(0 if PASS else 1)
