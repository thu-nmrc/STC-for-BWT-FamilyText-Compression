# digit_context_v5 Release Process

This project is not considered mature because it has a good compression score
alone. A release candidate must pass repeatable build, verification, speed,
boundary, and packaging gates.

## Release Levels

### Level 1: Local Candidate

- `scripts\verify_ltcb_submission.ps1` passes.
- The decoder-only package builds from a clean extraction.
- 64KiB smoke decode SHA-256 is OK.
- Ledger total matches archive bytes.
- Truncated archive rejection is checked.
- Source-boundary audit is clean.

### Level 2: Engineering Candidate

- Level 1 passes.
- `scripts\benchmark_dcv5_prefixes.ps1 -PrefixBytes 4194304,16777216`
  passes.
- Benchmark rows report SHA-256 OK and ledger total matches.
- Speed numbers are recorded from real serialized archives, not estimates.

### Level 3: Submission Candidate

- Level 2 passes.
- `scripts\verify_ltcb_submission.ps1 -Full` passes on full enwik9.
- The scored archive plus decoder package total is below the comparison
  baseline.
- `SUBMISSION_MANIFEST.md` and `SUBMISSION_EMAIL_DRAFT.md` match the latest
  verification output.
- The transport bundle, if used, is independently extracted and verified.

## One-Command Gate

Run the standard local release gate:

```powershell
.\scripts\run_dcv5_release_gate.ps1
```

Run the full pre-submission gate:

```powershell
.\scripts\run_dcv5_release_gate.ps1 -Full
```

The full gate is slower because it decodes full enwik9.

## Current Limits

- The current implementation is still a research-speed compressor.
- Full enwik9 compression is about 11 minutes 25 seconds on this machine.
- Full enwik9 decode verification is about 10 minutes.
- Full decode memory can reach multi-GB working sets.
- There is no official leaderboard status until the LTCB maintainer accepts
  the submission.

## Next Maturity Work

- Reduce full decode peak memory.
- Add block-level parallelism for encode/decode.
- Add cross-platform CI once a Linux runner is available.
- Add broader malformed-archive fuzz coverage.
- Profile hot loops before changing codec math.
