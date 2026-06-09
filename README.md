# STC v1.0

STC v1.0 is the public release name for the final `digit_context_v5` C++
BWT-family compression candidate from this workspace.

This repository contains the source code, build files, verification scripts,
and release documentation for the highest-compression result produced in the
project. It intentionally excludes raw datasets and large generated result
directories.

## Current Result

Status: local LTCB-style BWT-family submission candidate. This is not an
official leaderboard claim until an external reviewer or the LTCB maintainer
rebuilds, decodes, hashes, and accepts the submission.

Verified local result:

- Dataset: standard `enwik9`, `1,000,000,000` bytes
- Archive: `round_v271_enwik9_full_single.tcv5`
- Archive bytes: `157,388,188`
- Decoder-only source zip bytes: `183,174`
- Local LTCB-style total: `157,571,362`
- Comparison baseline `bsc-m03` BWT total: `160,364,392`
- Local margin vs `bsc-m03`: `2,793,030` bytes
- Archive SHA-256:
  `c6d4635c650c4d81194dbc310d512f3069a54676cea53c08314cad802f20cf87`
- Decoder zip SHA-256:
  `71b5de102a170b1e52797b6ea1b9b2dde2c30ff8c92d9e5060d52c0fccdd328d`
- Decoded `enwik9` SHA-256:
  `159b85351e5f76e60cbe32e04c677847a9ecba3adc79addab6f4c6c7aa3744bc`

## Repository Layout

- `cpp/digit_context_v5/src/dcv5.cpp` - C++ encoder/decoder implementation.
- `cpp/digit_context_v5/CMakeLists.txt` - CMake build entry point.
- `external/bsc-m03/m03_tables.h` - fixed GPL-3.0-or-later M03 model tables.
- `external/bsc-m03/libsais/` - Apache-2.0 libsais source used for BWT construction.
- `scripts/` - local packaging, release gate, and LTCB-style verification scripts.
- `output/round_v271_64k.tcv5` - small smoke archive used by verification scripts.
- `docs/` - reports, release process, and verification summaries.

Large release artifacts are not committed to the repo because the scored
archive is larger than GitHub's normal repository file limit. Upload them as
GitHub Release assets instead.

## Build

With CMake:

```sh
cmake -S cpp/digit_context_v5 -B build/dcv5 -DCMAKE_BUILD_TYPE=Release
cmake --build build/dcv5 --config Release
```

Direct GCC/G++ build:

```sh
gcc -std=c99 -O2 -Wall -Wextra -Wpedantic -c external/bsc-m03/libsais/libsais.c -o libsais.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic cpp/digit_context_v5/src/dcv5.cpp libsais.o -o dcv5
```

## Usage

```sh
dcv5 encode input archive.tcv5
dcv5 decode archive.tcv5 output
dcv5 inspect archive.tcv5
```

The `encode-chunked-adaptive` command also exists for experimental local runs:

```sh
dcv5 encode-chunked-adaptive input archive.tcv5 10000000 1000000
```

## Verification

Place the standard datasets under `data/` when running local verification:

- `data/enwik8` for smoke and prefix tests.
- `data/enwik9` for full verification.

Place the release assets under `output/` or pass explicit paths to the scripts:

```powershell
.\scripts\verify_ltcb_submission.ps1 `
  -Archive output\round_v271_enwik9_full_single.tcv5 `
  -DecoderZip output\ltcb_dcv5_decoder_only_src_v277.zip `
  -Full
```

For routine local gates:

```powershell
.\scripts\run_dcv5_release_gate.ps1
```

For pre-publication validation:

```powershell
.\scripts\run_dcv5_release_gate.ps1 -Full
```

The v283 full verification summary is preserved at
`docs/verification_summary_v283.json`.

## Low-Memory enwik9 Run

The single-block `encode` path is the scored high-ratio path. Do not use
`encode-chunked` for the final benchmark score unless a memory-constrained run
explicitly requires it, because chunk boundaries reduce the BWT-family context
available to the compressor.

The 2026-06-07 low-memory build frees temporary BWT and inverse-BWT buffers
before the largest parser and reconstruction phases. On the local validation
machine, full single-block `enwik9` reproduced the same archive:

- Archive bytes: `157,388,188`
- Archive SHA-256:
  `c6d4635c650c4d81194dbc310d512f3069a54676cea53c08314cad802f20cf87`
- Decoded `enwik9` SHA-256:
  `159b85351e5f76e60cbe32e04c677847a9ecba3adc79addab6f4c6c7aa3744bc`
- Observed encode peak working set: `10.782 GiB`
- Observed decode peak working set: `10.351 GiB`

See `docs/LOW_MEMORY_VALIDATION_20260607.md` for the validation commands,
hashes, and memory observations.

## Release Assets

The prepared package has a sibling `release-assets/` directory containing:

- `round_v271_enwik9_full_single.tcv5`
- `ltcb_dcv5_decoder_only_src_v277.zip`
- `SUBMISSION_MANIFEST.md`
- `SHA256SUMS.txt`

Upload those files to the GitHub Release for this tag. Do not commit the full
`.tcv5` archive to the Git repository.

## License And Notices

This release includes GPL-3.0-or-later model table data from `bsc-m03`, so the
repository root includes the GPL license. The bundled libsais source is
Apache-2.0 and its license is preserved under `external/bsc-m03/libsais/`.

See `THIRD_PARTY_NOTICES.md` for details.
