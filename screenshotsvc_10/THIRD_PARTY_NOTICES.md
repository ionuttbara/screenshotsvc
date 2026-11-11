# Third-party notices

ScreenshotService remains licensed under GPL-3.0-or-later as stated in `LICENSE`.

The native hardware encoder and diagnostic implementation in `nvenc.c`, `amfenc.c`,
`qsvenc.c`, `vpconv.c`, and `crash.c` is adapted from the GPL-3.0 Nitshot source
provided for this work. The adaptation integrates those paths with ScreenshotService's
recording lifecycle and Media Foundation fallback. The former experimental FFmpeg /
D3D12VA recording backend is not included.

SDK headers used at build time are kept with their own notices:

- `third_party/nvenc/`: NVIDIA NVENC API header from FFmpeg's `nv-codec-headers`,
  distributed under the MIT license noted in the header and adjacent README.
- `third_party/amf/`: AMD AMF headers under the MIT license in `LICENSE.txt`. See
  AMD's notice at the start of that file concerning third-party media technology
  patent royalties; the headers do not grant codec patent licenses.
- `third_party/vpl/`: Intel oneVPL API headers under the MIT license in `LICENSE`.

Encoder runtime DLLs are loaded from the installed GPU driver at run time; this
source archive does not redistribute NVIDIA, AMD, Intel, or FFmpeg runtime binaries.
