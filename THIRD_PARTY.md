# Third-party components

| Component | Where | License |
|---|---|---|
| [shadPS4](https://github.com/shadps4-emu/shadPS4): shader recompiler, commit `eeecd10` | built in `tools/shader_compiler_tool` (prebuilt copy in `tools/shader_compiler_tool/prebuilt`) | GPL-2.0-or-later |
| shadPS4: NID names (`aerolib`) | `tools/data/aerolib.csv` | GPL-2.0-or-later |
| shadPS4 v0.7.0: PKG extraction (`pkg.cpp`, `pfs.h`, `pkg_type`, `keys.h`, the crypto functions it uses, rewritten on Windows CNG) | `tools/pkg_extractor` (`build/pkg_extractor.exe`) | GPL-2.0-or-later |
| [miniz](https://github.com/richgel999/miniz): inflate (`tinfl`) only | `tools/pkg_extractor/miniz` | MIT |
| [Zydis](https://github.com/zyantific/zydis) and Zycore | `externals/zydis` | MIT |
| [SDL 3](https://github.com/libsdl-org/SDL) | `externals/sdl3` (static library) | zlib |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | `externals/vulkan-headers` | Apache-2.0 / MIT |
| Python (portable runtime, only in the distribution package) | `tools/python` in the installer | PSF License |
| [Anomaly](https://github.com/connorh315/anomaly) v1.1 by connorh315: LEGO Dimensions PS4 mod loader (hooks, layered text.csv / collection.txt), reimplemented inside the port | `src/hle/overrides/hle_anomaly.cpp` | GPL-2.0-or-later |
| [7-Zip](https://www.7-zip.org) 26.03 (`7z.exe` + `7z.dll`: installs .rar / .7z mods) | `tools/7zip` | GNU LGPL, with the unRAR restriction on part of `7z.dll`, and BSD |
| LegoToypad 1.9.3 (portal emulator app, bundled as-is; not covered by this project's license) | `tools/LegoToypad.exe` | its authors' terms |
| [inFAMOUS Second Son PC port](https://github.com/Castlesea) by Castlesea: launcher layout (one painted screen, menu with the PLAY bar, sliding selection bar, settings tabs with `< value >` rows, key hints, keyboard / controller navigation), adapted | `tools/dist/launcher.cpp` | GPL-2.0-or-later |
| Microsoft Visual C++ Redistributable (DLLs in the package) | `build/` in the installer | Microsoft redistribution license |

Each license text ships with the component in its folder. This project's license (GPL-2.0) is in [LICENSE](LICENSE).

Portuguese version: [TERCEIROS.md](TERCEIROS.md).
