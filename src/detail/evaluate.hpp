#pragma once

// Internal pure transition-evaluation core. Not installed.
//
// Evaluation is separated from the engine so that it can be exercised directly,
// deterministically, and without a store: it reads a state, a record, a request,
// and the adapter capability mask, and produces a report. It performs no I/O,
// holds no lock, and mutates nothing.

#include <cstdint>
#include <optional>

#include "ups_control/adapter.hpp"
#include "ups_control/command.hpp"
#include "ups_control/evaluation.hpp"
#include "ups_control/ids.hpp"
#include "ups_control/limits.hpp"
#include "ups_control/state.hpp"
#include "ups_control/status.hpp"

namespace ups_control::detail {

struct EvaluationInputs {
  const UpsState* state = nullptr;
  const UpsRecord* record = nullptr;
  const ControlCommand* command = nullptr;
  const ResourceLimits* limits = nullptr;
  /// True once the store has been revalidated at the request's instant.
  bool revalidated = false;
  /// True when an adapter is bound to the engine.
  bool adapter_bound = false;
  /// Command mask of the bound adapter.
  std::uint32_t adapter_supported_commands = 0;
  AdapterId adapter;
  /// True when the unit's stored state basis was established in this engine
  /// session. A basis that was only read back from a store is reported as
  /// \c StateBasis::Recovered, because persistence never carries a freshness
  /// claim across a restart.
  bool basis_confirmed = false;
};

/// Validates the shape of a request. Returns a status error for a malformed
/// request; a well-formed request that is not permitted produces a report with
/// findings instead.
Status validate_command_shape(const ControlCommand& command, const ResourceLimits& limits);

/// Evaluates one control request against one record. Pure.
Result<EvaluationReport> evaluate_transition(const EvaluationInputs& inputs);

/// Computes the readiness snapshot for one unit. Pure.
ReadinessReport compute_readiness(const UpsState& state, const UpsRecord& record,
                                  const ResourceLimits& limits, Tick now, bool revalidated,
                                  bool adapter_bound, std::uint32_t adapter_supported_commands,
                                  bool basis_confirmed);

/// The basis this session may report for one unit: the stored basis when it was
/// established in this session, and \c StateBasis::Recovered otherwise.
StateBasis reported_basis(const UpsRecord& record, bool basis_confirmed) noexcept;

/// The freshness window applied to observation-derived preconditions of one unit.
FreshnessPolicy unit_freshness_policy(const ReservePolicy& policy);

}  // namespace ups_control::detail
