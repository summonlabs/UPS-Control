#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ups_control/authority.hpp"
#include "ups_control/battery.hpp"
#include "ups_control/ids.hpp"
#include "ups_control/limits.hpp"
#include "ups_control/obligation.hpp"
#include "ups_control/operating.hpp"
#include "ups_control/telemetry.hpp"

namespace ups_control {

/// The authoritative record of one UPS.
///
/// Stable identity (\c id) is separate from mutable metadata (\c label). The
/// hardware generation, the per-device state revision, the control epoch, and the
/// controller incarnation are four distinct values and are never interchanged.
struct UpsRecord {
  UpsId id;
  std::string label;
  HardwareGeneration hardware;
  LifecycleState lifecycle = LifecycleState::Commissioning;
  OperatingState operating = OperatingState::Unknown;
  /// How the operating state was established. This is the durable record of how
  /// it was established when it was written; the engine **reports** \c Recovered
  /// for any unit whose basis was not established in the current session, because
  /// persistence never carries a freshness claim across a restart.
  /// \c UpsStore::read_file returns the stored value verbatim, which is what the
  /// field-by-field persistence round trip requires.
  StateBasis basis = StateBasis::Recovered;
  StateRevision revision;
  Tick state_since;
  Tick registered_at;
  std::string registration_note;

  /// The last telemetry accepted for this unit. Absent means nothing has ever been
  /// observed, which is distinct from an observation of zero.
  std::optional<ObservationRecord> observation;

  ReservePolicy reserve_policy;

  /// Bindings sorted by reference. Order is part of the canonical encoding.
  std::vector<ProtectedLoadObligation> obligations;
  /// Grants sorted by reference.
  std::vector<AuthorityGrant> grants;

  /// The unresolved control attempt holding the single-device ordering gate.
  std::optional<AttemptId> in_flight;
  Tick last_verified_at;

  friend bool operator==(const UpsRecord&, const UpsRecord&) = default;
};

const ProtectedLoadObligation* find_obligation(const UpsRecord& record,
                                               const ObligationRef& ref) noexcept;
const AuthorityGrant* find_grant(const UpsRecord& record, const AuthorityRef& ref) noexcept;

/// Inserts or replaces an obligation, keeping the vector sorted by reference.
Status upsert_obligation(UpsRecord& record, ProtectedLoadObligation obligation,
                         const ResourceLimits& limits);

/// Inserts or replaces a grant, keeping the vector sorted by reference.
Status upsert_grant(UpsRecord& record, AuthorityGrant grant, const ResourceLimits& limits);

Status validate_ups_record(const UpsRecord& record, const ResourceLimits& limits);

}  // namespace ups_control
