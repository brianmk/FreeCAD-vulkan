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
python3 tools/fcprobe/freecad_probe.py run tools/fcprobe/vk_live_probe.py \
    --validation-profile sync
```

`FC_VULKAN_VALIDATION_PROFILE` is accepted by the harness for symmetry; when
both are present, an explicit `VK_LAYER_SETTINGS_PATH` / `-e` override wins.

## Key names

The keys track the installed validation layer's settings schema.  If the layer
rejects a key it prints a message naming it; confirm the current spelling with
`vkconfig` or the layer's shipped `vk_layer_settings.txt`.  The `enables` list
uses the stable `VK_VALIDATION_FEATURE_ENABLE_*` enum names.
