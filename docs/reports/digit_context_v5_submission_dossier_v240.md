# digit_context_v5 Submission Dossier v240

## Status

This is a local reproducible strict-clean BWT-family result for full enwik8.
It is not an official LTCB ranking claim until an external reviewer or the
official submission path reproduces and accepts it.

## Primary Result

- Dataset: standard enwik8, 100,000,000 bytes
- Source SHA-256: `2b49720ec4d78c3c9fabaee6e4179a5e997302b3a70029f30f2d582218c024a8`
- Accepted archive: `results/round_v230_digit_context_v5_full_enwik8.tcv5`
- Accepted archive bytes: `19,996,029`
- Baseline bsc-m03 bytes: `20,263,925`
- Margin vs bsc-m03: `267,896` bytes
- Margin vs 20,000,000-byte target: `3,971` bytes
- Decode verification: SHA-256 OK
- Ledger verification: `ledger.total_bytes == archive_bytes`

## External Package

- Use package: `output/digit_context_v5_repro_package_v239_20260519_0630.zip`
- Package SHA-256: `0a1cbbe735096d76e536723ce442c5eda90526e6bf5f479c15b2599a43353370`
- Package bytes: `20,487,676`
- Do not use v236 or v238 for review. Isolation testing found missing dependencies in those packages.

## Independent Validation Performed

- v233: stable CLI verified the persisted full archive.
- v234: regression script verified 64KiB smoke and full persisted archive.
- v235: fresh full compression from `data/enwik8` reproduced the exact v230 archive hash, then fresh decompression matched source SHA-256.
- v239: extracted the external package into an isolated directory, supplied only standard `data/enwik8`, and package-local `verify` passed.
- v240: release compliance audit passed.

## Compliance Boundary

The accepted path uses the local `digit_context_v5` codec with decoder-synchronous digit side streams and local clean M03-style statistical coding labels in the JSON (`main_codec=m03`, side streams `m03`). The release audit found no `bsc` or `bsc-m03` executable payload in the package and no selected raw/ZPAQ/brotli/lzma/zstd/bzip2 fallback mode.

The package includes:

- `ltcboost/_tcbwt_arith.cp312-win_amd64.pyd` for local arithmetic-coder acceleration.
- `external/bsc-m03/build_manual/liblibsais.dll` only as suffix-array / inverse-BWT infrastructure.
- `external/bsc-m03/m03_tables.h` and `m03_tables_compact.xz` as fixed model tables used by the local parser.

The package does not include `bsc`, `bsc-m03`, zpaq, brotli, lzma, zstd, or bzip2 executables as compression backends.

## Reproduction Commands

From the extracted v239 package root, place standard enwik8 at `data/enwik8`, then run:

```powershell
python scripts\digit_context_v5_cli.py verify `
  --archive results\round_v230_digit_context_v5_full_enwik8.tcv5 `
  --jsonl results\round_v230_digit_context_v5_full_enwik8_archive_out.jsonl `
  --source data\enwik8 `
  --out results\external_verify.json
```

Fresh production check:

```powershell
python scripts\digit_context_v5_cli.py compress data\enwik8 results\fresh.tcv5 --json-out results\fresh_compress.json
python scripts\digit_context_v5_cli.py decompress results\fresh.tcv5 results\fresh.dec --verify-source data\enwik8 --json-out results\fresh_decompress.json
```

Expected accepted archive bytes: `19,996,029`.

## Current Caveat

This is ready for expert review or official submission packaging. Public claims should say "local reproducible result below bsc-m03" until official/external reproduction accepts it.
