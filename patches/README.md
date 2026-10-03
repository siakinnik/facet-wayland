# Patches to the bundled libraries

`scripts/build-deps.sh` applies `<library>-*.patch` to the library's sources
after cloning them (see DEPS for the versions).

- `wlroots-gpu-buffers.patch` changes wlroots 0.18.2: software renderers may
  read single-plane linear dma-bufs, linux-dmabuf works without access to
  the DRM device, and the compositor decides which clients see the
  clipboard. It contains parts of wlroots, which is under the MIT license:

```
Copyright (c) 2017, 2018 Drew DeVault
Copyright (c) 2014 Jari Vetoniemi
Copyright (c) 2023 The wlroots contributors

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The patch itself is part of this module and under its license (GPL-3.0); the
wlroots code it touches stays under the MIT license above.
