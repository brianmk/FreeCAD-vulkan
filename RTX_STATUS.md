# FreeCAD RTX Path-Tracing — Status & Next Steps

**Last updated:** 2026-08-30
**Scope:** Vulkan RTX path-tracing backend in the coin submodule (`SoRTXRenderBackend`), RTX/NVIDIA track.

---

## 1. Where we are

We are mid-Phase 1. **Phase 0 is committed and verified.** The DLSS-RR
go/no-go runtime probe is complete and its verdict is recorded (see §5) —
it is **CONDITIONAL GO**, which means we proceed with implementing the
backend but the final activation is gated on a registered NVIDIA App ID.

### Committed so far

- **Superproject** `f9adc358f4` — `Gui: RTX Phase 0 - probe/request optional RT capability extensions`
  - capability-bool self-probe,
  - optional-extension request / feature chaining in `QuarterVulkanWidget.cpp`.
- **coin submodule** `4faeb6541` — `rendering: RTX Phase 0 - capability probe,
  HDR accumulation, denoiser-ready G-buffer`
  - HDR unclamped accumulation in `PathTrace.glsl`,
  - denoiser-ready G-buffer plumbing.
  (> Later coin commits `5ae53835d`, `93cb2f5bd` are prior RTX work.)

The superproject shows `src/3rdParty/coin` as a dirty submodule — the
**submodule pointer is already bumped** to the Phase-0 commit; the `?` is
expected until the parent integrates it.

### Confirmed working (smoke test)

`/tmp/opencode/vk_quick_geo_probe.py` — floor Box + Cylinder + Sphere,
`VulkanPathTracing` toggled on, **PASS**; 134 accumulating pt-state frames,
no crash. Offscreen composite proof: `/tmp/vk_frame_134.png`.

---

## 2. Present blocker: DLSS-RR `CreateFeature1` returns `FAIL_NotInitialized`

### Facts (probe `<REPO>/tmp/opencode/dlxprobe/go_nogo.cpp`)

All technical plumbing is **green**:

- `libnvidia-ngx.so.1` dlopens; Vulkan symbols resolve.
- `NVSDK_NGX_VULKAN_Init` (FeatureCommonInfo scan-path variant) returns
  Success (0x1); Blackwell detected (minArch=352).
- `GetFeatureRequirements(RayReconstruction)` = Supported.
- Feature module loads under the **exact filename `libnvidia-ngx-dlssd.so`**
  (symlink to `dev/libnvidia-ngx-dlssd.so.310.7.0`); metadata validates
  (core 0x15 ≥ req 0x13; driver ≥ 535.101).
- Parameter API must use the **C++ vtable** (`params->Set(...)`); the
  free-function setters are **not exported** on Linux.
- `vkBeginCommandBuffer` requires a non-NULL begin-info.

The one failing call, on **every** variant tried:

```
NVSDK_NGX_VULKAN_CreateFeature1(RayReconstruction) -> 0xBAD00007 FAIL_NotInitialized
```

Variants tried: Allocate/GetCapability params, `rel/` vs `dev/` snippet,
SDK ver 0x14/0x15, OTA updater on/off. Control experiment with the same
generic app id on the DLSS-SR snippet **crashes inside the driver** — the
generic-appid path is not supported.

### Root cause

The 610.57.04 driver carries the `REQUIRE_CMSID` flag. For our
**unregistered** application the driver substitutes the generic app id
`0xED9E64D`, which the feature-creation path does not accept
(`CreateFeature_Validate` passes, but feature init does not proceed).

### Environment quirks to remember in the integration

- Nondeterministic `FAIL_OutOfDate` (0xBAD0000C) on Init, correlated with
  `nvidia-ngx-updater` launches. With `__NGX_DISABLE_UPDATER=1`, Init is
  mostly Success; **retry-once on Init** covers it in real usage.
- NGX log goes to `./nvngx.log` in the CWD (`__NGX_LOG_LEVEL` controls it).
- User config schema exists (`ngx_models_path`, `use_staging_url`,
  `file_format_version`) but our guessed JSON was rejected as Malformed —
  do **not** ship a conf without the real schema.
- The NVIDIA Vulkan ICD is inside `libnvidia-glcore.so.610.57.04`.

---

## 3. Verdict & decision (see `/tmp/opencode/DLSS_RR_GO_NO_GO.txt`)

**CONDITIONAL GO.** Implement Phase 1 as designed, runtime-gated on a
registered App ID; without one the backend reports itself **unavailable**
and dispatch falls through to the existing denoisers. Obtain a real
App/CMS ID by registering FreeCAD with the NVIDIA DLSS developer program
(manual, outside this repo).

---

## 4. What is next

### Phase 1 (DLSS-RR backend): IMPLEMENTED, runtime-gated

The backend is written and compiles (Coin + FreeCADGui build clean, option ON
and OFF). It is a **self-authored ABI shim** (no NVIDIA header vendoring —
see §6 for the licensing correction) that `dlopen`s the proprietary runtime.

Delivered:

- **`ngx_abi.h`** (new, MIT) — self-authored interop declarations: result /
  feature enums, the POD structs we touch, the `NVSDK_NGX_Parameter` vtable
  interface, and the Vulkan entry typedefs. No NVIDIA headers.
- **`ngx_loader.h/.cpp`** (new, MIT) — `dlopen`s `libnvidia-ngx.so.1`,
  resolves the entry points, and provides `ngxSetParam`/`ngxGetParam` vtable
  helpers (the free-function setters are **not exported** on Linux).
- **`SoRTXRenderBackendDlssRR.cpp`** (new, MIT) — `createDlssRrBackend`
  (init w/ retry-once for `FAIL_OutOfDate`, requirements query, param +
  scratch + output allocation, `CreateFeature1`), `evaluateDlssRr`
  (device-local buffer binding + `EvaluateFeature`), `teardownDlssRrBackend`.
- **`SoRTXRenderBackend.h`** — added `DenoiseDlssRr` to `DenoiseKind`,
  plus the gated NGX backend members.
- **`SoRTXRenderBackendDenoise.cpp`** — wired create/evaluate/teardown
  dispatch (`FC_VULKAN_PT_DENOISER=dlssrr` + GUI combo index 4).
- **`View3DInventorViewer.cpp`** / **`DlgSettings3DView.ui`** — GUI combo
  item "DLSS-RR (NVIDIA NGX, GPU)" → `"dlssrr"`, and
  `setDenoiserFilter` now maps `"dlssrr"`.
- **CMakeLists**: `COIN_BUILD_DLSS_RR_DENOISER` option (default **OFF**),
  `COIN_BUILD_DLSS_RR_DENOISER_VALUE`, compile definition, and the two new
  sources in `src/rendering/CMakeLists.txt`.

**Runtime gates**: backend active only when (a) built with the option ON,
(b) Vulkan device is NVIDIA, (c) `libnvidia-ngx.so.1` is dlopen-able, and
(d) `FC_RTX_DLSS_APPID` is set. Any gate failure → the denoiser list falls
back to OIDN (verified). `FC_RTX_DLSS_MODULE_DIR` optionally points at the
feature .so directory (exact filename `libnvidia-ngx-dlssd.so`).

**Verified**: fcprobe run with `dlssrr` requested and no App ID →
`[DENOISE] DLSS-RR disabled: FC_RTX_DLSS_APPID not set` → degrades to OIDN
→ `[VERDICT] PASS`, exit 0, no crash. Default RTX path still passes.

### Priority 2 — Register App ID (external, blocks activation)

- Apply to the NVIDIA DLSS developer program for a FreeCAD App/CMS ID.
- Re-run the probe with the registered id to confirm
  `CreateFeature1(RR)` returns Success.

### Deferred

- **Phase 2 — position-fetch** (motion/position G-buffer) — next after
  Phase 1.
- **NRD integration** — **paused** by user decision in favor of DLSS-RR.
  Vendored NRD headers exist uncommitted at
  `src/3rdParty/coin/src/3rdparty/NRD/` (`NRD.h`, `NRDDescs.h`,
  `NRDSettings.h`). NRD is **not** MIT; it must be runtime `dlopen`ed.

---

## 6. Licensing note (changed from original plan)

The original plan ("MIT-vendored headers") is **wrong**. Every NVIDIA NGX
header (`nvsdk_ngx_defs.h`, `nvsdk_ngx_vk.h`, `nvsdk_ngx_params.h`, …) carries
`SPDX-License-Identifier: LicenseRef-NvidiaProprietary` ("any use, reproduction,
disclosure or distribution ... without an express license agreement from
NVIDIA is strictly prohibited"). So we must **not** vendor them into the LGPL
repo. Instead, the repo now ships a self-authored MIT **ABI shim**
(`ngx_abi.h`) that reproduces only the interface declarations (interop
function-pointer typedefs, POD enums/structs, the `NgxParameter` vtable
layout). The proprietary runtime is `dlopen`'d and never redistributed. This
is the licensing-clean path chosen by the user.

---

## 5. Reference materials
- Probe source: `/tmp/opencode/dlxprobe/go_nogo.cpp`
  (vendored headers under `dlxprobe/include/`).
- Probe verdict: `/tmp/opencode/DLSS_RR_GO_NO_GO.txt`.
- SDK clone: `/tmp/opencode/DLSS/`
  (`lib/Linux_x86_64/{dev,rel}/libnvidia-ngx-dlssd.so.310.7.0`).
- Docs: `/tmp/opencode/dlss_rr_guide.txt`, `/tmp/opencode/dlss_pg.txt`.
- Backend enum: `src/3rdParty/coin/src/rendering/SoRTXRenderBackend.h`
  (`DenoiseKind` @ ~713).
- Dispatch points:
  `src/3rdParty/coin/src/rendering/SoRTXRenderBackend/SoRTXRenderBackendDenoise.cpp`.
- Frame dumps: `/tmp/vk_frame_<ordinal>.png` via
  `src/Gui/Quarter/VulkanFrameDumper.h`.
- GPU: RTX 5090 (Blackwell), driver 610.57.04, Vulkan 1.4.357; secondary
  AMD RADV iGPU.
