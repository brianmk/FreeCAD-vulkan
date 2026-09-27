# Vulkan validation layer profiles

The renderer enables `VK_LAYER_KHRONOS_validation` only when `FC_VULKAN_VALIDATION`
is set (see `src/Gui/Quarter/QuarterVulkanWidget.cpp`).  Which checks the layer
runs is configured here, through the loader's `VK_LAYER_SETTINGS_PATH` mechanism
(Qt's `QVulkanInstance` exposes no `pNext` chain, so a settings file is the only
practical hook).

| Profile            | Adds                                                        | Use for |
| ------------------ | ----------------------------------------------------------- | ------- |
| `default`          | core                                                        | general correctness |
| `sync`             | synchronization validation                                  | barriers, pre-pass, async compute |
| `gpu-assisted`     | GPU-assisted validation                                     | device-side bounds/descriptor bugs |
| `best-practices`   | best-practices (incl. `perf` advisories)                    | API hygiene |
| `ci`               | sync + best-practices + validation cache                    | the CI VUID ratchet |

## Selecting a profile

Directly:

```sh
export FC_VULKAN_VALIDATION=1
export VK_LAYER_SETTINGS_PATH=$PWD/tools/rendering/vk_layer_settings/sync.txt
./build/debug/bin/FreeCAD ...
```

Through fcprobe (sets `VK_LAYER_SETTINGS_PATH` and implies `--validation`):

```sh
python3 ../FreeCAD-DevTools/fcprobe/freecad_probe.py run ../FreeCAD-DevTools/fcprobe/vk_live_probe.py \
    --validation-profile sync
```

`--validation-profile <name>` selects one of the profiles above: the harness
points `VK_LAYER_SETTINGS_PATH` at the matching file here and turns on
`FC_VULKAN_VALIDATION`.  An explicit `-e VK_LAYER_SETTINGS_PATH=...` override
wins over the profile.

## Key names

The keys are the layer's `khronos_validation.*` settings (for example
`validate_core`, `validate_sync`, `report_flags`).  The older `enables` /
`VK_VALIDATION_FEATURE_ENABLE_*` form is not used.
