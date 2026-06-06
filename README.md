# OBS Graphics Studio Pro — Native OBS Plugin by OmniaTV

OBS Graphics Studio Pro is developed by OmniaTV. It is a fully native C++/Qt OBS plugin providing After Effects–style title creation,
management, and real-time compositing.

---

## Architecture

```
OBS-Graphics-Studio-Pro/
├── CMakeLists.txt
├── data/
│   └── locale/
│       └── en-US.ini
└── src/
    ├── plugin-main.h / .cpp   ← OBS module entry point
    ├── title-data.h  / .cpp   ← Data model (Title, Layer, Keyframe) + JSON persistence
    ├── title-source.h / .cpp  ← OBS source: GPU renderer via libobs gs_*
    ├── title-dock.h  / .cpp   ← OBS Dock: title list panel
    └── title-editor.h / .cpp  ← After Effects–style editor (Canvas + LayerStack + Timeline + Properties)
```

### Component Map

| Component | OBS Integration | Purpose |
|---|---|---|
| `TitleSource` | `obs_source_type INPUT` | Renders a title to the OBS video mix through an OBS-native GPU pipeline using libobs `gs_*` passes |
| `GpuTextureFrame` / `ObsGpuRenderPipeline` | libobs `gs_*` graphics | Owns GPU asset textures, text/shape style atlases, draw passes, effects, transforms, and per-layer GPU migration plans |
| `TitleDock` | `obs_frontend_add_dock()` | Floating/dockable title list with blank-title creation, Graphics Studio-style templates, and scene-add button |
| `TitleEditor` | `QDialog` (non-modal) | Full AE-style editor with canvas, layer stack, timeline, properties |
| `TitleDataStore` | Singleton | Owns all `Title` objects; serialises to `obs-graphics-studio-pro/titles.json` |

### GPU Rendering Transition

A structured rendering audit and migration roadmap is maintained in [`docs/rendering-gpu-transition.md`](docs/rendering-gpu-transition.md). The live OBS source path now routes drawing through an OBS-compatible GPU pipeline abstraction; legacy CPU 2-D raster composition is not used for source rendering.

---

## Dependencies

| Library | Purpose |
|---|---|
| **OBS Studio** (libobs + obs-frontend-api) | Plugin API, graphics, frontend dock |
| **Qt 5.15+ or Qt 6** | UI widgets and image/SVG asset ingestion |
| **nlohmann/json** | JSON serialisation (fetched automatically by CMake) |

---

## Build Instructions

### Linux (Ubuntu 22.04+)

```bash
# 1. Install dependencies
sudo apt install \
  cmake ninja-build \
  libobs-dev obs-frontend-api-dev \
  qtbase5-dev libqt5widgets5

# 2. Configure
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo

# 3. Build
cmake --build build

# 4. Install to OBS' per-user plugin folder
cmake --install build --prefix ~/.config/obs-studio/plugins
# or copy/symlink the staged build tree:
mkdir -p ~/.config/obs-studio/plugins
cp -R build/obs-graphics-studio-pro ~/.config/obs-studio/plugins/
```

### macOS

```bash
brew install cmake pkg-config

cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DOBS_SOURCE_DIR=/path/to/obs-studio

cmake --build build
```

The standalone build stages a directly copyable plugin folder at
`build/obs-graphics-studio-pro`, with the binary under `bin/<arch>` and locale files under
`data/locale`.

### Windows (Visual Studio / vcpkg)

Install Qt with vcpkg, then point the build at either an OBS
plugin dependencies package or an OBS Studio install tree with `OBS_SDK_DIR` (or
`-DOBS_SDK_DIR=...`). The helper script also accepts `-ObsSdkDir` and honours
`VCPKG_ROOT`, `OBS_SDK_DIR`, `OBS_STUDIO_DIR`, and `OBS_PLUGINS_PATH`. By
default, the helper installs to OBS' recommended per-machine plugin root,
`C:\ProgramData\obs-studio\plugins`.

```bat
vcpkg install qt6-base

set OBS_SDK_DIR=C:\path\to\plugin-deps-or-obs-studio
cmake -B build -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
  -DOBS_SDK_DIR=%OBS_SDK_DIR%

cmake --build build --config Release
```

Or run the convenience script:

```powershell
.\build-windows.ps1 -ObsSdkDir C:\path\to\plugin-deps-or-obs-studio
```

After install, OBS should see this structure:

```text
C:\ProgramData\obs-studio\plugins\obs-graphics-studio-pro\
├── bin\64bit\obs-graphics-studio-pro.dll
├── bin\64bit\Qt6Core.dll / Qt5Core.dll
├── bin\64bit\Qt6Gui.dll / Qt5Gui.dll
├── bin\64bit\Qt6Widgets.dll / Qt5Widgets.dll
└── data\locale\en-US.ini
```

Use `-InstallRoot` if you need a portable OBS/custom plugin root instead. If
OBS reports that `obs-graphics-studio-pro` failed to load, first verify that the dependency
DLLs above are beside `obs-graphics-studio-pro.dll`; a successful compile is not enough for
Windows to load the plugin at OBS startup.

---

## OBS Graphics Studio Pro Workflow

OBS Graphics Studio Pro by OmniaTV is designed around a Graphics Studio-style flow:

1. Open the **OBS Graphics Studio Pro** dock.
2. Click **Templates** and choose **Lower Third**, **Centered Title**, or **Ticker / Strap**.
3. Enter the starter text; the editor opens with editable text and shape layers.
4. Adjust text/position/style in the editor. Changes auto-save and update the title store.
5. Click **▶ Scene** in the dock to add the selected title source to the active OBS scene.

---

## Data Format

Titles are saved in the OBS profile config directory:

```
%APPDATA%\obs-studio\plugin_config\obs-graphics-studio-pro\titles.json   (Windows)
~/.config/obs-studio/plugin_config/obs-graphics-studio-pro/titles.json   (Linux)
~/Library/Application Support/obs-studio/plugin_config/obs-graphics-studio-pro/titles.json (macOS)
```

### Title JSON Schema (abbreviated)

```json
[
  {
    "id": "uuid",
    "name": "My Lower Third",
    "duration": 5.0,
    "bg_color": 0,
    "width": 1920,
    "height": 1080,
    "layers": [
      {
        "id": "uuid",
        "name": "Title Text",
        "type": 0,
        "visible": true,
        "in_time": 0.0,
        "out_time": 5.0,
        "pos_x": { "static_value": 960.0, "keyframes": [] },
        "pos_y": { "static_value": 900.0, "keyframes": [] },
        "opacity": {
          "static_value": 1.0,
          "keyframes": [
            { "time": 0.0, "value": 0.0, "easing": 2 },
            { "time": 0.5, "value": 1.0, "easing": 2 }
          ]
        },
        "text_content": "Breaking News",
        "font_family": "Helvetica Neue",
        "font_size": 72,
        "font_bold": true,
        "text_color": 4294967295
      }
    ]
  }
]
```

### Easing values

| Value | Easing |
|---|---|
| 0 | Linear |
| 1 | Ease In |
| 2 | Ease Out |
| 3 | Ease In/Out |
| 4 | Cubic Bezier |
| 5 | Hold (jump cut) |

---

## Extending the Plugin

### Adding a new layer type

1. Add a value to `enum class LayerType` in `title-data.h`
2. Add a GPU migration plan/pass in `title-renderer-gpu.*` and keep the live OBS source path free of CPU 2-D raster backends
3. Add OBS/libobs GPU paint logic in `title-editor.cpp → CanvasPreview::render_to_pixmap()` (CPU-only fallbacks are reserved for unavailable OBS graphics contexts)
4. Add UI controls in `PropertiesPanel`
5. Add JSON serialisation in `layer_to_json()` / `layer_from_json()`

### Adding keyframe support for a property in the editor

The `TimelineWidget` already draws keyframe diamonds for all `AnimatedProperty`
fields. To allow adding keyframes from the Properties panel, add a "◆ Add KF"
button next to the property spinbox that calls:

```cpp
Keyframe kf;
kf.time  = playhead_;
kf.value = spn_px_->value();
kf.easing = EasingType::EaseInOut;
layer_->pos_x.keyframes.push_back(kf);
std::sort(layer_->pos_x.keyframes.begin(), layer_->pos_x.keyframes.end(),
          [](auto &a, auto &b){ return a.time < b.time; });
emit property_changed();
```

---

## Roadmap / TODO

- [ ] Image layer type with file picker
- [ ] Shape (ellipse, polygon) layer type
- [ ] Gradient fills
- [ ] Keyframe editor: drag to move keyframes on timeline
- [ ] Keyframe editor: right-click → set easing
- [ ] Bezier curve editor overlay (velocity graph)
- [ ] Template system: save/load title presets
- [ ] Playlist mode: auto-advance through titles
- [x] OBS-native GPU rendering path foundation (GS effects, layer texture assets, GPU compositing/transforms)
- [ ] Live preview in dock (thumbnail strip)
- [ ] Undo/redo stack (Qt QUndoStack)
- [ ] Multi-select layers
- [ ] Text drop-shadow property
- [ ] Export title as PNG sequence
