# digit_context_v5 Engineering Checklist

## Current Status

`v277` is a locally verified LTCB-style BWT-family submission candidate:

- Scored archive: `output/round_v271_enwik9_full_single.tcv5`
- Decoder package: `output/ltcb_dcv5_decoder_only_src_v277.zip`
- Local total: `157,571,362` bytes
- Full `enwik9` decode SHA-256: OK
- Ledger total: matches archive bytes

This is not official leaderboard status until the LTCB maintainer accepts the
submission.

## Mandatory Gate Before Any Claim

Run the smoke gate:

```powershell
.\scripts\run_dcv5_release_gate.ps1
.\scripts\verify_ltcb_submission.ps1
```

Run the full gate before publishing a new scored decoder package:

```powershell
.\scripts\run_dcv5_release_gate.ps1 -Full
.\scripts\verify_ltcb_submission.ps1 -Full
```

Run the local prefix speed/roundtrip benchmark after compressor changes:

```powershell
.\scripts\benchmark_dcv5_prefixes.ps1
.\scripts\benchmark_dcv5_prefixes.ps1 -PrefixBytes 4194304,16777216
```

Required pass conditions:

- Decoder zip extracts with only expected source/documentation files.
- Source boundary audit reports no external process launcher, dynamic loader,
  or forbidden codec keyword in the decoder source path.
- Decoder builds from the zip.
- 64KiB smoke decode SHA-256 is OK.
- Truncated smoke archive is rejected.
- Full `enwik9` decode SHA-256 is OK when `-Full` is used.
- Ledger reports `ledger_total_matches=true`.
- Local score remains below the `bsc-m03` BWT total baseline.
- Prefix benchmark rows report `sha256_ok=true` and
  `ledger_total_matches=true`.

## Package Regeneration

The decoder-only package can be regenerated from the full C++ source:

```powershell
python scripts\package_dcv5_decoder_only.py --out-dir output\ltcb_dcv5_decoder_only_src_generated --zip output\ltcb_dcv5_decoder_only_src_generated.zip
```

A generated package is not a scored candidate until it passes the full gate.

## Known Engineering Gaps

- Compression speed is still research-prototype level: full `enwik9` took about
  11 minutes 25 seconds on this machine.
- The compressor path is single-process and does not yet have profile-guided
  optimization, block parallelism, or low-level memory tuning.
- CI is local-only. Cross-platform Linux/macOS builds remain to be verified.
- Fuzzing is limited to the current smoke/truncated gate; broader malformed
  archive coverage is still needed.
- Fixed M03 model tables must remain disclosed in any submission.
