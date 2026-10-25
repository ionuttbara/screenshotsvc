# oneVPL headers

Intel's oneAPI Video Processing Library API headers, unmodified, from
[intel/libvpl](https://github.com/intel/libvpl), tag `v2.13.0` (`api/vpl`,
without the C++ wrappers and the preview folder).

- The GPU runtime (`libmfx64-gen.dll`, API 2.x) comes with the Intel graphics
  driver for Arc and Xe, in the driver's DriverStore folder; `qsvenc.c` finds
  it through the adapter's `DriverStorePathForVPL` value, as Intel's
  dispatcher does, and loads it only from under `System32\DriverStore`.
  Intel's dispatcher itself is not used - it is C++ - and nothing is linked.
- Only the stable API is used; the D3D11 surface import is still
  `ONEVPL_EXPERIMENTAL` (in 2.17 too), so frames reach the encoder through an
  application frame allocator instead.
- Licence: MIT, in `LICENSE` beside this file; compatible with Nitshot's
  GPL-3.0. The headers compile cleanly as C17 at `/W4 /WX`.

| File | SHA-256 |
|---|---|
| `vpl/mfx.h` | `edd1b3e1133f835a137eb454cef952725e03abc00bb71226a942e4d43532e157` |
| `vpl/mfxadapter.h` | `1943b3a47cb9945eeb7703fef19738318a52544d6c5c174d8fafa81cfe803f02` |
| `vpl/mfxbrc.h` | `8548390294d8549f100a8e9b7e7a7631c78e8ede8087ec89b6f62a3854fcce71` |
| `vpl/mfxcamera.h` | `fc98ca2f285e53e8a03f453ec840f6bee48ec8489ecc93ca5159e20679c83fda` |
| `vpl/mfxcommon.h` | `c29d6dfc6ca1563bf5c11fe60ca0952e1ce54f451e7311e9e6d9f9897aa81b3e` |
| `vpl/mfxdefs.h` | `40ce5fc86d38271031ef81cfedaee8d24ff32635d213e6f9d3fd7c65f8128168` |
| `vpl/mfxdispatcher.h` | `19072d08daa7fc094d690c387be162fe4a8a5a30316886ac333deeedec67a0d8` |
| `vpl/mfxdispatcherprefixedfunctions.h` | `6d3c6d8bfea3ff213eda97af29de0c83023f40b2ef7c47b231d69c456601cfa9` |
| `vpl/mfxencodestats.h` | `5be29150757c39c1931d08d9c251324054ed407914fde8aa7986653df9013b22` |
| `vpl/mfximplcaps.h` | `6b827323e5b7495e6d9948c7fb3e4fcbd88b15a61bc3a71a0ea4a19e512ad4c2` |
| `vpl/mfxjpeg.h` | `6bccee77bb4f50f8605689b86f9066a4c207393e93b80ac09ca5991cc02685df` |
| `vpl/mfxmemory.h` | `4a3dc24bb3a925011157f17419c92d2ac951a9c72922637f5a4a49adeb498973` |
| `vpl/mfxmvc.h` | `4e3b5a0ab44cf25d1ea903f86c574c80137f5adb6cfba17d37f73564f0ea452e` |
| `vpl/mfxpcp.h` | `c00ed6ee90018090de26b7515d6a2394606e8c56ef1d1accbfb56cbf4fd7f9c7` |
| `vpl/mfxsession.h` | `d9a3d568df1db1b6267e0992c71cbe2522566e7b16bb80f8e6c70a473a4598c7` |
| `vpl/mfxstructures.h` | `6355b65aea817da9a1c160ada38e0761b79c71127c61bb22a017fc048a339b11` |
| `vpl/mfxsurfacepool.h` | `e05f031d9c0b856c3d1947888560025983d30350c594b5530470ba5eefa14fa0` |
| `vpl/mfxvideo.h` | `242cf5ebedd0101c7867ad004c562e9db6364d98a4c905e28f622e02b5b39519` |
| `vpl/mfxvp8.h` | `7a1f260be6d0aa441bc87349fc74d1c30475c7504520ddf7f827eba0752262ce` |
