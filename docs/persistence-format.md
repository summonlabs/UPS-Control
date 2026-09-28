# UPS Control store format, version 1

This document is the normative description of the durable artifact. The encoding
in `src/detail/serialization.cpp` and the publication protocol in
`src/store.cpp` implement exactly this document; where the two disagree, the code
is authoritative and the disagreement is a defect to fix.

## Why the store is two files

The store is a pair of files that share the caller's path:

| File | Role |
| --- | --- |
| `<path>` | the **head marker**: a fixed 128-byte, checksummed record that names the committed generation |
| `<path>.g<16 lowercase hex digits>` | the **generation payload**: the canonical logical state of exactly one generation |
| `<path>.staging-<pid>-<n>` | transient staging payload, present only during a commit |
| `<path>.head-tmp` | transient staged head marker, present only during a commit |
| `<path>.lock` | the writer lock file |

Splitting the head from the payload makes the commit point explicit and lets
recovery name exactly one generation. A reader derives the payload file name from
the generation the head names; it never searches for the newest file.

## Conventions

* All multi-byte integers are **little-endian**, unsigned unless stated.
* Every declared length is checked against a configured bound **before** any
  allocation happens.
* Text is UTF-8, bounded, and length-prefixed.
* Reserved fields must be zero. A non-zero reserved field is a corruption, not a
  value to interpret.
* An enumeration value outside its documented range is a corruption. In
  particular, a command kind that is not a documented operation is **never**
  mapped onto a neighbouring operation.

## Head marker (128 bytes)

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | magic `UPSCHD01` |
| 8 | 4 | format version (`1`) |
| 12 | 4 | byte-order marker `0x01020304` |
| 16 | 8 | store identity, high half |
| 24 | 8 | store identity, low half |
| 32 | 8 | committed generation |
| 40 | 8 | control epoch |
| 48 | 8 | controller incarnation |
| 56 | 8 | payload file size in bytes |
| 64 | 4 | CRC-32C over `payload[72 .. payload_bytes)` |
| 68 | 4 | CRC-32C over the recorded path bytes |
| 72 | 8 | commit ordinal, incremented once per successful commit |
| 80 | 8 | commit instant (signed 64-bit logical instant) |
| 88 | 8 | store creation instant |
| 96 | 24 | reserved, zero |
| 120 | 4 | format version repeated |
| 124 | 4 | CRC-32C over bytes `[0, 124)` |

The repeated format version and the trailing checksum make a head that was
truncated, extended, or byte-swapped detectable rather than interpretable. The
store identity is a 128-bit value drawn from the platform entropy source when the
store is created; the all-zero identity is never valid.

## Generation payload

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | magic `UPSPAY01` |
| 8 | 4 | format version (`1`) |
| 12 | 4 | byte-order marker `0x01020304` |
| 16 | 8 | generation |
| 24 | 8 | control epoch |
| 32 | 8 | controller incarnation |
| 40 | 8 | store identity, high half |
| 48 | 8 | store identity, low half |
| 56 | 4 | recorded path length in bytes |
| 60 | 4 | canonical state length in bytes |
| 64 | 4 | CRC-32C over the canonical state section |
| 68 | 4 | reserved, zero |
| 72 | *path bytes* | recorded path, UTF-8 |
| 72 + path | *state bytes* | canonical logical state |
| end - 12 | 8 | trailer magic `UPSEND01` |
| end - 4 | 4 | CRC-32C over `payload[0, end - 4)` |

The file size must equal `84 + path bytes + state bytes` exactly. A payload that
is shorter or longer is refused; a partial generation is never combined with a
whole one.

The recorded path is the absolute, lexically normalised, symlink-free path of the
store at the moment it was created. It is bound into the head marker by its
checksum, and it is the basis for the optional path-binding check.

## Canonical logical state

The state section is a fixed sequence of fields with no padding, no
implementation-defined ordering, and no volatile content. In particular it
contains no wall clock, no process identifier, no memory address, no store
identity, and no recorded path, so two logically equal states always encode to
identical bytes and share one digest.

```
u32  format version (1)
u32  byte-order marker (0x01020304)
u64  generation
u64  epoch
u64  incarnation
i64  created_at
i64  updated_at
i64  revalidated_at
u64  last attempt ordinal
u64  operation count
u32  unit count, then that many unit records
u32  attempt count, then that many attempt records
u32  idempotency count, then that many (key, attempt, plan digest, recorded_at)
u32  commit log count, then that many (generation, attempt, key, operation, unit, revision, at)
```

Each unit record carries its identifier, label, hardware generation, lifecycle
state, operating state, state basis, state revision, instants, the last accepted
telemetry observation with its provenance and observation instant, the reserve
policy, its obligation bindings, its authority grants, the in-flight attempt gate,
and the last verification instant. Units are stored in strictly increasing
identifier order and obligations and grants are stored in strictly increasing
reference order, so the encoding does not depend on insertion order.

## Publication protocol

```
plan      check the generation is exactly current + 1
validate  structural validation of the whole state
reserve   assign the staging file name and the commit ordinal
write     create the staging payload                        <- crash point 1 / 2
flush     flush the staging payload to the device           <- crash point 3
verify    read the staging payload back and compare it byte for byte
publish   atomically rename the staging payload to <path>.g<generation>   <- crash point 4
commit    durably write <path>.head-tmp, read it back, then atomically
          replace <path> with it                            <- THE COMMIT POINT
retire    remove the superseded payload and any staging residue  <- crash point 5
```

The commit point is the atomic replacement of the head marker. Everything before
it is invisible to a reader; everything after it is housekeeping. Nothing is
published in memory unless the head replacement succeeded.

Crash points are the durable stages named by `DurableCrashPoint`. They exist so
that the crash-recovery proof obligations can be discharged against real process
death, and a production caller never sets them.

## Recovery

Opening a store:

1. reads the head marker strictly, refusing any size other than 128 bytes, a wrong
   magic, a wrong version, a wrong byte order, a non-zero reserved field, and a
   failed checksum;
2. derives the payload path from the generation the head names;
3. refuses when the payload file size differs from the size the head declares;
4. verifies the payload envelope, both checksums, and the exact consumption of the
   declared sections;
5. decodes the canonical state, range-checking every enumeration and bounding
   every declared length against the configured limits;
6. requires the payload generation, epoch, incarnation, and store identity to
   agree with the head;
7. runs the structural state validation, which rejects duplicate identities,
   out-of-order collections, cross-references to missing objects, and any unit that
   holds the in-flight gate for an attempt that is not the single unresolved one;
8. enforces the caller's identity, generation floor, and path-binding
   preconditions.

Recovery adopts exactly one whole verified generation or refuses. It never falls
back to an older generation, and it **never** makes a dynamic observation fresh:
the observation instant is preserved verbatim, every unit is **reported** with the
`Recovered` state basis for the rest of the session, and the engine's own
lifecycle starts at `Recovered` so that no control decision may be taken until
`revalidate` is called with an explicit instant. `UpsStore::read_file` itself
returns the stored basis verbatim, which is what the field-by-field round-trip
contract requires; the downgrade is a property of the engine's view of a session,
not of the artifact.

## Path rules

Both files of the store are held to the same rules, and the derived names are
checked before they are written as well as before they are read:

* the caller's path is validated **before** any normalization, so normalization
  can never erase the evidence of a malformed input: it must be non-empty, must
  not contain an embedded NUL, must be valid UTF-8 on this platform, and must not
  exceed the configured path bound;
* the file name must not be a reserved platform device name (`CON`, `PRN`,
  `AUX`, `NUL`, `CLOCK$`, `COM1`..`COM9`, `LPT1`..`LPT9`), with or without an
  extension;
* the parent must exist and must be a directory;
* the target must not be a symbolic link, must not carry a reparse attribute on
  Windows (a junction, a mount point, or another name surrogate), must not be a
  directory, and must be a regular file if it exists at all;
* the **generation payload** path is subject to the same checks, so a payload
  reached through a link is refused before its content is examined, whichever
  target the link names. A link that resolves nowhere is refused as missing;
* the **recorded path** inside the payload is untrusted input like every other
  field. It is rejected outright if it contains an embedded NUL or is otherwise
  not a usable path, whether or not path binding is enforced; with binding
  enforced it must additionally equal the resolved path being read;
* the staging and staged-head names are checked before the durable write, because
  a staging name is derived from the process identifier and a serial and is
  therefore guessable. Without that check a pre-created symbolic link at that name
  would redirect the write somewhere else.

Residue from a crashed writer (`<path>.staging-*`, `<path>.head-tmp`, and any
`<path>.g*` file other than the one the head names) is removed by the next
read-write open and by the next successful commit. Residue removal is best effort
and its failures are recorded, never silently ignored into a success.

## Integrity model

CRC-32C detects accidental corruption: bit rot, a truncated write, a partial
flush, a torn block. It is **not** a cryptographic authentication code, it uses no
secret, and it does not resist an adversary who can rewrite the artifact and
recompute the checksum. What rejects a wrong-version, wrong-endian, oversized,
structurally invalid, or substituted artifact is the combination of the magic, the
version, the byte-order marker, the reserved-field rules, the declared-length
bounds, the cross-checks between the head and the payload, the structural state
validation, the optional expected identity, and the optional generation floor.

## Writer lock

A read-write open takes an operating-system level exclusive lock on
`<path>.lock`: on Windows the file is opened with share mode zero, on POSIX it is
held with `flock(LOCK_EX | LOCK_NB)`. Either way the kernel releases the lock when
the holding process exits, including abnormal exit, so process death relinquishes
writer authority with no recovery step. A read-only open takes no lock and either
observes one whole verified generation or fails; it never observes a partial one.
