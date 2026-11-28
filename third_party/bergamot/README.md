# Bergamot

Mozilla's machine translation engine (Bergamot, on its fork of Marian), as
Firefox Translations runs it, for Compass's on-device page translation
(docs/design/translation.md). Built natively by `BUILD.gn` into
`libcompass_translatekit`; Firefox itself ships it as WebAssembly.

Vendored by `tools/translation/update_bergamot.py`, which fetches each
project below at its revision, copies only what inference needs and applies
`patches/`. Don't edit the vendored files by hand: add a patch and run the
script.

| Directory | Project | Revision | License |
|---|---|---|---|
| `src/translator/`, `BERGAMOT_VERSION` | [mozilla/translations](https://github.com/mozilla/translations) `inference/` (Bergamot 0.6.0, the revision Firefox pins in `toolkit/components/translations/bergamot-translator/moz.yaml`) | `eea6e5a80aa4ddd86d9cc35ce9a65b79aa3ab96d` | MPL-2.0 (`LICENSE`) |
| `marian/` | Marian, the same repository's `inference/marian-fork/` | same | MIT (`marian/LICENSE.md`) |
| `marian/src/3rd_party/intgemm/` | [kpu/intgemm](https://github.com/kpu/intgemm) | `f7401513da71758dacce52fed1c7855549abee59` | MIT |
| `marian/src/3rd_party/simd_utils/` | [browsermt/simd_utils](https://github.com/browsermt/simd_utils) | `d0793d86aea9036a5bc77b9ca7791dff024168ca` | BSD-2-Clause (sse2neon in it: MIT) |
| `marian/src/3rd_party/onnxjs/` (the GEMM only) | [browsermt/onnxjs](https://github.com/browsermt/onnxjs) | `924924b08e9596b41aeebada4a172f026be95f5a` | MIT |
| `ssplit/` | [browsermt/ssplit-cpp](https://github.com/browsermt/ssplit-cpp), the sentence splitter | `a311f9865ade34db1e8e080e6cc146f55dafb067` | Apache-2.0 |
| `pcre2/` | [PCRE2](https://github.com/PCRE2Project/pcre2) 10.49, for ssplit | `388ef3c412ab5269b8f76101a6bef2d416974a15` | BSD-3-Clause with the PCRE2 exception (`LICENCE.md`) |

Marian's own third-party code in `marian/src/3rd_party/`, at Marian's
revision, each with its license file: CLI11, yaml-cpp, spdlog, Pathie,
phf, mio, cnpy (MIT), and half_float (BSD, in `umHalf.h`).

## What isn't here

- **Training, faiss, LSH, ONNX export, gzip input.** `BUILD.gn` defines
  `WASM_COMPATIBLE_SOURCE`, as Firefox's build does, which compiles them
  out; only `training/training_state.h`, which inference includes, is
  kept. The define also makes x86 rounding scalar instead of SSE4.1, so the
  engine needs only the SSSE3 that intgemm's slowest path wants.
- **ruy, cpuinfo, zlib, SentencePiece, protobuf, eigen:** Chromium's own
  (`//third_party/ruy`, `//third_party/sentencepiece`, ...), not Marian's
  copies. SentencePiece's trainer isn't built, so Marian's vocabulary
  training isn't either.
- **Generated files:** Marian's `common/git_revision.h`,
  `common/project_version.h` and `common/build_info.cpp`, Bergamot's
  `translator/project_version.h`, intgemm's `intgemm_config.h`, and
  PCRE2's `config.h`, `pcre2.h` and `pcre2_chartables.c` come from
  `BUILD.gn`, not from CMake.
- CUDA, FBGEMM (its two headers are kept: they're included, and their code
  is behind `USE_FBGEMM`), NCCL, the WASM bindings, emsdk, tests,
  benchmarks, PCRE2's JIT and tools.

## Matrix maths by platform

- Integer (the int8 models): intgemm on x86-64 (SSSE3, AVX2 or AVX-512,
  chosen at run time), ruy on arm64.
- Float: Accelerate on Apple platforms, ruy on other arm64, Marian's ONNX
  GEMM (on Chromium's eigen) on x86-64 elsewhere, so no BLAS ships.

## Local changes

None yet (`patches/`).
