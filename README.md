# STC v1.0

STC v1.0 is an experimental block sorting compressor built around the
`digit_context_v5` BWT-family path from this workspace.

The public repository layout intentionally follows the compact style used by
`bsc-m03`: the CMake entry point and main compressor source are in the repository
root, fixed M03 model tables are checked in as source data, and the required
`libsais` BWT builder source is bundled directly under `libsais/`.

This is a local LTCB-style submission candidate. It should not be described as
an official leaderboard result until an external reviewer or benchmark
maintainer rebuilds, decodes, hashes, and accepts the submission.

## License

STC v1.0 is distributed under the GNU General Public License because it includes
GPL-licensed M03 model table data derived from `bsc-m03`. The bundled `libsais`
source is Apache-2.0 and keeps its own license under `libsais/LICENSE`.

See `THIRD-PARTY-NOTICES` for details.

## Changes

* 2026-05-28 : Version 1.0.0
  * Repackaged the release in a root-level CMake/source layout modeled after
    `bsc-m03`.
  * Preserved validation evidence for the `v271` full enwik9 archive and
    `v277` decoder-only source package.
* 2026-05-19 : Internal validation
  * Full enwik9 decode verification passed with byte-for-byte SHA-256 match.
  * Local LTCB-style score measured below the `bsc-m03` BWT-family baseline.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Direct GCC/G++ build:

```sh
gcc -std=c99 -O2 -Wall -Wextra -Wpedantic -c libsais/libsais.c -o libsais.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic dcv5.cpp libsais.o -o stc
```

## Usage

```sh
stc encode input archive.tcv5
stc decode archive.tcv5 output
stc inspect archive.tcv5
```

The experimental adaptive command is also present:

```sh
stc encode-chunked-adaptive input archive.tcv5 10000000 1000000
```

## Repository Layout

* `dcv5.cpp` - encoder, decoder, archive inspection, and CLI implementation.
* `m03_tables.h` - fixed M03 model tables used by the arithmetic predictor.
* `libsais/` - bundled suffix-array/BWT construction dependency.
* `docs/` - validation notes, release process, and engineering checklist.
* `LICENSE`, `THIRD-PARTY-NOTICES` - license and attribution files.

Large scored archives are not committed to this repository tree.

## Benchmarks

### Large Text Compression Benchmark Corpus

| File name | Input size (bytes) | Output size (bytes) | Notes |
|:---------------:|:-----------:|:------------:|:------|
| enwik8 | 100000000 | 19996029 | Local strict-clean verification; SHA-256 OK |
| enwik9 | 1000000000 | 157388188 | Local full decode verification; SHA-256 OK |

Local enwik9 score accounting:

| Component | Bytes |
|:--|--:|
| Archive | 157388188 |
| Decoder-only source zip | 183174 |
| Local LTCB-style total | 157571362 |
| `bsc-m03` BWT baseline total used locally | 160364392 |
| Local margin vs baseline | 2793030 |

Verification evidence is preserved in `docs/verification_summary_v283.json`.

### Low-memory single-block validation

The single-block `encode` path is the scored high-ratio path. Do not use
`encode-chunked` for the final benchmark score unless a memory-constrained run
explicitly requires it, because chunk boundaries reduce the BWT-family context
available to the compressor.

The 2026-06-07 low-memory build frees temporary BWT and inverse-BWT buffers
before the largest parser and reconstruction phases. On the local validation
machine, full single-block `enwik9` reproduced the same archive:

| Metric | Value |
|:--|--:|
| Archive bytes | 157388188 |
| Observed encode peak working set | 10.782 GiB |
| Observed decode peak working set | 10.351 GiB |

Hashes:

* Archive SHA-256:
  `c6d4635c650c4d81194dbc310d512f3069a54676cea53c08314cad802f20cf87`
* Decoded `enwik9` SHA-256:
  `159b85351e5f76e60cbe32e04c677847a9ecba3adc79addab6f4c6c7aa3744bc`

See `docs/LOW_MEMORY_VALIDATION_20260607.md` for the validation commands,
hashes, and memory observations.
