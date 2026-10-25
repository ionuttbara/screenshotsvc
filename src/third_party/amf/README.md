# AMF headers

AMD's Advanced Media Framework SDK headers, unmodified, from
[GPUOpen-LibrariesAndSDKs/AMF](https://github.com/GPUOpen-LibrariesAndSDKs/AMF),
tag `v1.4.35` (`amf/public/include`): all of `core/` except `VulkanAMF.h` and
`D3D12AMF.h`, which nothing here includes, and the six components the encoder
needs.

- Runtime version 1.4.35 or later is needed - the AMD driver's `amfrt64.dll`,
  loaded from System32 at run time; nothing is linked.
- Licence: MIT, in `LICENSE.txt` beside this file, compatible with Nitshot's
  GPL-3.0. The notices at the top of that file - on codec patent royalties
  and on FFmpeg - are AMD's, for the SDK as a whole; only the headers are
  used here.
- Used through the headers' C interface; they compile cleanly as C17 at
  `/W4 /WX`.

| File | SHA-256 |
|---|---|
| `components/ColorSpace.h` | `2f6887d14516ebdcf034d59e39235f5d9974533ddb0b04d685c0c3b6d397d3d6` |
| `components/Component.h` | `f3f18680d55a65fdf5e0351d3fde1dcd9bdd2d6caec268c8c915c4227ffa1edc` |
| `components/ComponentCaps.h` | `2479eb2a6174a1b63552c509abac1b701eddcab5a5eac5a7f4d9f40274d588b4` |
| `components/PreAnalysis.h` | `dae3564f20ef5db959d31dfc74c226453b039f4919ddb65027a183cdae95c171` |
| `components/VideoEncoderHEVC.h` | `b7827d2c3380a32cef350cf0f180802694f9f93db295f1c8e08238160d17c3d8` |
| `components/VideoEncoderVCE.h` | `d106ce15028a9ea42e7299cff4b1a8697614a746b96740c9251f9b943bac2edc` |
| `core/AudioBuffer.h` | `c9382c911fd3b5e12105c1d2a67eb11e62e9475068825222a285bb1314ac0ddd` |
| `core/Buffer.h` | `1ad18d3c813c3f81a836d393bbe86dfc0b7a1d7f04a77a272eb1461ce85ae3a4` |
| `core/Compute.h` | `ef5721466fe48e1c038151018699519761e6e351e66e76a5de65386924653e20` |
| `core/ComputeFactory.h` | `50acf69fb670bc6cf093384d1b698bf9e44cda8dd49e02ef24eacfb86ad8ccae` |
| `core/Context.h` | `f9b4acb784038aad234ddec490e5f6ad6dacc4b14794eb84f3523d56dc3d6dd2` |
| `core/CurrentTime.h` | `76d16ba57d48ae9e457f3d904698a1e7c625007ac8887db9bcecdb90fd36d835` |
| `core/Data.h` | `eaac4d80dd3215638c70f8a4ca01791d26d9df77586f665a772bb51907a8035b` |
| `core/Debug.h` | `56666589bf6ad20c6286a048da751c2d7722f70c73d962ee3c205a6d709b6aff` |
| `core/Dump.h` | `a5e0fa9290cae767a4d135962ee5e30302e65e8d0ca5e016c620e72b759c86c6` |
| `core/Factory.h` | `9ba2ebe93af628bce621f29de493cddab87bf8a030eed504b25fcdc822c61b44` |
| `core/Interface.h` | `304f6d6077e9b0733859d44a5e43a343ae374c8fe72c7d79741c0aca89c21162` |
| `core/Plane.h` | `5ad5165e9de2afcfc2da828e8704c838b1f82e8ead7422fbcfc32c758672e1f3` |
| `core/Platform.h` | `59a4d11ecd150af25ee0b8d1d15f2efdfe5cf1060b8e8ab539f6a46d4bff8bc8` |
| `core/PropertyStorage.h` | `0059b787e78cd7c4e57212ccaab56ad7e17e9474a31337da0eac5b2d056a5dd8` |
| `core/PropertyStorageEx.h` | `871136e46586baa1fc77121935b503aaa16523d006c3e435ce50698605675fad` |
| `core/Result.h` | `a469ff33875484b5762199da757819056ff4a7535bfdcec1f4cf2ffc403b98f2` |
| `core/Surface.h` | `7f0c99bbfe156169440031f75b0e37c28c435dd140ecb4e5089d0b1f01912c47` |
| `core/Trace.h` | `ae8c5fee671f02b90d3ccab312cfa6f74424351f070a7dd4ed7aba437fd55c30` |
| `core/Variant.h` | `72841e751595a2a8f62c8ac4a3e630b181411af8e0741d716bbe3379c9bf9a99` |
| `core/Version.h` | `e26e25e658513f7120bb16d64d753d99205b456d83a1120a0a0d2ffdbf19f8c5` |
