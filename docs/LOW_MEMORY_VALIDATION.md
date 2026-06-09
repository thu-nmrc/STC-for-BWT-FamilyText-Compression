# STC low-memory validation, 2026-06-07

Goal: keep the winning single-block STC path below an 11.8 GB available-memory
target so that the benchmark does not need to use `encode-chunked 500000000`.

## Source change

- Source: `STC_v1.0_GitHub_upload/cpp/digit_context_v5/src/dcv5.cpp`
- Released the temporary forward-BWT vector before entering the largest M03
  parser state.
- Avoided copying the arithmetic encoder payload at `finish()`.
- Added a clear `std::bad_alloc` diagnostic.
- Added a low-memory inverse-BWT path that reconstructs LF directly from the
  decoded L stream, avoiding simultaneous `bwt`, `full_l`, and `ranks`
  gigabyte-scale buffers.

## Build

```powershell
gcc -std=c99 -O2 -Wall -Wextra -Wpedantic -c STC_v1.0_GitHub_upload\external\bsc-m03\libsais\libsais.c -o build\dcv5_lowmem_20260607_v2\libsais.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic STC_v1.0_GitHub_upload\cpp\digit_context_v5\src\dcv5.cpp build\dcv5_lowmem_20260607_v2\libsais.o -o build\dcv5_lowmem_20260607_v2\stc_lowmem_v2.exe
```

## 64 KiB smoke

- Archive bytes: `19,322`
- Decoded bytes: `65,536`
- Round-trip: OK

## Full enwik9 encode

- Command: `stc_lowmem.exe encode data\enwik9 output\lowmem_stc_20260607\full_enwik9\enwik9_single_lowmem.tcv5`
- Archive bytes: `157,388,188`
- Main payload bytes: `148,105,611`
- Side payload bytes: `9,282,534`
- Ledger total matches: true
- Observed peak working set: `10.782 GiB`

## Full enwik9 decode

- Command: `stc_lowmem_v2.exe decode output\lowmem_stc_20260607\full_enwik9\enwik9_single_lowmem.tcv5 output\lowmem_stc_20260607\full_enwik9\enwik9_single_lowmem_v2.dec`
- Decoded bytes: `1,000,000,000`
- Ledger total matches: true
- Observed peak working set: `10.351 GiB`

## Hashes

- Source `data\enwik9` SHA-256:
  `159b85351e5f76e60cbe32e04c677847a9ecba3adc79addab6f4c6c7aa3744bc`
- Decoded output SHA-256:
  `159b85351e5f76e60cbe32e04c677847a9ecba3adc79addab6f4c6c7aa3744bc`
- Archive SHA-256:
  `c6d4635c650c4d81194dbc310d512f3069a54676cea53c08314cad802f20cf87`

## Result

The single-block archive result is unchanged and the observed encode/decode
working sets are below the 11.8 GB target. The chunked benchmark result should
not be used as the final comparison unless the benchmark environment cannot run
this low-memory single-block build.
