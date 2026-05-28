# digit_context_v5 Strict Clean BWT Expert Report

Date: 2026-05-19

## Executive Summary

The current accepted result is a strict clean BWT-family compression path for full enwik8:

- Archive: `results/round_v230_digit_context_v5_full_enwik8.tcv5`
- Compressed bytes: `19,996,029`
- Full enwik8 input bytes: `100,000,000`
- Decode SHA-256: OK
- Ledger total: OK
- Persisted file size: `19,996,029`, matching JSON and ledger
- Margin vs bsc-m03 `20,263,925`: `267,896` bytes better
- Margin vs hard target `20,000,000`: `3,971` bytes better

This should be described as a local strict-clean result until independently reproduced under the exact intended LTCB/baseline rules.

## What Changed

The accepted implementation is `digit_context_v5` in:

- `scripts/probe_digit_context_v5_m03clean.py`
- Reproducible CLI: `scripts/digit_context_v5_cli.py`
- Verification helper: `scripts/verify_digit_context_v5_artifact.py`

The transform replaces digit bytes in the main stream with a fixed digit placeholder (`'9'`) and moves the original digit payloads into deterministic side streams. The main stream and every side stream are compressed through the existing clean BWT/M03 path.

The final container uses 13 side streams:

- Buckets are digit-run lengths capped at 12.
- Buckets 1, 2, 3, 5-12 each use one side stream.
- Bucket 4 is split into two streams: the two bytes of the packed 4-digit integer.

## Innovation

The key idea is decoder-synchronous side-stream ordering. The encoder does not store a permutation table or explicit interval boundary table. Instead, the decoder reconstructs the same ordering from data it already has:

- decoded main stream context around each digit run;
- deterministic run length buckets;
- for bucket 4, the already decoded first byte of the packed 4-digit value.

The strongest improvement came from bucket 4. Four-digit runs are packed as a two-byte big-endian integer. `digit_context_v5` decodes the first byte stream first, then sorts the second byte stream by that decoded first byte. This uses side information recovered during decoding, not stored side metadata.

## Why It Works

The digit payload distribution is not independent of its surrounding text. Page IDs, timestamps, years, numeric XML fields, coordinates, and repeated wiki-format patterns produce digit runs whose values correlate with nearby non-digit bytes and with high-order bits of the numeric value.

The original v189 digit-pair-length transform already separated digit payloads but kept side streams in source order. `digit_context_v5` keeps the same reversible main/side separation, then reorders side payloads into lower-entropy BWT inputs using deterministic context.

Important byte accounting:

- v189 side payload: `627,565`
- v230 side payload: `595,257`
- side payload gain: `32,308`
- final full archive: `19,996,029`

## Validation Evidence

Accepted full run:

- JSON: `results/round_v230_digit_context_v5_full_enwik8_archive_out.jsonl`
- Archive: `results/round_v230_digit_context_v5_full_enwik8.tcv5`
- Stderr: `results/round_v230_digit_context_v5_full_enwik8.stderr.log`

Full ledger:

```text
total_bytes = 19,996,029
mode_bytes = 1
digit_header_bytes = 39
main_payload_bytes = 19,400,733
side_payload_bytes = 595,257
```

Prefix gates completed:

```text
64KiB    19,322 bytes, sha256 OK, ledger OK
256KiB   67,195 bytes, sha256 OK, ledger OK
1MiB    251,440 bytes, sha256 OK, ledger OK
4MiB    972,052 bytes, sha256 OK, ledger OK
16MiB 3,634,001 bytes, sha256 OK, ledger OK
```

Smallbench:

- 6/6 files decoded SHA-256 OK.
- Ledgers matched archive sizes.

Independent verifier command:

```powershell
python scripts\verify_digit_context_v5_artifact.py `
  --archive results\round_v230_digit_context_v5_full_enwik8.tcv5 `
  --jsonl results\round_v230_digit_context_v5_full_enwik8_archive_out.jsonl `
  --source data\enwik8 `
  --out results\round_v231_digit_context_v5_independent_verify.json
```

Stable CLI verification command:

```powershell
python scripts\digit_context_v5_cli.py verify `
  --archive results\round_v230_digit_context_v5_full_enwik8.tcv5 `
  --jsonl results\round_v230_digit_context_v5_full_enwik8_archive_out.jsonl `
  --source data\enwik8 `
  --out results\round_v233_digit_context_v5_cli_verify_full.json
```

The CLI verifier produced `all_checks_passed: true` in `179.958` seconds.

## Compliance With Clean Constraints

This result does not use the prohibited routes as the final compression backend:

- no bsc/bsc-m03 wrapper/backend for the container;
- no zpaq, CM, brotli, lzma, zstd, bzip2, or raw fallback victory path;
- no explicit suffix interval boundary table;
- no proxy entropy claim as final result;
- full archive is decoded and SHA-256 checked;
- ledger total equals actual persisted blob size.

The path still uses the local clean BWT/M03 implementation as its BWT-family entropy path. The report should not claim an official external leaderboard position until reproduced by an independent reviewer under official LTCB rules.

## Risks And Caveats

- The margin under 20,000,000 is only `3,971` bytes, so any serialization or environment change can erase it.
- The implementation is currently a research/probe script, not a polished CLI compressor format.
- Official leaderboard claims require exact rule matching, reproducible build instructions, and external verification.
- There is an unrelated old harness process under `.codex-harness\bwt-ten-method-bscm03`; it is not part of this accepted result.

## Recommended Next Steps

1. Keep `digit_context_v5` frozen as the accepted reference until independent verification is complete.
2. Run `scripts/verify_digit_context_v5_artifact.py` on a second machine or clean checkout.
3. Convert the probe script into a stable encoder/decoder CLI with format versioning.
4. Prepare a paper section around decoder-synchronous side-stream ordering and dependent side-stream ordering.
5. Only resume ratio work after the verifier and report artifacts are preserved.
