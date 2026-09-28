# UPS Control

UPS Control is the vendor-neutral UPS control and lifecycle model of the Summon
Software Labs Data Center Control Plane. It answers one question for one
uninterruptible power supply at a time:

> **What operating transition may this UPS safely perform now, given its
> protected-load obligations, battery and reserve evidence, bypass and failover
> state, and current authority - and how is command acknowledgement kept separate
> from verified electrical effect?**

It is a C++20 library with a command line tool, runnable examples, a benchmark,
and an installable CMake package. It has no third-party dependencies: the portable
core uses the C++ standard library, and the only operating-system primitives it
reaches for are a bounded file read, a durable write and flush, an atomic rename,
and an exclusive writer lock.

## Systems boundary

This runtime owns the UPS control model and nothing else.

**In scope**

* UPS identity, hardware generation, control epoch and incarnation, state revision.
* Lifecycle states (`commissioning`, `standby`, `in_service`, `maintenance`,
  `degraded`, `faulted`, `isolated`, `decommissioned`) and operating states
  (`online_normal`, `online_battery`, `static_bypass`, `maintenance_bypass`,
  `recharge`, `self_test`, `battery_test`, `faulted`, `isolated`, `retired`,
  and the explicit `unknown`).
* Battery reserve evidence with exact units, provenance, quality, freshness and
  explicit unknown states.
* Protected-load obligations as external, authority-bound references.
* Bypass eligibility and transfer/failover readiness.
* Recharge and discharge permissions and limits as authority inputs.
* Vendor-neutral operation attempts for bypass entry and exit, test transitions,
  isolation, recharge and discharge enablement.
* Adapter acknowledgement, observed state and verified effect as separate stages.
* Explicit, reasoned refusal for unsafe or indeterminate transitions.
* Durable, versioned, integrity-checked persistence of all of the above, an attempt
  journal, authority epochs, and durable obligation bindings.

**Out of scope**

Facility-wide power policy, electrical topology, feed authority, PDU control,
generator control, load shedding, power capacity, and the energy ledger are *not*
owned here. This runtime consumes upstream permissions and obligations and exposes
UPS-specific state and control outcomes. It does not become the facility
controller, and it does not re-derive anything those systems own.

It also does **not** model battery chemistry, runtime-estimation physics, derating,
or vendor behaviour. Reserve evidence is accepted as external evidence with its
provenance attached. Percentages are accepted only on an explicitly defined scale
(basis points, 0..10000, where 10000 is exactly 100 percent) and are never
converted to or from other units by this runtime.

## Design principles

Seven separations run through the whole design:

| Kept separate | From | Because |
| --- | --- | --- |
| stable identity | mutable metadata | renaming a unit must not change what it is |
| hardware generation | state revision | a control-card swap and a state change are different events |
| observation | authority | a value existing is not permission to act |
| planning and eligibility | permission | "this edge would be legal" is not "you may take it" |
| permission | attempted actuation | a grant is not a command |
| acknowledgement | verified effect | a device accepting a request is not the request having happened |
| recovered state | fresh state | a store that survived a restart is not current evidence |

Unknown, unavailable, unsupported, stale, denied, unsafe, and zero are distinct
outcomes with distinct codes. A missing input is never converted into zero or into
a safe default. Authority is never inferred merely because a value exists.

## Architecture

```
include/ups_control/     public headers (installed)
  status, ids, limits, arith, units, refusal, operating, telemetry, battery,
  obligation, authority, command, ups, evaluation, state, store, adapter, engine
src/                     implementation
  detail/                internal, not installed
    codec                little-endian byte writer and reader with declared-length bounds
    crc32c               CRC-32C (Castagnoli)
    digest               FNV-1a 64 change-detection digest
    file_lock            cross-process exclusive writer lock
    platform_io          bounded read, durable write and flush, atomic replace,
                         path validation, canonicalisation, process termination
    serialization        canonical state codec and store envelope codec
    evaluate             the pure transition-evaluation core
    crash                durable-stage crash injection used by the crash tests
tools/ups_cli.cpp        the ups-control administration tool
examples/                four runnable examples
bench/bench_ups.cpp      completed-operation benchmark
tests/                   15 test executables, one per group of proof obligations
downstream/consumer/     an independent out-of-tree find_package consumer
docs/                    store format and authority/transition specification
```

The evaluation core (`src/detail/evaluate.hpp`) is a pure function: it takes a
state, a unit record, a request, and the adapter capability mask, and returns a
report. It performs no I/O, holds no lock, and mutates nothing. The engine owns
persistence, the attempt journal, and the adapter boundary.

### Concurrency model

There is exactly one C++ lock in this runtime and one operating-system lock.

* The engine owns one `std::mutex`. It protects the in-memory authoritative
  snapshot and the attempt journal. Read paths copy an immutable snapshot under the
  mutex and then compute outside it; mutation replaces the snapshot pointer.
* The mutex is **never** held across an adapter call, so no callback and no user
  code runs under it. The adapter is invoked with no internal lock held.
* The mutex **is** held across the durable commit of a mutation, deliberately:
  the persisted state and the in-memory snapshot must advance together, and
  releasing the lock between them would let a second mutation read the older
  snapshot and silently overwrite the first.
* The store takes one operating-system exclusive lock at a read-write open and
  releases it at close. It is never nested inside the engine mutex, and the engine
  mutex is never taken while the writer lock is being acquired.

The result is a single lock level, so there is no lock-order question, no
read-to-write upgrade, and no re-entry through a callback. The engine spawns no
threads and uses no background work; it is single-threaded internally and safe to
call from several threads.

## Authority and fencing

Four independent fences, each a distinct strongly typed value:

* **Hardware generation** fences a command planned against a device that has since
  been swapped or had its control card replaced.
* **Control epoch** fences everything planned before a control-plane handoff.
* **Controller incarnation** fences everything planned by a previous controller
  process within the same epoch.
* **State revision** fences a command planned against a device state that has since
  moved.

Every state-dependent mutation states the authority and the version it was planned
against, and a mismatch is **refused**, never merged. An authority handoff must be
strictly forward: an equal or older epoch, or an equal or older incarnation within
the same epoch, is refused, which is the rollback fence for authority. A grant is
bound to the exact epoch and incarnation it was issued under, so a handoff fences
every grant that predates it.

Idempotency is deliberately outside the fences for retries: the plan fingerprint
covers the device, the command, its parameters and the cited authority, but *not*
the instant, epoch, incarnation or planned revision. A retry of an already
committed attempt therefore returns the prior accepted result **before** any
staleness check runs, which is what makes it survive a lost response. The same key
with a different intent is refused. Retention is bounded and the oldest binding is
evicted first; an evicted key is treated as a new request and is then refused by
the ordinary staleness checks rather than silently re-applied.

## State semantics

**States are not the same as evidence.** The operating state is authoritative
durable state; a battery reading is an observation whose freshness is relative to
an instant. Recovery therefore preserves the operating state and the observation
instant verbatim, and starts the engine at `Recovered` so that no control decision
may be taken until `revalidate` is called with an explicit instant. A unit whose
state basis was only read back from a store is **reported** as `Recovered`
regardless of what it was when it was written, because persistence never carries a
freshness claim across a restart; only telemetry recorded in the current session,
an adopted state, or a verified effect restores a stronger basis in that session. Revalidation never makes an old
observation fresh: it re-evaluates freshness against the instant it is given, so an
observation that has aged past its window becomes stale and stops satisfying any
precondition, with the original observation instant preserved in the refusal
explanation.

**Unknown is a closed source state.** No control transition leaves `unknown`; only
an adopted telemetry observation may replace it, and adoption is not a control
transition.

**Acknowledgement is not effect.** An attempt walks
`planned -> issued -> acknowledged -> observed -> verified`, with
`refused`, `failed` and `unsupported` as terminal alternatives. A terminal
attempt never reopens. An acknowledgement never changes the operating state. A
verified effect is established only from an observation that is newer than the
acknowledgement, fresh at the requested instant, non-contradictory, and one of the
command's documented accepted effect states.

**One device transition at a time.** At most one unresolved attempt may exist per
unit. A second command is refused until the first is verified, failed, or
abandoned. This is enforced structurally: a state in which a unit holds the gate
for anything other than its single unresolved attempt fails validation and cannot
be persisted at all.

The full transition graph, the refusal precedence table, and the safety posture are
specified in [docs/authority-and-transitions.md](docs/authority-and-transitions.md).

## Persistence and recovery

The store is a checksummed head marker plus one generation payload, published by an
explicit protocol whose commit point is the atomic replacement of the head marker:

```
plan -> validate -> reserve generation -> write staging -> flush to the device
     -> read back and verify -> publish the generation payload
     -> atomically replace the head marker      <-- THE COMMIT POINT
     -> retire the superseded payload and residue
```

Nothing is visible at the store path until the head marker is replaced, and nothing
is published in memory unless that replacement succeeded. After a crash, the head
marker names exactly one generation; the payload it names must verify in full, or
the open is refused. A partial generation is never stitched together with a whole
one, and recovery never falls back to an older generation.

The format, the field-by-field layout, the integrity model, and the exact recovery
checks are specified in [docs/persistence-format.md](docs/persistence-format.md).

## Using the library

```cpp
#include "ups_control/engine.hpp"

using namespace ups_control;

EngineOpenOptions options;
options.store.path = "unit.upsstore";
options.store.access = StoreAccess::ReadWrite;
options.store.create_if_missing = true;
options.store.created_at = Tick{1000};
options.store.epoch = ControlEpoch{7};
options.store.incarnation = Incarnation{1};
options.adapter = std::make_shared<SyntheticAdapter>(SyntheticAdapter::Script{});

std::shared_ptr<UpsControlEngine> engine = UpsControlEngine::open(options).value();

// Recovered state is never current evidence until it is revalidated.
RevalidateRequest revalidate;
revalidate.authority = ControlContext{ControlEpoch{7}, Incarnation{1}};
revalidate.now = Tick{1000};
engine->revalidate(revalidate);

// A command is planned against an exact device generation and state revision.
ControlCommand command;
command.authority = ControlContext{ControlEpoch{7}, Incarnation{1}};
command.ref = UpsRef{unit.id, unit.hardware, unit.revision};
command.now = Tick{1001};
command.key = IdempotencyKey::parse("transfer-1").value();
command.kind = CommandKind::EnterStaticBypass;
command.authority_ref = AuthorityRef::parse("authority-ops").value();

const Result<AttemptRecord> attempt = engine->submit(command);
// attempt->phase is acknowledged or issued. It is NEVER verified here.
```

## Command line tool

`ups-control` is a thin shell over the library: every command opens a real store,
runs a real engine call, and prints what the engine returned. Nothing about the
model is reimplemented in the tool.

| Command | Purpose |
| --- | --- |
| `init` | create a store |
| `register` | register a UPS with its identity, hardware generation and lifecycle |
| `status` | authoritative state of one unit, or of every unit |
| `battery` | reserve evidence, its provenance, its age and the assessment |
| `obligations`, `bind-obligation`, `release-obligation` | protected-load obligations |
| `grant`, `revoke-grant` | authority grants |
| `policy` | reserve policy for a unit |
| `observe` | record a telemetry observation, including explicit unknowns |
| `readiness` | what the unit can do now, and every reason it cannot |
| `evaluate` | evaluate a transition without acting on it |
| `run` | evaluate, record and issue a command through the bound adapter |
| `ack`, `verify`, `abandon` | the acknowledgement, verification and terminal stages |
| `attempts`, `replay` | the attempt journal and the idempotent replay |
| `adopt-authority`, `adopt-state`, `set-lifecycle` | forward authority handoff, state adoption, lifecycle moves |
| `revalidate` | revalidate recovered state at an explicit instant |
| `history`, `store-audit`, `verify-store` | the durable commit history and the store audit |

Exit codes are `0` for success or an allowed evaluation, `2` when the model
refused the request, and `1` for a usage, I/O or store error. `--json` selects
machine-readable output.

By default a command is planned against the currently committed epoch,
incarnation, hardware generation and state revision. The `--epoch`,
`--incarnation`, `--hardware` and `--revision` overrides exist so that a
stale-authority refusal can be produced and inspected on purpose.

## Examples

Four runnable examples, each linked against the public target only:

* `example_lifecycle` - register, observe, grant, command, observe the effect,
  verify, and replay the same request idempotently.
* `example_bypass_refusal` - a life-safety protected-load obligation refuses a
  maintenance bypass transfer that would drop protection, and the refusal is lifted
  only by an authority-bound release.
* `example_stale_evidence` - an unknown reserve, a known reserve below the floor,
  the same evidence aged past its window, and a request fenced by a superseded
  epoch: four different refusals with four different codes.
* `example_restart` - an unresolved attempt survives a restart, is never reissued
  as new, blocks a different key, and returns the same attempt for the same key.

## Building

Requirements: CMake 3.21 or newer and a C++20 compiler. The exercised platform is
Windows with MSVC (Visual Studio 2022, toolset 19.44). No third-party dependency is
required or downloaded.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

First-party warnings are errors: MSVC builds with `/W4 /WX /permissive-`, other
compilers with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
-Wnon-virtual-dtor -Wcast-qual -Wdouble-promotion -Werror`.

## Installing and consuming the package

```sh
cmake --install build --prefix /some/prefix
```

```cmake
find_package(UpsControl 1.0 REQUIRED)
target_link_libraries(my_service PRIVATE UpsControl::ups_control)
```

`downstream/consumer` is an independent out-of-tree project that consumes the
installed package exactly this way. It is configured separately with
`CMAKE_PREFIX_PATH` pointing at an install prefix and is never part of this build
tree.

## Validation

Everything below was produced by running this repository on Windows with MSVC
19.44. [VALIDATION.md](VALIDATION.md) carries the full evidence: exact commands,
per-executable test inventories, the adversarial hardening table, the benchmark
methodology, and the remaining limitations.

| Configuration | Result |
| --- | --- |
| Release, `/W4 /WX /permissive-` | clean, zero warnings |
| Debug, `/W4 /WX /permissive-` | clean, zero warnings; full suite passes with MSVC iterator debugging active |
| Release + AddressSanitizer | clean, zero warnings; full suite passes with no sanitizer diagnostic |

```
ctest --test-dir build-rel -C Release
100% tests passed, 0 tests failed out of 15
```

15 test executables, 295 test cases, structured by proof obligation rather than by
target count. There is no test timeout anywhere in this repository. The suite
includes real multiprocess tests (writer exclusion, process-death release of writer
authority, disjoint lock tenures), real process death at every durable stage of the
commit protocol with both the journal and the adapter's effects log asserted at each
stage, deterministic concurrency and race tests, seeded randomized state-machine
walks checked against an independent model, and 34 adversarial parser, store and path
tests.

Benchmarks measure completed operations only, including every mandatory step through
the head-marker commit. On this host a completed store open is 1 244 ops/s, a
completed durable telemetry commit is 93.7 ops/s, and a completed control attempt
(two durable commits plus the adapter issuance) is 44.9 ops/s. The store path is
labelled `REAL` and the device interaction `SYNTHETIC`, because the deterministic
simulator is bound; hardware validation is `UNSUPPORTED`, because no UPS hardware is
attached to this host.

The repository has no third-party dependency, no network access, no telemetry, and no
mechanism to reach a device other than the `UpsAdapter` interface a deployment binds
explicitly.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
