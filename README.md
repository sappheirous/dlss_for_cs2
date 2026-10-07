# dlss_for_cs2

<p align="center">
  <strong>DLAA and DLSS Super Resolution for Counter-Strike 2</strong><br>
  Windows x64 · DirectX 11
</p>

> **WARNING — USE AT YOUR OWN RISK**
>
> This project is for educational purposes only and is not affiliated with Valve or NVIDIA.

## Features

- **Four render modes**: DLAA, Quality, Balanced and Performance.
- **Render preset selection** with hover descriptions in the in-game menu.
- **Optional sharpening** after DLSS, with a strength control from 0 to 1.
- **In-game menu and live stats** for GPU upscale time, frame time and FPS. Press **Insert** to open or close it.
- **Automatic settings save** and restoration of the game's previous MSAA and Ambient Occlusion settings.

## Build from source

You need Windows x64, the MSVC x64 C++ tools and Windows SDK, CMake 3.24 or newer, Ninja, Git and a bootstrapped [vcpkg](https://github.com/microsoft/vcpkg) checkout. CS2 and the NVIDIA SDK are not needed to build.

Open PowerShell in the project folder and run:

```powershell
$env:VCPKG_ROOT = 'C:\path\to\vcpkg'
.\scripts\build.ps1 -Configuration release
```

For a Debug build, use:

```powershell
.\scripts\build.ps1 -Configuration debug
```

The DLL is written to `out/release/cs2_dlss.dll` or `out/debug/cs2_dlss.dll`.

## Run in game

1. Rename the built `cs2_dlss.dll` to `d3d11.dll` and place it beside `cs2.exe`.
2. Launch CS2 in DirectX 11 mode with `-insecure -dlss` in the launch options.
3. Make a compatible `nvngx_dlss.dll` available beside `cs2.exe`. NVIDIA binaries are not included.
4. Press **Insert** to open the menu and choose your settings.

## In-game settings

| Setting | Choices | Details |
| --- | --- | --- |
| Quality mode | DLAA, Quality, Balanced, Performance | DLAA renders at 100%; the other modes use 66.67%, 58% and 50% of output resolution per axis. |
| Render preset | Default, Latest, A–F, J–M | Requests a DLSS model preset. Runtime support varies. |
| Sharpness | 0 to 1 | Optional RCAS sharpening after DLSS. At 0 the sharpening pass is skipped. |

DLSS turns off MSAA and Ambient Occlusion while active, then restores their previous settings when disabled or when DLSS cannot continue. Turn DLSS off before changing the game's FSR settings.

Settings and logs are stored beside `d3d11.dll` as `settings.ini` and `cs2_dlss.log`.

Defaults are DLSS disabled, Quality mode, Default preset and Sharpness 0.

## Compatibility and troubleshooting

- **Windows x64 and DirectX 11 only.** Vulkan and frame generation are not supported.
- **A compatible NVIDIA GPU and DLSS runtime are required.** Preset availability depends on that runtime.
- **Moving objects may show reconstruction artifacts.** Motion vectors currently come from scene depth and camera movement; moving objects do not have their own vectors.
- **Game updates may break the integration.** If the menu reports an error, check the runtime log at the path above.

| If… | Check… |
| --- | --- |
| The menu does not appear | The proxy is named `d3d11.dll` beside `cs2.exe`, and CS2 is running in DirectX 11 with `-insecure -dlss`. |
| DLSS is unavailable | A compatible `nvngx_dlss.dll` is beside `cs2.exe`. |
| A preset looks different than expected | The runtime or driver may have selected a fallback. Check the feature log for the requested preset. |
| Another mod uses `d3d11.dll` | The current proxy cannot chain an existing D3D11 proxy. |

## Credits

Thanks to [rushensky](https://github.com/rushensky2h) for creating the original prototype that inspired this project. The codebase in this repository was rebuilt from scratch.

## License

Project code is [MIT licensed](LICENSE). Game and NVIDIA binaries are not included and retain their own licenses. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for dependency notices.
