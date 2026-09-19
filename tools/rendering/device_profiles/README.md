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
python3 tools/fcprobe/freecad_probe.py run tools/fcprobe/vk_live_probe.py \
    --profile vulkan --device-profile minimum-1-2

# Parity matrix across several simulated devices:
python3 tools/fcprobe/freecad_probe.py matrix tools/fcprobe/vk_live_probe.py \
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
