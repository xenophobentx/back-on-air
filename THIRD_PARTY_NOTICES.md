# Back on Air: third-party notices

| Component | Licence | Copyright | Used for |
|---|---|---|---|
| [SMAA](https://github.com/iryoku/smaa) reference implementation (`SMAA.hlsl`, `AreaTex.h`, `SearchTex.h`), commit 71c806a, unmodified in `third_party/smaa/` | MIT, text below and in `third_party/smaa/LICENSE.txt` | 2013 Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez | `aa = smaa` (`src/postfx.cpp`) |
| [DXVK](https://github.com/doitsujin/dxvk) 2.7.1, the 32-bit `d3d9.dll` of the release tarball, unmodified, in the player zip's `dxvk/` folder (with its licence) | zlib/libpng, text below and in `third_party/dxvk/LICENSE` | 2017 Philip Rebohle, 2019 Joshua Ashton, 2019 Robin Kertels, 2023 Jeffrey Ellison | `renderer = dxvk` on Windows (`src/renderer.cpp`) |
| FXAA 3.11 quality algorithm (the method only; the shader in `src/postfx.cpp` is written from the published description, no NVIDIA code) | - | the algorithm: Timothy Lottes, NVIDIA | `aa = fxaa` |
| AMD FidelityFX CAS (the method only; the shader in `src/postfx.cpp` is written from it, no AMD code) | MIT (the original) | the algorithm: 2019 Advanced Micro Devices, Inc. | `sharpen` |

## SMAA licence

```
Copyright (C) 2013 Jorge Jimenez (jorge@iryoku.com)
Copyright (C) 2013 Jose I. Echevarria (joseignacioechevarria@gmail.com)
Copyright (C) 2013 Belen Masia (bmasia@unizar.es)
Copyright (C) 2013 Fernando Navarro (fernandn@microsoft.com)
Copyright (C) 2013 Diego Gutierrez (diegog@unizar.es)

Permission is hereby granted, free of charge, to any person obtaining a copy
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software. As clarification, there is no
requirement that the copyright notice and permission be included in binary
distributions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## DXVK licence

```
                  Copyright (c) 2017 Philip Rebohle
                  Copyright (c) 2019 Joshua Ashton
                  Copyright (c) 2019 Robin Kertels
                  Copyright (c) 2023 Jeffrey Ellison

                          zlib/libpng license

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

– The origin of this software must not be misrepresented; you must not
  claim that you wrote the original software. If you use this software
  in a product, an acknowledgment in the product documentation would be
  appreciated but is not required.

– Altered source versions must be plainly marked as such, and must not
  be misrepresented as being the original software.

– This notice may not be removed or altered from any source distribution.
```
