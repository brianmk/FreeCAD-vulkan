#!/usr/bin/env python3
"""Host-side assertions for vk_diag_probe.py (Phase 1 diagnostics tooling).

Checks performed on the run bundle:
  1. SoVulkanConfig::dump() emitted the resolved diagnostics block and the
     GPU-timing / pipeline-feedback flags are on (the probe must be launched
     with FC_VULKAN_BACKEND_DEBUG, FC_VULKAN_GPU_TIMING and
     FC_VULKAN_PIPELINE_FEEDBACK).
  2. SoVulkanGpuTimers emitted at least one "[RTDBG] gpuTiming <scope>=<ms>"
     line (the external render path is instrumented).  Informational only:
     GPU timestamps are own-queue only, so the GUI's external path emits none.
  3. VK_EXT_pipeline_creation_feedback emitted a "[RTDBG] pipelineFeedback ..."
     line.  Informational only: the extension is device-dependent (an
     RT-capable device that exposes it and a created pipeline), so its absence
     is not a failure.
  4. No Vulkan validation VUID diagnostics were produced.

Run:
  FC_VULKAN_BACKEND_DEBUG=1 FC_VULKAN_DEBUG_UTILS=1 \\
  FC_VULKAN_GPU_TIMING=1 FC_VULKAN_PIPELINE_FEEDBACK=1 \\
      python3 tools/fcprobe/freecad_probe.py run tools/fcprobe/vk_diag_probe.py \\
      --profile vulkan --validation-profile sync --name diag
"""

import re
import sys

CONFIG = re.compile(r"\[VKCONFIG\] diagnostics debugUtils=(\d+)\s+debugPrintf=(\d+)"
                    r"\s+gpuTiming=(\d+)\s+pipelineFeedback=(\d+)")
GPUTIMING = re.compile(r"\[RTDBG\] gpuTiming (\S+)=([\d.]+)ms")
PIPEFB = re.compile(r"\[RTDBG\] pipelineFeedback (\S+) cacheHit=(\d+) "
                    r"creation=([\d.]+)us")
VUID = re.compile(r"VUID-")


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    config = None
    gpu_scopes = []
    feedback = []
    vuids = []
    for ln in lines:
        m = CONFIG.search(ln)
        if m and config is None:
            config = tuple(int(m.group(i)) for i in range(1, 5))
        m = GPUTIMING.search(ln)
        if m:
            gpu_scopes.append(m.group(1))
        m = PIPEFB.search(ln)
        if m:
            feedback.append((m.group(1), int(m.group(2)), float(m.group(3))))
        if VUID.search(ln):
            vuids.append(ln.strip())

    if config is None:
        err("no [VKCONFIG] diagnostics line: set FC_VULKAN_BACKEND_DEBUG=1 so "
            "SoVulkanConfig::dump() runs")
    else:
        debug_utils, debug_printf, gpu_timing, pipeline_feedback = config
        sys.stderr.write(
            f"[CHECK] config debugUtils={debug_utils} debugPrintf={debug_printf} "
            f"gpuTiming={gpu_timing} pipelineFeedback={pipeline_feedback}\n")
        if not gpu_timing:
            err("FC_VULKAN_GPU_TIMING was not resolved on (gpuTiming=0)")
        if not pipeline_feedback:
            err("FC_VULKAN_PIPELINE_FEEDBACK was not resolved on "
                "(pipelineFeedback=0)")

    if not gpu_scopes:
        # Informational: GPU timestamps are only recorded on the own-queue
        # render() path.  The GUI renders through renderExternal(), whose
        # caller-owned command buffer is already inside a render pass, where
        # vkCmdResetQueryPool (required before vkCmdWriteTimestamp) is illegal.
        sys.stderr.write(
            "[CHECK] note: no [RTDBG] gpuTiming lines (GUI uses the external "
            "path; GPU timers are own-queue only)\n")
    else:
        sys.stderr.write(f"[CHECK] gpuTiming scopes={sorted(set(gpu_scopes))}\n")

    if not feedback:
        # Informational, not a failure: VK_EXT_pipeline_creation_feedback is
        # device-dependent.  It is only enabled on an RT-capable device that
        # exposes the extension and only logs when a pipeline is actually
        # created, so a non-RT device (or a frame that creates no pipelines)
        # legitimately emits no lines.
        sys.stderr.write(
            "[CHECK] note: no [RTDBG] pipelineFeedback lines (VK_EXT_pipeline_"
            "creation_feedback is device-dependent: needs an RT-capable device "
            "that exposes the extension and a created pipeline)\n")
    else:
        labels = sorted({f[0] for f in feedback})
        sys.stderr.write(f"[CHECK] pipelineFeedback labels={labels}\n")

    if vuids:
        err("Vulkan validation produced %d VUID diagnostic(s), first: %s"
            % (len(vuids), vuids[0][:200]))
