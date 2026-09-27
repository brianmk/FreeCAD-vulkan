# Device profiles (VK_LAYER_KHRONOS_profiles)

These JSON files configure the `VK_LAYER_KHRONOS_profiles` layer so a probe can
run against a *simulated* device instead of the real GPU.  The layer either
restricts what the instance reports from a Vulkan Profiles document
(`profile_file`/`profile_name`) or filters the real device's capabilities via
its own environment knobs (`env`), so the renderer's fallback paths are
exercised without owning the hardware.

The layer must be installed; profile documents come from the `vulkan-profiles`
package.  This is the fallback for the unpackaged
`VK_LAYER_LUNARG_device_simulation` layer.

## Schema

Each `<name>.json` has:

| Key            | Required | Meaning |
| -------------- | -------- | ------- |
| `description`  | no       | Free-form note describing what the profile simulates. |
| `profile_file` | no       | Basename of a Vulkan Profiles JSON (e.g. `VP_KHR_roadmap.json`).  Resolved from the current directory, the distro/install profile dirs (`/usr/share/vulkan/config/VK_LAYER_KHRONOS_profiles`, `/usr/local/...`, `~/.local/...`), then this directory.  Omit for an env-only filter. |
| `profile_name` | no       | Profile to select inside `profile_file` (e.g. `VP_KHR_roadmap_2022`). |
| `env`          | no       | Extra layer environment (via `setdefault`, so an explicit `-e K=V` still wins).  Common keys: `VK_KHRONOS_PROFILES_EXCLUDE_DEVICE_EXTENSIONS`, `VK_KHRONOS_PROFILES_EMULATE_PORTABILITY`. |

`apply_device_profile()` also sets `VK_KHRONOS_PROFILES_DEBUG_REPORTS` /
`_DEBUG_ACTIONS` / `_DEBUG_FILENAME` so the layer's findings land in the run
stdout and `/tmp/opencode/profiles_layer.log`, and appends
`VK_LAYER_KHRONOS_profiles` to `VK_INSTANCE_LAYERS`.

## Usage

```sh
# Single run on a simulated device:
python3 ../FreeCAD-DevTools/fcprobe/freecad_probe.py run ../FreeCAD-DevTools/fcprobe/vk_live_probe.py \
    --profile vulkan --device-profile minimum-1-2

# Parity matrix across several simulated devices:
python3 ../FreeCAD-DevTools/fcprobe/freecad_probe.py matrix ../FreeCAD-DevTools/fcprobe/vk_live_probe.py \
    --device-profile minimum-1-2 --device-profile desktop-baseline-2022
```

## Shipped profiles

| Name                     | Simulates |
| ------------------------ | --------- |
| `minimum-1-2`            | `VP_LUNARG_minimum_requirements_1_2`: core-only device; every optional path must fall back |
| `desktop-baseline-2022`  | `VP_LUNARG_desktop_baseline_2022`: conservative desktop baseline |
| `roadmap-2022`           | `VP_KHR_roadmap_2022`: widest common denominator (desktop + mobile) |
| `no-descriptor-indexing` | real device minus `VK_EXT_descriptor_indexing` / `VK_EXT_descriptor_buffer` |
| `no-ray-tracing`         | real device minus the ray-tracing extensions |
| `portability`            | real device with portability-subset emulation forced on |
| `rtx-2060`               | Strict Turing RTX 20-series sim: ray tracing present, but position-fetch and the Ada/Blackwell optional extensions (`VK_KHR_ray_tracing_position_fetch`, `VK_EXT_opacity_micromap`, NV cluster/partitioned AS, linear swept spheres) hidden, plus a constrained `maxMemoryAllocationCount` from `VP_NVIDIA_rtx_2060.json` |

## Simulating a lower-tier RTX GPU

`rtx-2060` hides position-fetch and the post-Turing optional extensions while
leaving ray tracing advertised, and applies `VP_NVIDIA_rtx_2060.json`
(`SIMULATE_PROPERTIES_BIT`) to constrain device limits, so the RTX backend must
still initialize and path-trace with the five caps reported 0.  The
`rtx-2060` / `rtx-2060-control` cases in `vk_suite.json` run the same probe with
and without the profile and assert the caps flip (the control proves the
profile, not the machine, is what zeroes them).

> **What a device profile can and cannot simulate.** The Khronos profiles layer
> can override device limits, extensions, features and formats. It **cannot**
> override `deviceName` / `deviceID` / `deviceType` (`deviceType` is reported
> "not modifiable") or memory heaps — the Vulkan Profiles schema has no
> `deviceName` and no `VkPhysicalDeviceMemoryProperties`. So a "2060" profile
> simulates the capability/limit set, not the 6 GB VRAM or the device string.

```sh
# Simulated RTX 2060 (four caps must read 0, RT still works)
python3 ../FreeCAD-DevTools/fcprobe/freecad_probe.py run ../FreeCAD-DevTools/fcprobe/vk_rtx2060_probe.py \
    --profile vulkan --device-profile rtx-2060 --env FC_VULKAN_RT_DEBUG=1

# Unsimulated control (the real device advertises all four)
python3 ../FreeCAD-DevTools/fcprobe/freecad_probe.py run ../FreeCAD-DevTools/fcprobe/vk_rtx2060_probe.py \
    --profile vulkan --env FC_RTX2060_EXPECT=present --env FC_VULKAN_RT_DEBUG=1
```
