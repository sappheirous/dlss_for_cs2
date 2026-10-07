# Third-party notices

Library dependencies are installed by vcpkg, not vendored in the source tree. Their complete license texts
are available in `build/msvc-x64/vcpkg_installed/x64-windows-static/share/<package>/copyright`.
When distributing a compiled DLL, include those copyright/license texts for the statically linked
libraries; this repository's MIT license does not replace them.

| Component | Upstream | License |
| --- | --- | --- |
| Dear ImGui | https://github.com/ocornut/imgui | MIT |
| MinHook | https://github.com/TsudaKageyu/minhook | BSD-2-Clause |
| spdlog | https://github.com/gabime/spdlog | MIT |
| fmt (spdlog dependency) | https://github.com/fmtlib/fmt | MIT |
| SimpleIni | https://github.com/brofield/simpleini | MIT |

The Windows SDK/compiler are build prerequisites. NVIDIA `nvngx_dlss.dll`, NVIDIA models, and
Valve game binaries are not distributed or relicensed by this project. NGX is dynamically resolved
from the game's renderer; NVIDIA SDK sources and static libraries are not included.

## AMD FidelityFX RCAS

`shaders/rcas.hlsl` adapts the non-packed FP32 RCAS filter from
[AMD FidelityFX FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR/blob/master/ffx-fsr/ffx_fsr1.h).
It uses native HLSL arithmetic, a normalized strength multiplier, safe denominators for flat black/white
regions, clamped border loads and pass-through alpha. The optional denoise path is omitted.
Include the following notice when distributing source or compiled shaders/DLLs containing this code.

Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial
portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
