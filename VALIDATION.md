# Validation

Everything below was produced by running the software in this repository on the
host described in **Environment**. Nothing here is projected, estimated, or
inferred from the design. Where a result could not be obtained, it is stated as a
limitation rather than replaced by a weaker claim.

## Environment

| | |
| --- | --- |
| Operating system | Windows 10.0.26200 |
| Compiler | MSVC 19.44.35222.0 (Visual Studio 2022 Build Tools, toolset 14.44.35207) |
| Generator | Visual Studio 17 2022, x64 |
| CMake | 4.3.2 |
| Logical processors | 16 |
| Third-party dependencies | none |

## Build configurations

Every configuration is built with the first-party warning set as errors:
`/W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc`.

| Configuration | Result |
| --- | --- |
| Release | clean, zero warnings |
| Debug | clean, zero warnings; the full test suite passes with MSVC iterator debugging active |
| Release + AddressSanitizer (`/fsanitize=address`) | clean, zero warnings; the full test suite passes |
| Release + library only, incremental | clean |

No warning is suppressed anywhere in the build. No `/wd` flag, no warning pragma,
and no per-file warning override exists in this repository.

## Automated tests

15 test executables, one per group of proof obligations, 295 test cases. There is
no test timeout anywhere: no CTest `TIMEOUT` property, no timeout wrapper, no
watchdog process, and no forced termination that is classified as a pass. A test
that did not finish would be a defect to diagnose.

```
ctest --test-dir build-rel -C Release
100% tests passed, 0 tests failed out of 15     (65.2 s)
```

| Executable | Cases | What it proves |
| --- | --- | --- |
| `uc_test_types` | 26 | identifiers, UTF-8 labels, typed ids and fingerprints, exact unit conversions, checked integer arithmetic at the boundaries |
| `uc_test_graph` | 26 | the operating and lifecycle graphs against a hand-written reference table, closure, terminal states, readiness and evaluation agreeing on the same capability set |
| `uc_test_evidence` | 24 | freshness windows, unknown-versus-zero semantics, contradiction detection, telemetry validation |
| `uc_test_obligations` | 27 | a protected-load obligation is never silently dropped, including a lapsed one; release and re-assertion semantics |
| `uc_test_authority` | 23 | grant precedence, scope substitution refusal, revocation, expiry, epoch and incarnation fencing, handoff monotonicity |
| `uc_test_transitions` | 22 | deterministic refusal precedence, the documented command vocabulary, relabelling refusal, unsupported handling, per-precondition refusals, evaluation purity |
| `uc_test_attempts` | 11 | acknowledgement, observation and verification as separate stages; idempotent replay before any staleness check; bounded retention and eviction |
| `uc_test_persistence` | 35 | the artifact format, canonical determinism, every envelope rejection, structural rejection after valid checksums, generation floor, path binding, writer exclusion, 50 open/close cycles with residue assertions |
| `uc_test_recovery` | 7 | recovered state is never fresh: the basis is reported as `Recovered`, evidence ages against the original instant, an unresolved attempt is never reissued, epochs and bindings survive |
| `uc_test_crash` | 7 | real process death at every durable stage of the commit protocol, with the journal **and** the adapter's effects log asserted at each stage |
| `uc_test_concurrency` | 5 | one winner per device gate, twenty contention rounds, authority handoff racing with submission, readers never observing a torn state, the adapter never seeing two in-flight attempts |
| `uc_test_multiprocess` | 5 | writer exclusion across real processes, process-death release of writer authority, disjoint lock tenures, superseded-incarnation fencing |
| `uc_test_property` | 3 | seeded randomized state-machine walks against an independent model, with a reproducible transcript digest |
| `uc_test_adversarial` | 34 | declared-length boundaries on both sides, absurd sizes, malformed and truncated artifacts, path attacks, Unicode, symlinks and reparse points, reordered events, integer boundaries, tiny limits |
| `uc_test_cli` | 40 | the full administration path through real processes and real stores, including exit codes, JSON structure and every refusal reached from the tool |

Configuration matrices:

```
Release : 15/15 passed, 295 cases           (65.2 s)
Debug   : 15/15 passed                       (81.2 s)
ASan    : 15/15 passed                       (112.7 s)
```

## Real multiprocess and crash results

These are real operating-system processes, not threads pretending to be processes,
and real operating-system termination, not a simulated crash.

* **Writer exclusion.** A second read-write open of the same store, in a second
  process, fails with `LockConflict` while the first holds it; a read-only open
  succeeds at the same instant and observes one whole verified generation.
* **Process-death release.** A writer process is terminated at the operating-system
  level. A read-write open then succeeds **with no recovery step**, and the store's
  canonical digest is unchanged, which is the proof that the operating system
  released the lock.
* **Disjoint tenures.** Several probe processes contend for the same store; the
  intervals during which each holds it never overlap, established from the ready
  and release files rather than from timing assumptions.
* **Crash at every durable stage.** The probe is started with a durable-stage crash
  injection and terminates itself with exit code `0x00C0DE01` at the named stage of
  the named commit. For each stage the parent then asserts:
  * the store opens and verifies in full, or is refused with a documented status;
    it is never partially adopted;
  * the committed generation is exactly the pre-crash generation or the pre-crash
    generation plus one, and never anything else;
  * no `.staging-` or `.head-tmp` residue remains after the reopen;
  * **before** the durable plan commit there is no attempt and the adapter's effects
    log is empty;
  * **after** the durable plan commit the attempt is in the journal, the effects log
    is *still* empty (the commit point was reached but the adapter had not been
    called), the device gate is held, and a retry with the same idempotency key
    returns the same attempt and still does not invoke the adapter;
  * after the acknowledgement commit the effects log holds exactly one line, so the
    device was addressed exactly once.
* **Fencing across processes.** A probe citing a superseded incarnation is refused
  and leaves the store unchanged.

## Randomized and reference-model results

* A seeded walk over the operating-state graph (seeds `0x5EED0001`–`0x5EED0003`)
  submits both legal and illegal edges and asserts that only legal edges with every
  other precondition met are accepted.
* A seeded mutation walk of 200 steps per seed (seeds `0xC0FFEE01`–`0xC0FFEE03`),
  with a reopen in the middle, compares the live store against an independent model
  after every step: generation advancing by exactly one per mutation, revisions
  non-decreasing, at most one unresolved attempt per unit with the gate agreeing
  exactly, no binding protective obligation dropped without an explicit release, and
  the number of adapter issuances matching the number of attempts that reached
  issuance.
* A transcript digest proves the same seed reproduces the same accept and refuse
  sequence exactly.
* The graph, the readiness predicates and the protected-impact rule are each checked
  against a second, hand-written statement of the rule in the test itself, so a
  change to either side fails the suite.

## AddressSanitizer

`/fsanitize=address` on MSVC 19.44 built the entire suite, and all 15 executables
pass under it with no sanitizer diagnostic. The sanitizer was confirmed active in
the same shell with `ASAN_OPTIONS=verbosity=1`, which prints
`AddressSanitizer Init done`.

Limitation: MSVC's AddressSanitizer does not support leak detection on Windows
(`AddressSanitizer: detect_leaks is not supported on this platform`), so the ASan
run proves memory-error freedom but **not** leak freedom. Leak checking was not
substituted with a different tool.

## Install, export and downstream consumption

```
cmake --install build-rel --config Release --prefix <prefix>
```

installs the library, the public headers, the generated version header, the
namespace-qualified CMake package (`UpsControlConfig.cmake`,
`UpsControlConfigVersion.cmake`, `UpsControlTargets.cmake`), the CLI, and the
licence and notice.

`downstream/consumer` is an independent CMake project that is **not** part of this
build tree. It is configured separately with `CMAKE_PREFIX_PATH` pointing at the
install prefix and consumes the package with
`find_package(UpsControl 1.0 REQUIRED)` and
`target_link_libraries(consumer PRIVATE UpsControl::ups_control)`. It then runs a
complete lifecycle: create a durable store, register a unit, record telemetry, issue
a grant, evaluate a transition, submit it through the synthetic adapter, verify the
effect, close, reopen, and confirm that the reopened store's canonical digest and
generation match what was committed.

```
consumer: ups_control 1.0.0
consumer: generation=9 canonical_digest=2605192002058532539
consumer: recovered_lifecycle=recovered operating=static_bypass basis=recovered
consumer: device_evidence=SYNTHETIC
consumer: ok
```

`basis=recovered` is the documented rule in action: the unit was `verified` when the
store was written, and a session that only read the state back reports `Recovered`
until telemetry, an adopted state, or a verified effect establishes a stronger basis
in that session. `UpsStore::read_file` still returns the committed field verbatim,
which is what the field-by-field round-trip contract requires.

```
fresh clone, configure -> build -> ctest -> install -> downstream -> examples
configure_exit=0  build_exit=0  ctest_exit=0
100% tests passed, 0 tests failed out of 15     (65.0 s)
install_exit=0    downstream_exit=0
example_lifecycle exit=0  example_bypass_refusal exit=0
example_stale_evidence exit=0  example_restart exit=0
compiler diagnostics: 0
```

The fresh clone was taken from the pushed commit and built outside the source tree.
The only diagnostics in its build log were 26 occurrences of the MSBuild advisory
that an intermediate directory should not reside under the temporary directory, which
is a property of where the clone was placed rather than of the code.

The benchmark was also run from the fresh clone at 100 repetitions: 952.5 completed
store opens per second, 94.8 durable telemetry commits per second, and 45.9 completed
control attempts per second, with `cleanup_state=clean`. Those differ from the
300-repetition numbers below only by run-to-run variance and are not presented as a
comparison.
## Benchmarks

Methodology. Every measurement times a whole completed operation, including all of
its mandatory work: request validation, deterministic evaluation, canonical
encoding, the staging write, the device flush, the read-back verification, the
atomic generation publish, the head-marker commit, and residue cleanup. Nothing
times enqueue or submission latency. Results are the arithmetic mean over all
completed operations in a run of 300 repetitions at a stated workload scale, and
the scaffolding that restores the identical starting position between control
attempts is reported separately rather than absorbed into the attempt number. The
benchmark verifies its own output state, removes every store it created, and reports
the cleanup state; no residue is left behind.

Workload scale: one registered unit with one obligation and one grant.

| Operation | Evidence | Completed ops | ns/op | ops/s |
| --- | --- | --- | --- | --- |
| `engine_open_with_full_store_verification` | REAL | 300 | 803 799 | 1 244.1 |
| `durable_telemetry_commit` | REAL | 300 | 10 677 700 | 93.7 |
| `completed_control_attempt` | REAL (store) + SYNTHETIC (device) | 300 | 22 251 000 | 44.9 |
| `scaffolding_between_control_attempts` | REAL, not part of the attempt | 300 | 56 010 900 | 17.9 |

Labels:

* **REAL** — the durable store path: real files, real device flushes, real atomic
  renames on the host filesystem, measured in this environment.
* **SYNTHETIC** — the device interaction. The deterministic simulator is bound, so
  no hardware behaviour is claimed for any part of these numbers.
* **UNSUPPORTED** — hardware validation. No UPS hardware is attached to this host, so
  no hardware result exists and none is claimed.

A completed control attempt costs two durable commits (the plan and the
acknowledgement) plus the adapter issuance, which is consistent with the telemetry
number: a single commit is roughly 10.7 ms here, and the attempt is roughly two of
them plus evaluation and encoding.

These numbers were produced by the current implementation only. No before/after
pair is published, because no controlled alternating comparison was run.

## Adversarial hardening pass

After the suite first passed, the implementation was attacked separately from the
tests. Everything below was refused; nothing was adopted, silently repaired, or
clamped.

| Attack | Result |
| --- | --- |
| Store replaced by an older but fully valid copy of itself | adopted without a floor (it *is* a valid store), refused with `stale_generation` when a generation floor is recorded |
| Store replaced by an older head whose payload had been retired | refused with `not_found`; nothing partial was stitched together |
| Head marker replaced by a symbolic link | refused with `invalid_argument` |
| Generation payload replaced by a symbolic link | refused with `invalid_argument` before any content was examined |
| Symbolic link pre-created at a staging name | never written through; the write target is validated first |
| Wrong expected store identity | refused with `stale_authority` |
| `--limit 99999999999999999999` | refused as an out-of-range integer, never clamped |
| `--reserve-value` beyond the integer range | refused as an out-of-range integer |
| Unknown unit token (`furlongs`) | refused, with the accepted set named |
| Basis points above 10000 | refused with the valid range named |
| Unknown command name | refused; never mapped onto a neighbouring operation |
| A `--now` that moves backwards | refused with `precondition_failed` |
| A label encoding a code point above U+10FFFF | refused as invalid UTF-8 |
| A store path naming a reserved device | refused |
| Absurd declared counts and sizes | refused from the declared length before any allocation |

## Defects found and fixed during this work

Each was found by an independent check, reproduced, fixed in the library, and is now
pinned by at least one test.

1. **A code point above U+10FFFF was accepted as valid UTF-8** when encoded with a
   `0xF4` lead byte, because the maximum was checked against the lead byte only.
   `F4 90 80 80` (U+110000) and `F4 BF BF BF` (U+13FFFF) were accepted. Fixed by
   re-checking the maximum after the continuation bytes are folded in; `F4 8F BF BF`
   (U+10FFFF, the true maximum) is still accepted, so the fix is pinned from both
   sides.
2. **`static_bypass_ready` was weaker than the command it describes.** It required
   four of the five tri-states that `EnterStaticBypass` requires, so readiness could
   report ready while the same request was refused with `BatteryNotReady`. Fixed in
   the fail-closed direction: readiness now requires exactly the set the command
   requires, and a test enumerates all 3^5 combinations and asserts the agreement.
3. **A request citing a state revision ahead of the unit's could not be recorded.**
   The structural validation bounded the attempt's planned revision above by the unit
   revision, so such a request produced an unrecorded `InvariantViolation` instead of
   the documented reasoned refusal, leaving no journal entry and no idempotency
   binding. Fixed by removing that bound: an attempt record is a durable fact about a
   request, and a request that cited a revision the unit never had is exactly the
   request whose refusal must be recorded.
4. **Recovery preserved the pre-restart freshness claim.** After a reopen, the state
   basis still read `Verified`, contradicting the documented rule that persistence
   never carries a freshness claim across a restart. Fixed by making the reported
   basis session-relative: a unit that was only read back from a store is reported as
   `Recovered` until telemetry, an adopted state, or a verified effect establishes a
   stronger basis in that session. The store layer still returns the committed field
   verbatim, which is what the field-by-field round-trip contract requires.
5. **`checked_mul` evaluated a signed overflow before checking it**, which is
   undefined behaviour that happened to produce the documented refusal on MSVC.
   Rewritten to bound the product by division before forming it.
   `checked_div_exact(INT64_MIN, -1)` had the same shape and is now refused before
   the operator is evaluated.
6. **`checked_sub` refused the one representable subtraction whose right operand is
   `INT64_MIN`** (`INT64_MIN - INT64_MIN`), reporting an overflow that does not exist.
7. **A code point above U+10FFFF in a *payload's recorded path*, and a payload reached
   through a symbolic link, were accepted.** The recorded path is untrusted input like
   every other field and is now validated before it is used, and both store files are
   held to the same path rules.
8. **The two views of one unit disagreed about the state basis**: the single-unit view
   reported `Recovered` while the listing reported the raw stored value. Fixed by
   applying the same reporting rule on every read path.
9. **A revalidation commit could move the recorded revalidation instant backwards.**
   Fixed: the recorded instant is only ever advanced.
10. **The test harness could not pass an argument containing a space on Windows**,
    because `cmd /c` strips the outermost quotes of a line holding more than two.
    Fixed by wrapping the whole command line in one more pair of quotes, which is what
    allowed the CLI escaping cases to be exercised for real.

## Limitations

* **No hardware validation.** No uninterruptible power supply is attached to this
  host. Every control attempt in this repository is carried out through the
  deterministic synthetic adapter and is labelled `SYNTHETIC` wherever it is
  reported. Nothing here is hardware proof, and no vendor protocol is implemented.
* **No vendor behaviour is modelled.** Battery chemistry, runtime estimation,
  derating, and vendor command semantics are outside the systems boundary. Reserve
  evidence is accepted as external evidence with its provenance attached, and the
  three reserve classes are never converted into one another.
* **The POSIX branches are written but unverified.** `flock`, `fsync`, `rename`,
  `posix_spawn`, and `_exit` paths exist and are structurally complete, but this
  environment is Windows/MSVC, so they were compiled out of every run reported here.
  Portability is not claimed as runtime proof.
* **Leak detection is unavailable** with MSVC's AddressSanitizer on Windows, as noted
  above.
* **The `Unsupported` attempt phase is reached deliberately, not automatically.** An
  adapter capability gap is refused during evaluation, before the plan is committed
  and before the adapter is contacted, which is the safer order. The terminal
  `Unsupported` phase is reached when an adapter reports a command as unimplemented
  after issuance, which the library supports and the CLI exposes through
  `ack --outcome unsupported`.
* **The CLI takes narrow (`char**`) arguments.** A non-ASCII store path passed as a
  command line argument depends on the active code page; the library API handles
  arbitrary Unicode paths correctly, which the adversarial suite proves directly.
* **`RevalidationReport` deliberately counts evidence staleness separately** from an
  undecided reserve, and an undecided reserve separately from one known to be below
  the floor. The three counters are documented on the struct.
* **Idempotency retention is bounded** by `ResourceLimits::max_idempotency_records`
  and the oldest binding is evicted first. An evicted key is treated as a new request
  and is then refused by the ordinary staleness checks rather than silently
  re-applied. This is a documented contract, not an accident.
* **Residue removal is best effort.** A file that cannot be removed is reported, never
  silently ignored into a success.
