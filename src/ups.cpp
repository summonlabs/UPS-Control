#include "ups_control/ups.hpp"

#include <algorithm>
#include <string>

namespace ups_control {
namespace {

template <typename Container, typename Key>
auto lower_bound_by_key(Container& container, const Key& key) {
  return std::lower_bound(container.begin(), container.end(), key,
                          [](const auto& element, const Key& value) {
                            return element.ref < value;
                          });
}

}  // namespace

const ProtectedLoadObligation* find_obligation(const UpsRecord& record,
                                               const ObligationRef& ref) noexcept {
  const auto found = std::lower_bound(record.obligations.begin(), record.obligations.end(), ref,
                                      [](const ProtectedLoadObligation& element,
                                         const ObligationRef& value) {
                                        return element.ref < value;
                                      });
  if (found == record.obligations.end() || !(found->ref == ref)) {
    return nullptr;
  }
  return &*found;
}

const AuthorityGrant* find_grant(const UpsRecord& record, const AuthorityRef& ref) noexcept {
  const auto found = std::lower_bound(record.grants.begin(), record.grants.end(), ref,
                                      [](const AuthorityGrant& element, const AuthorityRef& value) {
                                        return element.ref < value;
                                      });
  if (found == record.grants.end() || !(found->ref == ref)) {
    return nullptr;
  }
  return &*found;
}

Status upsert_obligation(UpsRecord& record, ProtectedLoadObligation obligation,
                         const ResourceLimits& limits) {
  auto found = lower_bound_by_key(record.obligations, obligation.ref);
  if (found != record.obligations.end() && found->ref == obligation.ref) {
    *found = std::move(obligation);
    return Status::success();
  }
  if (record.obligations.size() >= limits.max_obligations_per_ups) {
    return Status::error(StatusCode::LimitExceeded,
                         "the unit already holds " + std::to_string(record.obligations.size()) +
                             " obligations, the limit is " +
                             std::to_string(limits.max_obligations_per_ups));
  }
  record.obligations.insert(found, std::move(obligation));
  return Status::success();
}

Status upsert_grant(UpsRecord& record, AuthorityGrant grant, const ResourceLimits& limits) {
  auto found = lower_bound_by_key(record.grants, grant.ref);
  if (found != record.grants.end() && found->ref == grant.ref) {
    *found = std::move(grant);
    return Status::success();
  }
  if (record.grants.size() >= limits.max_grants_per_ups) {
    return Status::error(StatusCode::LimitExceeded,
                         "the unit already holds " + std::to_string(record.grants.size()) +
                             " grants, the limit is " + std::to_string(limits.max_grants_per_ups));
  }
  record.grants.insert(found, std::move(grant));
  return Status::success();
}

Status validate_ups_record(const UpsRecord& record, const ResourceLimits& limits) {
  Status status = validate_identifier(record.id.value(), limits.max_identifier_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), "ups id: " + status.message());
  }
  status = validate_label(record.label, limits.max_label_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), "ups label: " + status.message());
  }
  status = validate_label(record.registration_note, limits.max_detail_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), "ups note: " + status.message());
  }
  if (record.revision.is_zero()) {
    return Status::error(StatusCode::InvariantViolation,
                         "a registered unit must carry a positive state revision");
  }
  if (!is_valid_instant(record.registered_at) || !is_valid_instant(record.state_since)) {
    return Status::error(StatusCode::InvalidArgument,
                         "a registered unit must carry positive registration and state instants");
  }
  if (static_cast<int>(record.lifecycle) < static_cast<int>(LifecycleState::Commissioning) ||
      static_cast<int>(record.lifecycle) > static_cast<int>(LifecycleState::Decommissioned)) {
    return Status::error(StatusCode::InvalidArgument, "lifecycle state is outside the enumeration");
  }
  if (static_cast<int>(record.operating) < static_cast<int>(OperatingState::Unknown) ||
      static_cast<int>(record.operating) > static_cast<int>(OperatingState::Retired)) {
    return Status::error(StatusCode::InvalidArgument, "operating state is outside the enumeration");
  }
  if (!is_consistent(record.lifecycle, record.operating)) {
    return Status::error(StatusCode::Conflict,
                         std::string("lifecycle ") + to_string(record.lifecycle) +
                             " is inconsistent with operating state " +
                             to_string(record.operating));
  }
  status = validate_reserve_policy(record.reserve_policy, limits);
  if (!status.ok()) {
    return Status::error(status.code(), "reserve policy: " + status.message());
  }
  if (record.observation.has_value()) {
    status = validate_identifier(record.observation->evidence.value(), limits.max_identifier_bytes);
    if (!status.ok()) {
      return Status::error(status.code(), "observation evidence id: " + status.message());
    }
    status = validate_identifier(record.observation->source.value(), limits.max_identifier_bytes);
    if (!status.ok()) {
      return Status::error(status.code(), "observation source: " + status.message());
    }
    if (record.observation->provenance == Provenance::Unknown) {
      return Status::error(StatusCode::InvalidArgument, "an observation must state its provenance");
    }
    if (!is_valid_instant(record.observation->observed_at) ||
        !is_valid_instant(record.observation->received_at)) {
      return Status::error(StatusCode::InvalidArgument,
                           "an observation must carry positive instants");
    }
    status = validate_reserve_observation(record.observation->battery.reserve, limits);
    if (!status.ok()) {
      return Status::error(status.code(), "observation reserve: " + status.message());
    }
    status = validate_reserve_observation(record.observation->battery.state_of_charge, limits);
    if (!status.ok()) {
      return Status::error(status.code(), "observation state of charge: " + status.message());
    }
    status = validate_label(record.observation->contradiction_detail, limits.max_detail_bytes);
    if (!status.ok()) {
      return Status::error(status.code(), "observation contradiction detail: " + status.message());
    }
  }
  if (record.obligations.size() > limits.max_obligations_per_ups) {
    return Status::error(StatusCode::LimitExceeded, "too many obligations on one unit");
  }
  for (std::size_t index = 0; index < record.obligations.size(); ++index) {
    status = validate_obligation(record.obligations[index], limits);
    if (!status.ok()) {
      return status;
    }
    if (index > 0 && !(record.obligations[index - 1].ref < record.obligations[index].ref)) {
      return Status::error(StatusCode::DuplicateIdentity,
                           "obligation bindings must be strictly ordered by reference and free of "
                           "duplicates: '" + record.obligations[index].ref.value() + "'");
    }
  }
  if (record.grants.size() > limits.max_grants_per_ups) {
    return Status::error(StatusCode::LimitExceeded, "too many grants on one unit");
  }
  for (std::size_t index = 0; index < record.grants.size(); ++index) {
    status = validate_grant(record.grants[index], limits);
    if (!status.ok()) {
      return status;
    }
    if (index > 0 && !(record.grants[index - 1].ref < record.grants[index].ref)) {
      return Status::error(StatusCode::DuplicateIdentity,
                           "grants must be strictly ordered by reference and free of duplicates: '" +
                               record.grants[index].ref.value() + "'");
    }
  }
  if (record.in_flight.has_value() && record.in_flight->is_zero()) {
    return Status::error(StatusCode::InvariantViolation,
                         "the in-flight attempt gate must name a real attempt or be unset");
  }
  if (record.last_verified_at.value() < 0) {
    return Status::error(StatusCode::InvalidArgument,
                         "the last verification instant must not be negative");
  }
  return Status::success();
}

}  // namespace ups_control
