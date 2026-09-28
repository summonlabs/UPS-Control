# Authority, transitions and refusals

This document is the normative description of what UPS Control will and will not
do, and of the exact order in which it decides. A change to any table here is a
change to the public contract.

## The core question

> What operating transition may this UPS safely perform now, given its
> protected-load obligations, battery and reserve evidence, bypass and failover
> state, and current authority - and how is command acknowledgement kept separate
> from verified electrical effect?

## The seven separations

| Kept separate | From | Because |
| --- | --- | --- |
| stable identity | mutable metadata | a label change must not change identity |
| hardware generation | state revision | a control-card swap and a state change are different events |
| observation | authority | a value existing is not permission to act |
| planning and eligibility | permission | "this edge would be legal" is not "you may take it" |
| permission | attempted actuation | a grant is not a command |
| acknowledgement | verified effect | a device accepting a request is not the request having happened |
| recovered state | fresh state | a store that survived a restart is not current evidence |

## Identity and generation vocabulary

* `UpsId` - which device.
* `HardwareGeneration` - which physical device generation. A swap advances it.
* `ControlEpoch` / `Incarnation` - which control authority is in charge, and which
  controller process within it.
* `StateRevision` - the monotonic revision of one device's authoritative state.
* `SourceRevision` - the reporting device's own sequence number.
* `ObligationRevision` / `GrantRevision` - the revision of one binding.
* `StoreGeneration` - the durable store generation.
* `AttemptId` - one control attempt.
* `Tick` - a logical instant supplied by the caller. This runtime never reads a
  wall clock for an authoritative decision.

These are distinct C++ types. None of them converts implicitly to another, and the
store format encodes each in its own field.

## State

### Lifecycle (administrative, durable)

`Commissioning, Standby, InService, Maintenance, Degraded, Faulted, Isolated,
Decommissioned`

The complete successor set of every lifecycle state, which is exactly what
`is_legal_lifecycle_transition` implements. No other pair is an edge, and no state
is its own successor.

| From | Legal successors |
| --- | --- |
| `Commissioning` | `Standby`, `InService`, `Decommissioned` |
| `Standby` | `InService`, `Maintenance`, `Faulted`, `Decommissioned` |
| `InService` | `Maintenance`, `Degraded`, `Faulted`, `Standby`, `Isolated`, `Decommissioned` |
| `Maintenance` | `InService`, `Standby`, `Isolated`, `Faulted`, `Decommissioned` |
| `Degraded` | `InService`, `Maintenance`, `Faulted`, `Isolated`, `Decommissioned` |
| `Faulted` | `InService`, `Degraded`, `Maintenance`, `Isolated`, `Decommissioned` |
| `Isolated` | `Maintenance`, `Standby`, `Decommissioned` |
| `Decommissioned` | - (terminal) |

`lifecycle_permits_control` returns true only for `Standby`, `InService` and
`Degraded`.

### Operating state (physical, observable)

`Unknown, Offline, Standby, OnlineNormal, OnlineBattery, StaticBypass,
MaintenanceBypass, Recharge, SelfTest, BatteryTest, Faulted, Isolated, Retired`

The complete successor set of every operating state, which is exactly what
`is_legal_operating_transition` implements. No other pair is an edge, and no state
is its own successor.

| From | Legal control successors |
| --- | --- |
| `Unknown` | - (closed; only an adopted observation replaces it) |
| `Offline` | `Standby`, `Isolated`, `Retired` |
| `Standby` | `OnlineNormal`, `Offline`, `Faulted`, `Isolated`, `Retired` |
| `OnlineNormal` | `OnlineBattery`, `StaticBypass`, `MaintenanceBypass`, `SelfTest`, `BatteryTest`, `Recharge`, `Standby`, `Faulted`, `Isolated` |
| `OnlineBattery` | `OnlineNormal`, `Recharge`, `StaticBypass`, `Standby`, `Faulted`, `Isolated` |
| `StaticBypass` | `OnlineNormal`, `MaintenanceBypass`, `Standby`, `Faulted`, `Isolated` |
| `MaintenanceBypass` | `StaticBypass`, `OnlineNormal`, `Standby`, `Isolated`, `Retired` |
| `Recharge` | `OnlineNormal`, `OnlineBattery`, `Standby`, `Faulted`, `Isolated` |
| `SelfTest` | `OnlineNormal`, `OnlineBattery`, `StaticBypass`, `Faulted` |
| `BatteryTest` | `OnlineNormal`, `Recharge`, `OnlineBattery`, `StaticBypass`, `Faulted` |
| `Faulted` | `Isolated`, `Offline`, `Retired` |
| `Isolated` | `Standby`, `Offline`, `Retired` |
| `Retired` | - (terminal) |

Three properties matter:

* `Unknown` is a **closed source state**. No control transition leaves it. Only an
  adopted telemetry observation may replace it, and adoption is not a control
  transition.
* `Retired` is terminal.
* Control out of `Faulted` reaches only `Isolated`, `Offline` and `Retired`.
  Returning a faulted unit to a healthy state is an *observation*, not a command.

The transition graph constrains control. It does not constrain observation: a
device may jump from `OnlineNormal` to `Faulted` and the runtime records exactly
that.

### State basis

`Recovered, Observed, CommandedUnverified, Verified`

The basis says how the current operating state was established. An
acknowledgement never establishes an operating state. A unit whose state basis was
only read back from a store is **reported** as `Recovered` for the whole session,
because persistence must never carry a freshness claim across a restart. Only
telemetry recorded in this session, an adopted state, or a verified effect restores
a stronger reported basis, and the basis is a session-relative report rather than a
durable fact.

## Bypass and transfer readiness

`TransferStatus` is five tri-state values: bypass availability, bypass
qualification, output synchronisation, device transfer readiness, and battery
readiness. `Bool3::Unknown` is not `False` and never satisfies a precondition. A
device that does not report a capability therefore blocks the transfer that
depends on it, with a refusal naming the missing capability.

Readiness and evaluation require the **same** set of capabilities for the same
operation, so they never disagree:

| Operation | Required tri-states |
| --- | --- |
| `EnterStaticBypass` / `static_bypass_ready` | availability, qualification, synchronisation, transfer readiness, battery readiness |
| `EnterMaintenanceBypass` / `maintenance_bypass_ready` | availability, qualification |
| `LeaveStaticBypass`, `LeaveMaintenanceBypass` | transfer readiness |

The static bypass transfer requires battery readiness because the transfer window
is carried by the battery if the input fails part-way through; a battery that is
not reported ready is not assumed ready.

## Lifecycle and operating state must agree

The two state axes are independent, but they are not unconstrained. A pair is
consistent when:

| Lifecycle | Operating states that are consistent with it |
| --- | --- |
| `Commissioning` | `Unknown`, `Offline`, `Standby`, `Isolated` |
| `Standby` | anything except `Retired` |
| `InService` | anything except `Retired` |
| `Maintenance` | anything except `Retired`; a unit under maintenance may still be carrying the load on a bypass path, or may be isolated |
| `Degraded` | anything except `Retired` |
| `Faulted` | anything except `Retired` |
| `Isolated` | `Isolated`, `Offline`, `Retired` |
| `Decommissioned` | `Retired`, `Offline` |

An inconsistent pair is refused with `StatusCode::Conflict`, and the refusal says
which axis has to move first. In particular a decommissioning request against an
online unit is refused rather than silently decommissioning an energized asset, and
a telemetry report that asserts an inconsistent pair is recorded as contradictory
and never adopted.

## Battery reserve evidence

Reserve is reported in one of three comparable classes:

* **energy** - milliwatt-hours, watt-hours, kilowatt-hours; exactly convertible to
  each other;
* **runtime** - seconds, the device's or operator's estimate at the present load;
* **nameplate fraction** - basis points on a fixed 0..10000 scale where 10000 is
  exactly 100 percent.

This runtime does **not** convert between classes. It does not model battery
chemistry, discharge physics, derating, or runtime estimation. A requirement
stated in seconds cannot be satisfied by evidence stated in watt-hours; the
assessment returns `Indeterminate` with `UnitMismatch` rather than inventing a
conversion.

`ReserveObservation` is either a known quantity with a stated evidence quality, or
an explicit unknown with a stated reason. **Unknown is not zero.** An unknown
reserve cannot satisfy a reserve floor, and it is never read as a small reserve:
the assessment distinguishes `Insufficient` (known and below the floor) from
`Indeterminate` (cannot be decided).

Freshness is evaluated against an explicit instant. An observation exactly
`max_age` old is still fresh; one tick older is stale. An observation ahead of the
requested instant is fresh only within the tolerated future skew, and beyond that
it is a future observation, which is never fresh.

### Contradictory observations

A telemetry report is contradictory when it asserts two things that cannot both be
true. The complete rule set, in the order it is evaluated:

1. a fault is asserted while the unit reports a protecting state, other than
   `online_battery`;
2. the unit reports `faulted` while no fault is asserted;
3. the unit is on battery while the battery reports charging;
4. the unit is recharging while the battery reports discharging;
5. the unit is on maintenance bypass while no bypass path is reported;
6. the unit is on battery while the reported reserve is exactly zero;
7. the reserve is reported as known with an unknown evidence quality;
8. charging is reported while charging is reported inhibited;
9. discharging is reported while discharging is reported inhibited;
10. the unit is discharging while the discharge path is reported disabled;
11. the unit is recharging while the recharge path is reported disabled.

Two exemptions are deliberate and are part of the contract rather than gaps:

* rule 1 exempts `online_battery`: a unit that has transferred to battery because
  of a fault is in a normal degraded operating condition, not a contradiction;
* no rule fires for the `unknown` operating state. Nothing is claimed about the
  electrical path, so there is nothing for a fault assertion to contradict.

A contradictory report is **recorded** with its explanation and the identity of the
evidence, and it never moves the authoritative operating state and never satisfies
a precondition. A reserve assessment over a contradictory observation returns
`Indeterminate` with the `Contradictory` reason.

## Protected-load obligations

An obligation is an external, authority-bound reference: which load, how critical,
what protection is required, which authority asserted it, under which epoch, from
when, and until when.

* `MustRemainProtected` obligations block any transition that removes UPS
  protection from their load, unless the obligation itself was explicitly released
  or suspended. Citing an authority is **not** enough.
* `MayBeInterruptedWithAuthority` obligations are covered when the request cites
  the asserting authority itself.
* `Informational` obligations carry no protection requirement.
* An obligation whose declared window has lapsed is **unresolved, not void**. Time
  passing never releases protection. A lapsed obligation blocks a
  protection-dropping transition with `ObligationExpired` until it is released or
  re-asserted.
* A release is permanent and auditable: re-binding a released reference is refused,
  and a release records who released it, when, and why.

Only two commands drop protection: `EnterMaintenanceBypass` and `IsolateOutput`.
Both are checked against every protective obligation bound to the unit.

## Authority

An authority grant is policy input. It is bound to one scope, one UPS, one epoch
and one incarnation, and optionally to an expiry instant and a reserve floor.

* Telemetry never creates, widens, or renews a grant. A device reporting the
  recharge path enabled does not authorize enabling recharge.
* A grant is bound to the exact epoch and incarnation it was issued under. Any
  handoff fences every grant issued before it; the grants must be re-issued.
* A cited grant whose scope differs from the required scope is refused as
  out-of-scope. A neighbouring scope is never substituted.
* An explicit denial is recorded, so "denied" and "no authority" are
  distinguishable.
* Moving the store to a new epoch or incarnation must be strictly forward. An
  equal or older pair is refused, which is the rollback fence for authority.

## Evaluation precedence

Evaluation collects every violation it finds and reports all of them, ordered by
precedence. The **primary** refusal is the one with the lowest numeric
`RefusalCode`, so the same invalid request always produces the same primary error
regardless of the order in which its violations are discovered.

The precedence, highest first:

| # | Code | Meaning |
| --- | --- | --- |
| 1 | `UnknownUps` | no such unit |
| 2 | `ResourceLimitExceeded` | a configured bound is exceeded |
| 3 | `StaleHardwareGeneration` | planned against a superseded device generation |
| 4 | `StaleEpoch` | planned against a superseded control epoch |
| 5 | `StaleIncarnation` | planned against a superseded controller incarnation |
| 6 | `StaleRevision` | planned against a superseded state revision |
| 7 | `UnsupportedCommand` | not a documented operation |
| 8 | `AdapterUnavailable` | no adapter is bound |
| 9 | `CapabilityUnsupported` | the adapter does not implement it |
| 10 | `StateNotRevalidated` | recovered state has not been revalidated |
| 11 | `OperatingStateUnknown` | no validated observation establishes the state |
| 12 | `IllegalOperatingTransition` | not an edge of the operating graph |
| 13 | `LifecycleForbidsControl` | the lifecycle state does not permit control |
| 14 | `LifecycleTransitionIllegal` | not an edge of the lifecycle graph |
| 15 | `StateBasisUnverified` | a prior command was acknowledged and never verified |
| 16 | `ObligationUnreleased` | a binding protected obligation would be dropped |
| 17 | `ObligationExpired` | a lapsed obligation would be dropped |
| 18 | `BypassNotAvailable` | no bypass path is established |
| 19 | `BypassNotQualified` | the bypass path is not qualified |
| 20 | `OutputNotSynchronized` | the output is not synchronised |
| 21 | `TransferNotReady` | the device does not assert transfer readiness |
| 22 | `BatteryNotReady` | the battery is not reported ready |
| 23 | `ReserveRequirementMissing` | a reserve-dependent operation has no declared floor |
| 24 | `ReserveEvidenceMissing` | no reserve evidence exists |
| 25 | `ReserveEvidenceStale` | reserve evidence is outside its window |
| 26 | `ReserveIndeterminate` | reserve evidence cannot decide the precondition |
| 27 | `ReserveInsufficient` | known reserve below the floor |
| 28 | `AuthorityMissing` | no authority covers the operation |
| 29 | `AuthorityDenied` | an authority explicitly denies it |
| 30 | `AuthorityExpired` | the authority's window has lapsed |
| 31 | `AuthorityRevoked` | the authority was revoked |
| 32 | `AuthorityFenced` | the authority belongs to a superseded epoch or incarnation |
| 33 | `TransitionInProgress` | another unresolved attempt holds the single-device gate |
| 34 | `IdempotencyConflict` | the key is bound to a different intent |
| 35 | `ContradictoryObservation` | the observation contradicts itself |
| 36 | `InternalInvariant` | an internal consistency guarantee was violated |

A request that violates nothing is allowed; the report carries the full finding
list and a non-blocking warning list (for example a recovered state basis, a stale
observation that the command does not depend on, or a grant that declares no
expiry).

## The control attempt lifecycle

```
Planned ---- refusal recorded ----> Refused        (terminal)
   |
   +--- handed to adapter ---------> Issued --------+
                                       |            |
                                       v            v
                                  Acknowledged   Unsupported   (terminal)
                                       |            |
                                       v            v
                                   Observed       Failed       (terminal)
                                       |
                                       v
                                   Verified                    (terminal)
```

A terminal attempt never reopens. The durable plan is committed **before** the
adapter is invoked, and the acknowledgement is committed after it. A crash between
the two leaves the attempt unresolved in the journal with the device possibly
addressed; it is recovered as it was and is **never reissued as new**. Resolving it
requires either a verification against a fresh observation that is newer than the
issuance, or an explicit abandonment.

The single-device ordering gate allows at most one unresolved attempt per unit. A
second command for that unit is refused with `TransitionInProgress` until the
first is verified, failed, or abandoned. This is enforced structurally: a state in
which a unit holds the gate for anything other than its single unresolved attempt
fails validation and cannot be persisted at all.

## Idempotency

The idempotency key is the identity of one intent. The plan fingerprint covers the
device, the command, its parameters, and the cited authority - deliberately **not**
the instant, epoch, incarnation, or planned revision, because a retry after a lost
response naturally re-reads those fences.

Consequences:

* A retry of an already committed attempt returns the prior result **before** any
  staleness check runs, so a retry is not rejected by a generation that moved on
  while the response was lost.
* The same key with a different intent is refused with `IdempotencyConflict`.
* Retention is bounded by `ResourceLimits::max_idempotency_records` and the oldest
  binding is evicted first. An evicted key is no longer recognized, so a retry
  carrying it is treated as a new request and is then refused by the ordinary
  staleness checks rather than silently re-applied.

## Safety posture

* Nothing here ever claims physical actuation because a command object was created
  or an adapter acknowledged receipt. Authorization, issued command,
  acknowledgement, observed effect, and verified effect are separate recorded
  states.
* Safety interlocks and protected obligations fail closed. A missing, stale, or
  contradictory input is never promoted to permission.
* Unknown, unavailable, unsupported, stale, denied, unsafe, and zero are distinct
  outcomes with distinct codes.
* The only way this runtime can affect a device is through the `UpsAdapter`
  boundary. There is no implicit adapter and no hidden autonomous control path.
* Every attempt records its evidence class. An attempt carried out through the
  deterministic simulator is labelled `SYNTHETIC` everywhere it is reported, and
  no hardware behaviour is claimed for it.
