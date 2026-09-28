// Real threads and real races.
//
// Every race here is started by releasing all threads from one atomic flag after
// they are all parked, so the contention is real and not a scheduling accident.
// Both sides of every race are asserted to have actually run: a test in which
// the losing side never gets to run proves nothing.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "fixture.hpp"
#include "test_harness.hpp"

#include "ups_control/adapter.hpp"
#include "ups_control/engine.hpp"
#include "ups_control/store.hpp"

using namespace ups_control;

namespace {

/// The instant a race is evaluated at: inside the freshness window of the
/// prepared observation (observed at 1000 with max_evidence_age 600).
constexpr std::int64_t kRaceTick = 1500;

/// A deterministic adapter that records every plan it was handed, counts the
/// issues, and detects whether two calls for the same unit were ever inside it
/// at the same time.
class RecordingAdapter final : public UpsAdapter {
 public:
  explicit RecordingAdapter(AckOutcome outcome) : outcome_(outcome) {}

  AdapterCapabilities capabilities() const override {
    AdapterCapabilities capabilities;
    capabilities.id = AdapterId::parse("recording-adapter").value();
    capabilities.supported_commands = all_command_kinds_mask();
    capabilities.reports_transfer_status = true;
    capabilities.reports_battery_evidence = true;
    capabilities.evidence = EvidenceClass::Synthetic;
    return capabilities;
  }

  AdapterIssueResult issue(const AdapterPlan& plan) override {
    AdapterIssueResult result;
    result.outcome = outcome_;
    result.detail = "recorded by the test adapter";
    {
      std::lock_guard<std::mutex> guard(mutex_);
      plans_.push_back(plan);
      std::uint64_t& active = active_by_ups_[plan.ups.value()];
      if (active != 0) {
        ++overlapped_;
      }
      ++active;
    }
    // Widen the window in which a second call for the same unit could be
    // observed if the engine's single-device gate were not held.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    {
      std::lock_guard<std::mutex> guard(mutex_);
      --active_by_ups_[plan.ups.value()];
    }
    return result;
  }

  std::size_t issue_count() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return plans_.size();
  }

  std::vector<AdapterPlan> plans() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return plans_;
  }

  /// Number of times a second call for the same unit overlapped an active one.
  std::size_t overlapped() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return overlapped_;
  }

 private:
  AckOutcome outcome_;
  mutable std::mutex mutex_;
  std::vector<AdapterPlan> plans_;
  std::map<std::string, std::uint64_t> active_by_ups_;
  std::size_t overlapped_ = 0;
};

/// A ready-to-use engine bound to a fresh scratch store, already registered,
/// observed, and granted. Built here rather than through uc_test::Fixture so
/// that the recording adapter is the one the engine was opened with.
struct Setup {
  std::unique_ptr<uc_test::ScratchDirectory> scratch;
  std::filesystem::path store_path;
  std::shared_ptr<UpsControlEngine> engine;
  std::shared_ptr<RecordingAdapter> adapter;
  std::vector<UpsRecord> units;
  std::vector<AuthorityRef> grants;
  ControlContext context{ControlEpoch{1}, Incarnation{1}};
  bool ok = false;
  std::string error;
};

Setup build(const std::string& name, std::size_t unit_count,
            AckOutcome acknowledgement = AckOutcome::Accepted) {
  Setup setup;
  setup.scratch = std::make_unique<uc_test::ScratchDirectory>(name);
  setup.store_path = setup.scratch->file("unit.upsstore");
  setup.adapter = std::make_shared<RecordingAdapter>(acknowledgement);

  EngineOpenOptions options;
  options.store.path = setup.store_path;
  options.store.access = StoreAccess::ReadWrite;
  options.store.create_if_missing = true;
  options.store.created_at = Tick{1000};
  options.store.epoch = ControlEpoch{1};
  options.store.incarnation = Incarnation{1};
  options.adapter = setup.adapter;
  const Result<std::shared_ptr<UpsControlEngine>> opened = UpsControlEngine::open(options);
  if (!opened.ok()) {
    setup.error = "the store could not be opened: " + opened.status().to_string();
    return setup;
  }
  setup.engine = opened.value();

  RevalidateRequest revalidate;
  revalidate.authority = setup.context;
  revalidate.now = Tick{1000};
  const Status revalidated = setup.engine->revalidate(revalidate).status();
  if (!revalidated.ok()) {
    setup.error = "revalidation failed: " + revalidated.to_string();
    return setup;
  }

  for (std::size_t index = 0; index < unit_count; ++index) {
    const std::string suffix = std::to_string(index + 1);
    RegisterUpsRequest registration;
    registration.authority = setup.context;
    registration.now = Tick{1000};
    registration.id = UpsId::parse("ups-" + suffix).value();
    registration.label = "unit " + suffix;
    registration.hardware = HardwareGeneration{1};
    registration.lifecycle = LifecycleState::InService;
    registration.operating = OperatingState::OnlineNormal;
    registration.policy.max_evidence_age = TickSpan{600};
    registration.policy.discharge_floor = ReserveQuantity{ReserveUnit::Seconds, 600};
    const Result<UpsRecord> registered = setup.engine->register_ups(registration);
    if (!registered.ok()) {
      setup.error = "registration failed: " + registered.status().to_string();
      return setup;
    }
    const Result<ObservationRecord> observed = setup.engine->record_telemetry(RecordTelemetryRequest{
        setup.context,
        UpsRef{registered.value().id, registered.value().hardware, registered.value().revision},
        Tick{1000},
        uc_test::healthy_report(registered.value().id, registered.value().hardware, Tick{1000},
                                SourceRevision{1}, "ev-" + suffix)});
    if (!observed.ok()) {
      setup.error = "the observation was refused: " + observed.status().to_string();
      return setup;
    }
    const AuthorityRef grant_ref = AuthorityRef::parse("grant-" + suffix).value();
    const Result<std::vector<UpsRecord>> current = setup.engine->units();
    const UpsRecord* live_unit = nullptr;
    if (current.ok()) {
      for (const UpsRecord& candidate : current.value()) {
        if (candidate.id == registered.value().id) {
          live_unit = &candidate;
        }
      }
    }
    if (live_unit == nullptr) {
      setup.error = "the registered unit vanished before its grant was issued";
      return setup;
    }
    IssueGrantRequest grant;
    grant.authority = setup.context;
    grant.ref = UpsRef{live_unit->id, live_unit->hardware, live_unit->revision};
    grant.now = Tick{1000};
    grant.grant.ref = grant_ref;
    grant.grant.scope = GrantScope::BypassTransfer;
    grant.grant.granted_by = AuthorityRef::parse("facility-ops").value();
    grant.grant.issued_at = Tick{1000};
    grant.grant.expires_at = Tick{100000000};
    const Result<AuthorityGrant> issued = setup.engine->issue_grant(grant);
    if (!issued.ok()) {
      setup.error = "the grant was refused: " + issued.status().to_string();
      return setup;
    }
    setup.grants.push_back(grant_ref);
  }

  const Result<std::vector<UpsRecord>> units = setup.engine->units();
  if (!units.ok()) {
    setup.error = "the units could not be read: " + units.status().to_string();
    return setup;
  }
  setup.units = units.value();
  setup.ok = true;
  return setup;
}

ControlCommand command_for(const Setup& setup, const UpsRecord& unit, const AuthorityRef& grant,
                           const std::string& key, Tick at) {
  ControlCommand command;
  command.authority = setup.context;
  command.ref = UpsRef{unit.id, unit.hardware, unit.revision};
  command.now = at;
  command.kind = CommandKind::EnterStaticBypass;
  command.key = IdempotencyKey::parse(key).value();
  command.authority_ref = grant;
  return command;
}

/// The observed outcome of one racing submission.
struct Submission {
  bool ok = false;
  AttemptPhase phase = AttemptPhase::Planned;
  RefusalCode refusal = RefusalCode::None;
  StatusCode error = StatusCode::Ok;
  AttemptId id;
  ControlContext cited;
  StateRevision planned_revision;
  ControlEpoch recorded_epoch;
  Incarnation recorded_incarnation;
};

bool is_accepted(const Submission& submission) {
  return submission.ok && (submission.phase == AttemptPhase::Acknowledged ||
                           submission.phase == AttemptPhase::Issued);
}

bool is_refused(const Submission& submission) {
  return submission.ok && submission.phase == AttemptPhase::Refused;
}

/// Releases every thread from one flag at the same moment and collects one
/// outcome per command. Each thread writes only its own slot.
std::vector<Submission> race_submissions(const std::shared_ptr<UpsControlEngine>& engine,
                                         const std::vector<ControlCommand>& commands) {
  const std::size_t count = commands.size();
  std::vector<Submission> results(count);
  std::atomic<int> parked{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  threads.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    threads.emplace_back([&engine, &commands, &results, &parked, &go, index]() {
      Submission& slot = results[index];
      slot.cited = commands[index].authority;
      slot.planned_revision = commands[index].ref.revision;
      parked.fetch_add(1);
      while (!go.load()) {
        std::this_thread::yield();
      }
      const Result<AttemptRecord> attempt = engine->submit(commands[index]);
      if (!attempt.ok()) {
        slot.error = attempt.status().code();
        return;
      }
      slot.ok = true;
      slot.phase = attempt.value().phase;
      slot.id = attempt.value().id;
      slot.recorded_epoch = attempt.value().epoch;
      slot.recorded_incarnation = attempt.value().incarnation;
      if (attempt.value().refusal.has_value()) {
        slot.refusal = attempt.value().refusal->code;
      }
    });
  }
  while (parked.load() < static_cast<int>(count)) {
    std::this_thread::yield();
  }
  go.store(true);
  for (std::thread& thread : threads) {
    thread.join();
  }
  return results;
}

}  // namespace

UC_TEST(concurrency, one_submission_wins_and_the_other_seven_are_refused_by_the_gate) {
  constexpr std::size_t kThreads = 8;
  Setup setup = build("conc-single", 1);
  if (!setup.ok) {
    UC_CHECK_MSG(false, setup.error);
    return;
  }
  const UpsRecord unit = setup.units.front();
  std::vector<ControlCommand> commands;
  for (std::size_t index = 0; index < kThreads; ++index) {
    commands.push_back(command_for(setup, unit, setup.grants.front(),
                                   "single-key-" + std::to_string(index), Tick{kRaceTick}));
  }
  const std::vector<Submission> results = race_submissions(setup.engine, commands);

  std::size_t accepted = 0;
  std::size_t refused = 0;
  std::size_t failed = 0;
  std::size_t wrong_refusal = 0;
  for (const Submission& submission : results) {
    if (is_accepted(submission)) {
      ++accepted;
      UC_CHECK_MSG(submission.phase == AttemptPhase::Acknowledged,
                   "an accepted submission stopped at phase " +
                       std::string(to_string(submission.phase)));
    } else if (is_refused(submission)) {
      ++refused;
      if (submission.refusal != RefusalCode::TransitionInProgress) {
        ++wrong_refusal;
      }
    } else {
      ++failed;
    }
  }
  UC_CHECK_MSG(accepted == 1,
               "exactly one submission must win the single-device gate, " +
                   std::to_string(accepted) + " did");
  UC_CHECK_MSG(refused == kThreads - 1,
               std::to_string(kThreads - 1) + " submissions must be refused by the gate, " +
                   std::to_string(refused) + " were");
  UC_CHECK_MSG(failed == 0,
               std::to_string(failed) + " submissions failed outright instead of being evaluated");
  UC_CHECK_MSG(wrong_refusal == 0,
               std::to_string(wrong_refusal) +
                   " refusals did not name transition_in_progress as the primary refusal");
  UC_CHECK_MSG(setup.adapter->issue_count() == 1u,
               "the adapter was invoked " + std::to_string(setup.adapter->issue_count()) +
                   " times although only one attempt may be issued at a time");

  const Result<std::vector<UpsRecord>> after = setup.engine->units();
  UC_CHECK_MSG(after.ok() && !after.value().empty(), "the unit vanished after the race");
  if (after.ok() && !after.value().empty()) {
    UC_CHECK_MSG(after.value().front().revision == unit.revision,
                 "an attempt changed the unit revision");
    UC_CHECK_MSG(after.value().front().in_flight.has_value(),
                 "the accepted attempt does not hold the in-flight gate");
  }
  (void)setup.engine->close();
}

UC_TEST(concurrency, twenty_rounds_of_four_threads_hold_the_gate_in_every_round) {
  constexpr std::size_t kThreads = 4;
  constexpr std::size_t kRounds = 20;
  Setup setup = build("conc-rounds", 1);
  if (!setup.ok) {
    UC_CHECK_MSG(false, setup.error);
    return;
  }
  std::int64_t tick = 2000;
  std::size_t accepted_total = 0;
  std::size_t refused_total = 0;
  OperatingState current = OperatingState::OnlineNormal;
  for (std::size_t round = 0; round < kRounds; ++round) {
    const Result<std::vector<UpsRecord>> units = setup.engine->units();
    if (!units.ok() || units.value().empty()) {
      UC_CHECK_MSG(false, "round " + std::to_string(round) + ": the unit is missing");
      return;
    }
    const UpsRecord unit = units.value().front();
    UC_CHECK_MSG(unit.operating == current,
                 "round " + std::to_string(round) + ": the unit is in " +
                     std::string(to_string(unit.operating)) + " instead of " +
                     std::string(to_string(current)));
    current = unit.operating;

    // A fresh observation of the current state keeps the transfer preconditions
    // established, so the only blocking finding in the race is the gate.
    RecordTelemetryRequest observation;
    observation.authority = setup.context;
    observation.ref = UpsRef{unit.id, unit.hardware, unit.revision};
    observation.now = Tick{tick};
    observation.report = uc_test::healthy_report(unit.id, unit.hardware, Tick{tick},
                                                 SourceRevision{2 + round},
                                                 "round-observation-" + std::to_string(round));
    observation.report.operating = current;
    const Result<ObservationRecord> recorded = setup.engine->record_telemetry(observation);
    if (!recorded.ok()) {
      UC_CHECK_MSG(false, "round " + std::to_string(round) +
                              ": the round observation was refused: " + recorded.status().to_string());
      return;
    }
    ++tick;

    const Result<std::vector<UpsRecord>> refreshed = setup.engine->units();
    if (!refreshed.ok() || refreshed.value().empty()) {
      UC_CHECK_MSG(false, "round " + std::to_string(round) + ": the unit is missing");
      return;
    }
    const UpsRecord prepared = refreshed.value().front();
    const CommandKind kind = current == OperatingState::OnlineNormal ? CommandKind::EnterStaticBypass
                                                                    : CommandKind::LeaveStaticBypass;
    std::vector<ControlCommand> commands;
    for (std::size_t index = 0; index < kThreads; ++index) {
      ControlCommand command = command_for(setup, prepared, setup.grants.front(),
                                           "round-" + std::to_string(round) + "-key-" +
                                               std::to_string(index),
                                           Tick{tick});
      command.kind = kind;
      commands.push_back(command);
    }
    const std::vector<Submission> results = race_submissions(setup.engine, commands);
    std::size_t accepted = 0;
    std::size_t refused = 0;
    std::size_t failed = 0;
    std::size_t wrong_refusal = 0;
    AttemptId winner;
    for (const Submission& submission : results) {
      if (is_accepted(submission)) {
        ++accepted;
        winner = submission.id;
      } else if (is_refused(submission)) {
        ++refused;
        if (submission.refusal != RefusalCode::TransitionInProgress) {
          ++wrong_refusal;
        }
      } else {
        ++failed;
      }
    }
    UC_CHECK_MSG(accepted == 1, "round " + std::to_string(round) + ": " +
                                    std::to_string(accepted) + " submissions were accepted");
    UC_CHECK_MSG(refused == kThreads - 1, "round " + std::to_string(round) + ": " +
                                              std::to_string(refused) + " submissions were refused");
    UC_CHECK_MSG(failed == 0, "round " + std::to_string(round) + ": " +
                                  std::to_string(failed) + " submissions failed outright");
    UC_CHECK_MSG(wrong_refusal == 0,
                 "round " + std::to_string(round) +
                     ": a refusal did not name transition_in_progress");
    accepted_total += accepted;
    refused_total += refused;
    ++tick;

    // End the round with a verified effect: the observation establishes the
    // target and the verification resolves the attempt, releasing the gate.
    const Result<std::vector<UpsRecord>> pending = setup.engine->units();
    if (!pending.ok() || pending.value().empty()) {
      UC_CHECK_MSG(false, "round " + std::to_string(round) + ": the unit is missing");
      return;
    }
    const OperatingState target = canonical_target(kind).value().value();
    RecordTelemetryRequest effect;
    effect.authority = setup.context;
    effect.ref = UpsRef{pending.value().front().id, pending.value().front().hardware,
                        pending.value().front().revision};
    effect.now = Tick{tick};
    effect.report = uc_test::healthy_report(pending.value().front().id,
                                            pending.value().front().hardware, Tick{tick},
                                            SourceRevision{3 + round},
                                            "round-effect-" + std::to_string(round));
    effect.report.operating = target;
    const Result<ObservationRecord> observed = setup.engine->record_telemetry(effect);
    if (!observed.ok()) {
      UC_CHECK_MSG(false, "round " + std::to_string(round) +
                              ": the effect observation was refused: " + observed.status().to_string());
      return;
    }
    ++tick;
    VerifyRequest verify;
    verify.authority = setup.context;
    verify.now = Tick{tick};
    verify.attempt = winner;
    const Result<AttemptRecord> verified = setup.engine->verify(verify);
    UC_CHECK_MSG(verified.ok() && verified.value().phase == AttemptPhase::Verified,
                 "round " + std::to_string(round) + ": the winning attempt was not verified: " +
                     (verified.ok() ? std::string(to_string(verified.value().phase))
                                    : verified.status().to_string()));
    ++tick;
    current = target;
  }
  UC_CHECK_MSG(accepted_total == kRounds,
               "the rounds accepted " + std::to_string(accepted_total) + " submissions instead of " +
                   std::to_string(kRounds));
  UC_CHECK_MSG(refused_total == kRounds * (kThreads - 1),
               "the rounds refused " + std::to_string(refused_total) + " submissions instead of " +
                   std::to_string(kRounds * (kThreads - 1)));
  UC_CHECK_MSG(setup.adapter->issue_count() == kRounds,
               "the adapter issued " + std::to_string(setup.adapter->issue_count()) +
                   " commands instead of one per round");
  (void)setup.engine->close();
}

UC_TEST(concurrency, authority_handoff_racing_with_submit_never_applies_a_stale_request) {
  constexpr std::size_t kSubmitters = 3;
  constexpr std::size_t kSubmissions = 12;
  constexpr std::size_t kHandoffs = 40;
  Setup setup = build("conc-handoff", 1);
  if (!setup.ok) {
    UC_CHECK_MSG(false, setup.error);
    return;
  }
  const UpsId ups = setup.units.front().id;
  const AuthorityRef original_grant = setup.grants.front();

  // One forward handoff before the race starts, so that a submission citing the
  // original incarnation is provably superseded from the first instant.
  const EngineInfo initial = setup.engine->info();
  AdoptAuthorityRequest first_handoff;
  first_handoff.authority = setup.context;
  first_handoff.now = Tick{kRaceTick};
  first_handoff.epoch = initial.epoch;
  first_handoff.incarnation = Incarnation{initial.incarnation.value() + 1};
  const Result<ControlContext> adopted = setup.engine->adopt_authority(first_handoff);
  UC_CHECK_MSG(adopted.ok(), "the initial handoff was refused: " + adopted.status().to_string());
  if (!adopted.ok()) {
    (void)setup.engine->close();
    return;
  }
  const ControlContext current_authority = adopted.value();

  // A reference submission under the current authority, taken before the race
  // starts: it proves that the accepting side of this lane is reachable at all,
  // and that an accepted attempt carries exactly the authority it cited.
  std::size_t reference_accepted = 0;
  {
    const Result<std::vector<UpsRecord>> units = setup.engine->units();
    if (!units.ok() || units.value().empty()) {
      UC_CHECK_MSG(false, "the unit is missing before the reference submission");
      (void)setup.engine->close();
      return;
    }
    IssueGrantRequest grant;
    grant.authority = current_authority;
    grant.ref = UpsRef{units.value().front().id, units.value().front().hardware,
                       units.value().front().revision};
    grant.now = Tick{kRaceTick};
    grant.grant.ref = AuthorityRef::parse("reference-authority").value();
    grant.grant.scope = GrantScope::BypassTransfer;
    grant.grant.granted_by = AuthorityRef::parse("facility-ops").value();
    grant.grant.issued_at = Tick{kRaceTick};
    grant.grant.expires_at = Tick{100000000};
    const Result<AuthorityGrant> issued = setup.engine->issue_grant(grant);
    UC_CHECK_MSG(issued.ok(), "the reference grant was refused: " + issued.status().to_string());
    if (issued.ok()) {
      const Result<std::vector<UpsRecord>> refreshed = setup.engine->units();
      if (refreshed.ok() && !refreshed.value().empty()) {
        ControlCommand command;
        command.authority = current_authority;
        command.ref = UpsRef{refreshed.value().front().id, refreshed.value().front().hardware,
                             refreshed.value().front().revision};
        command.now = Tick{kRaceTick};
        command.kind = CommandKind::EnterStaticBypass;
        command.key = IdempotencyKey::parse("reference-key").value();
        command.authority_ref = issued.value().ref;
        const Result<AttemptRecord> attempt = setup.engine->submit(command);
        UC_CHECK_MSG(attempt.ok(), "the reference submission failed outright: " +
                                       attempt.status().to_string());
        if (attempt.ok()) {
          UC_CHECK_MSG(attempt.value().phase == AttemptPhase::Acknowledged,
                       "the reference submission reached phase " +
                           std::string(to_string(attempt.value().phase)));
          UC_CHECK_MSG(attempt.value().epoch == current_authority.epoch &&
                           attempt.value().incarnation == current_authority.incarnation,
                       "the reference submission was recorded under a different authority");
          if (attempt.value().phase == AttemptPhase::Acknowledged) {
            ++reference_accepted;
            AbandonRequest abandon;
            abandon.authority = current_authority;
            abandon.now = Tick{kRaceTick};
            abandon.attempt = attempt.value().id;
            abandon.reason = "the reference submission is released";
            const Result<AttemptRecord> abandoned = setup.engine->abandon(abandon);
            UC_CHECK_MSG(abandoned.ok(), "the reference attempt could not be abandoned: " +
                                             abandoned.status().to_string());
          }
        }
      }
    }
  }

  std::atomic<bool> go{false};
  std::atomic<int> parked{0};
  std::atomic<std::size_t> accepted{0};
  std::atomic<std::size_t> stale_refusals{0};
  std::atomic<std::size_t> other_refusals{0};
  std::atomic<std::size_t> refused_without_reason{0};
  std::atomic<std::size_t> unexpected_errors{0};
  std::atomic<std::size_t> clock_races{0};
  std::atomic<std::size_t> mismatched_authority{0};
  std::atomic<std::size_t> handoffs{1};
  std::atomic<std::uint64_t> source_revision{2};

  std::thread adopter([&]() {
    parked.fetch_add(1);
    while (!go.load()) {
      std::this_thread::yield();
    }
    for (std::size_t index = 0; index < kHandoffs; ++index) {
      const EngineInfo info = setup.engine->info();
      AdoptAuthorityRequest adopt;
      adopt.authority = ControlContext{info.epoch, info.incarnation};
      adopt.now = Tick{kRaceTick};
      adopt.epoch = info.epoch;
      adopt.incarnation = Incarnation{info.incarnation.value() + 1};
      const Result<ControlContext> result = setup.engine->adopt_authority(adopt);
      if (!result.ok()) {
        continue;
      }
      handoffs.fetch_add(1);
      const Result<std::vector<UpsRecord>> units = setup.engine->units();
      if (!units.ok() || units.value().empty()) {
        continue;
      }
      const UpsRecord& unit = units.value().front();
      RecordTelemetryRequest observation;
      observation.authority = result.value();
      observation.ref = UpsRef{unit.id, unit.hardware, unit.revision};
      observation.now = Tick{kRaceTick};
      observation.report = uc_test::healthy_report(unit.id, unit.hardware, Tick{kRaceTick},
                                                   SourceRevision{source_revision.fetch_add(1)},
                                                   "handoff-observation");
      observation.report.operating = unit.operating;
      (void)setup.engine->record_telemetry(observation);
    }
  });

  std::vector<std::thread> submitters;
  // The first submitter always cites the superseded original authority.
  submitters.emplace_back([&]() {
    parked.fetch_add(1);
    while (!go.load()) {
      std::this_thread::yield();
    }
    for (std::size_t index = 0; index < kSubmissions; ++index) {
      const Result<std::vector<UpsRecord>> units = setup.engine->units();
      if (!units.ok() || units.value().empty()) {
        continue;
      }
      const UpsRecord& unit = units.value().front();
      ControlCommand command;
      command.authority = setup.context;
      command.ref = UpsRef{unit.id, unit.hardware, unit.revision};
      command.now = Tick{kRaceTick};
      command.kind = CommandKind::EnterStaticBypass;
      command.key = IdempotencyKey::parse("stale-lane-key-" + std::to_string(index)).value();
      command.authority_ref = original_grant;
      const Result<AttemptRecord> attempt = setup.engine->submit(command);
      if (!attempt.ok()) {
        if (attempt.status().code() == StatusCode::PreconditionFailed) {
          clock_races.fetch_add(1);
        } else {
          unexpected_errors.fetch_add(1);
        }
        continue;
      }
      if (attempt.value().phase == AttemptPhase::Refused) {
        if (!attempt.value().refusal.has_value()) {
          refused_without_reason.fetch_add(1);
          continue;
        }
        const RefusalCode code = attempt.value().refusal->code;
        if (code == RefusalCode::StaleEpoch || code == RefusalCode::StaleIncarnation ||
            code == RefusalCode::AuthorityFenced) {
          stale_refusals.fetch_add(1);
        } else {
          other_refusals.fetch_add(1);
        }
      } else {
        // The superseded lane must never be applied.
        mismatched_authority.fetch_add(1);
      }
    }
  });
  for (std::size_t thread = 0; thread < kSubmitters; ++thread) {
    submitters.emplace_back([&, thread]() {
      parked.fetch_add(1);
      while (!go.load()) {
        std::this_thread::yield();
      }
      for (std::size_t index = 0; index < kSubmissions; ++index) {
        const EngineInfo info = setup.engine->info();
        const Result<std::vector<UpsRecord>> units = setup.engine->units();
        if (!units.ok() || units.value().empty()) {
          continue;
        }
        const UpsRecord unit = units.value().front();
        const ControlContext cited{info.epoch, info.incarnation};
        const std::string grant_ref =
            "race-grant-" + std::to_string(thread) + "-" + std::to_string(index);
        IssueGrantRequest grant;
        grant.authority = cited;
        grant.ref = UpsRef{unit.id, unit.hardware, unit.revision};
        grant.now = Tick{kRaceTick};
        grant.grant.ref = AuthorityRef::parse(grant_ref).value();
        grant.grant.scope = GrantScope::BypassTransfer;
        grant.grant.granted_by = AuthorityRef::parse("facility-ops").value();
        grant.grant.issued_at = Tick{kRaceTick};
        grant.grant.expires_at = Tick{100000000};
        const Result<AuthorityGrant> issued = setup.engine->issue_grant(grant);
        if (!issued.ok()) {
          continue;
        }
        // Issuing the grant advanced the unit revision, so the submission is
        // planned against the revision the unit holds now, never the one read
        // before the grant.
        const Result<std::vector<UpsRecord>> refreshed = setup.engine->units();
        if (!refreshed.ok() || refreshed.value().empty()) {
          continue;
        }
        const StateRevision live_revision = refreshed.value().front().revision;
        ControlCommand command;
        command.authority = cited;
        command.ref = UpsRef{unit.id, unit.hardware, live_revision};
        command.now = Tick{kRaceTick};
        command.kind = CommandKind::EnterStaticBypass;
        command.key = IdempotencyKey::parse(grant_ref + "-key").value();
        command.authority_ref = issued.value().ref;
        const Result<AttemptRecord> attempt = setup.engine->submit(command);
        if (!attempt.ok()) {
          if (attempt.status().code() == StatusCode::PreconditionFailed) {
            clock_races.fetch_add(1);
          } else {
            unexpected_errors.fetch_add(1);
          }
          continue;
        }
        if (attempt.value().phase == AttemptPhase::Refused) {
          if (!attempt.value().refusal.has_value()) {
            refused_without_reason.fetch_add(1);
            continue;
          }
          const RefusalCode code = attempt.value().refusal->code;
          if (code == RefusalCode::StaleEpoch || code == RefusalCode::StaleIncarnation ||
              code == RefusalCode::AuthorityFenced) {
            stale_refusals.fetch_add(1);
          } else {
            other_refusals.fetch_add(1);
          }
          continue;
        }
        if (!(attempt.value().epoch == cited.epoch) ||
            !(attempt.value().incarnation == cited.incarnation) ||
            !(issued.value().epoch == attempt.value().epoch) ||
            !(issued.value().incarnation == attempt.value().incarnation)) {
          // The attempt was applied under an authority generation other than the
          // one the caller cited, or under a grant from a different generation.
          mismatched_authority.fetch_add(1);
        }
        accepted.fetch_add(1);
        AbandonRequest abandon;
        abandon.authority = cited;
        abandon.now = Tick{kRaceTick};
        abandon.attempt = attempt.value().id;
        abandon.reason = "the racing submission is released";
        (void)setup.engine->abandon(abandon);
      }
    });
  }

  while (parked.load() < static_cast<int>(kSubmitters) + 1) {
    std::this_thread::yield();
  }
  go.store(true);
  adopter.join();
  for (std::thread& thread : submitters) {
    thread.join();
  }

  UC_CHECK_MSG(accepted.load() + reference_accepted >= 1,
               "no submission was ever accepted, so the race never exercised the accepting side");
  UC_CHECK_MSG(stale_refusals.load() >= 1,
               "no submission was refused as stale, so the race never exercised the fencing side");
  UC_CHECK_MSG(mismatched_authority.load() == 0,
               std::to_string(mismatched_authority.load()) +
                   " submissions were applied under an authority the caller did not cite");
  UC_CHECK_MSG(refused_without_reason.load() == 0,
               std::to_string(refused_without_reason.load()) +
                   " refusals recorded no reason at all");
  UC_CHECK_MSG(unexpected_errors.load() == 0,
               std::to_string(unexpected_errors.load()) +
                   " submissions failed with an unexpected status");
  UC_CHECK_MSG(handoffs.load() >= 2, "the handoff thread never moved the authority forward");

  // The accepted attempts and the grants they cited must belong to the same
  // authority generation: a mismatch would mean the store applied a request
  // under an epoch it was never planned against.
  const Result<UpsRecord> final_unit = [&]() -> Result<UpsRecord> {
    const Result<std::vector<UpsRecord>> units = setup.engine->units();
    if (!units.ok()) {
      return units.status();
    }
    if (units.value().empty()) {
      return Status::error(StatusCode::NotFound, "no unit");
    }
    return units.value().front();
  }();
  UC_CHECK_MSG(final_unit.ok(), "the unit is missing after the race");
  const EngineInfo final_info = setup.engine->info();
  const std::uint64_t final_epoch = final_info.epoch.value();
  const std::uint64_t final_incarnation = final_info.incarnation.value();
  std::size_t accepted_in_journal = 0;
  if (final_unit.ok()) {
    HistoryQuery query;
    query.limit = 4096;
    const Result<std::vector<AttemptRecord>> journal = setup.engine->history(query);
    UC_CHECK_MSG(journal.ok(), "the journal could not be read");
    if (journal.ok()) {
      for (const AttemptRecord& attempt : journal.value()) {
        if (attempt.refusal.has_value() && attempt.phase != AttemptPhase::Refused) {
          UC_CHECK_MSG(false, "an attempt that was not refused carries a refusal record");
        }
        if (attempt.ack != AckOutcome::None) {
          // The device answered this attempt, so it was issued at least once.
          ++accepted_in_journal;
          // Every attempt that reached the device was planned against the
          // authority generation it cites, which the accepting thread checked
          // against the grant it had just obtained.
          if (attempt.epoch.value() > final_epoch ||
              attempt.incarnation.value() > final_incarnation) {
            UC_CHECK_MSG(false,
                         "an attempt cites an authority generation the store never reached");
          }
        }
      }
    }
  }
  UC_CHECK_MSG(accepted_in_journal >= accepted.load() + reference_accepted,
               "the journal holds fewer issued attempts than were accepted");

  (void)setup.engine->close();
  // The store must still be structurally valid after all of that.
  const Result<std::shared_ptr<const UpsState>> state =
      UpsStore::read_file(setup.store_path, StoreReadOptions{});
  UC_CHECK_MSG(state.ok(), "the store could not be read after the race: " +
                               state.status().to_string());
  if (state.ok()) {
    const Status valid = validate_state(*state.value(), ResourceLimits::defaults());
    UC_CHECK_MSG(valid.ok(), "the store is structurally invalid after the race: " +
                                 valid.to_string());
  }
}

UC_TEST(concurrency, readers_never_observe_a_torn_state_while_a_writer_mutates) {
  constexpr std::size_t kMutations = 200;
  constexpr std::size_t kReaderIterations = 4000;
  Setup setup = build("conc-readers", 1);
  if (!setup.ok) {
    UC_CHECK_MSG(false, setup.error);
    return;
  }
  const UpsId ups = setup.units.front().id;
  const AuthorityRef grant = setup.grants.front();

  std::atomic<bool> go{false};
  std::atomic<bool> writer_done{false};
  std::atomic<bool> invariant_ok{true};
  std::atomic<int> parked{0};
  std::atomic<std::size_t> mutations{0};
  std::atomic<std::size_t> reader_iterations{0};
  std::atomic<std::size_t> generation_changes{0};
  std::atomic<std::uint64_t> last_generation{0};

  std::thread writer([&]() {
    parked.fetch_add(1);
    while (!go.load()) {
      std::this_thread::yield();
    }
    for (std::size_t index = 0; index < kMutations; ++index) {
      const Result<std::vector<UpsRecord>> units = setup.engine->units();
      if (!units.ok() || units.value().empty()) {
        invariant_ok.store(false);
        break;
      }
      const UpsRecord& unit = units.value().front();
      const Tick at{2000 + static_cast<std::int64_t>(index) * 2};
      RecordTelemetryRequest observation;
      observation.authority = setup.context;
      observation.ref = UpsRef{unit.id, unit.hardware, unit.revision};
      observation.now = at;
      observation.report = uc_test::healthy_report(unit.id, unit.hardware, at,
                                                   SourceRevision{2 + index}, "writer-observation");
      observation.report.operating = OperatingState::OnlineNormal;
      const Result<ObservationRecord> recorded = setup.engine->record_telemetry(observation);
      if (!recorded.ok()) {
        invariant_ok.store(false);
        break;
      }
      mutations.fetch_add(1);
    }
    writer_done.store(true);
  });

  std::thread reader([&]() {
    parked.fetch_add(1);
    while (!go.load()) {
      std::this_thread::yield();
    }
    std::size_t iterations = 0;
    while (iterations < kReaderIterations && !(writer_done.load() && iterations >= 500)) {
      const Result<StoreAuditReport> audit = setup.engine->store_audit();
      if (!audit.ok()) {
        invariant_ok.store(false);
        break;
      }
      const std::uint64_t generation = audit.value().generation.value();
      const std::uint64_t previous = last_generation.load();
      if (generation < previous) {
        invariant_ok.store(false);
        break;
      }
      if (generation != previous) {
        generation_changes.fetch_add(1);
        last_generation.store(generation);
      }
      // This walk issues no control attempt at all, so the journal stays empty
      // and every reader must see the single registered unit.
      if (audit.value().attempt_count != 0 || audit.value().unit_count != 1) {
        invariant_ok.store(false);
        break;
      }

      UpsQuery query;
      query.ups = ups;
      query.now = Tick{kRaceTick};
      const Result<StatusReport> status = setup.engine->status(query);
      if (!status.ok() || status.value().revision.value() < 1) {
        invariant_ok.store(false);
        break;
      }
      const Result<ReadinessReport> readiness = setup.engine->readiness(query);
      if (!readiness.ok() || readiness.value().revision.value() < 1) {
        invariant_ok.store(false);
        break;
      }
      const Result<std::vector<UpsRecord>> units = setup.engine->units();
      if (!units.ok() || units.value().empty()) {
        invariant_ok.store(false);
        break;
      }
      for (const UpsRecord& unit : units.value()) {
        if (unit.revision.value() < 1) {
          invariant_ok.store(false);
          break;
        }
      }
      const Result<TransitionEvaluation> evaluation =
          setup.engine->evaluate(command_for(setup, units.value().front(), grant, "reader-key",
                                             Tick{kRaceTick}));
      if (!evaluation.ok() || evaluation.value().report.current_revision.value() < 1) {
        invariant_ok.store(false);
        break;
      }
      ++iterations;
    }
    reader_iterations.store(iterations);
  });

  while (parked.load() < 2) {
    std::this_thread::yield();
  }
  go.store(true);
  writer.join();
  reader.join();

  UC_CHECK_MSG(invariant_ok.load(), "a reader observed a torn or inconsistent state");
  UC_CHECK_MSG(mutations.load() == kMutations,
               "the writer committed " + std::to_string(mutations.load()) + " of " +
                   std::to_string(kMutations) + " mutations");
  UC_CHECK_MSG(reader_iterations.load() > 0, "the reader never ran a full iteration");
  UC_CHECK_MSG(generation_changes.load() > 0,
               "no reader iteration ever observed a generation change, so the race proved nothing");
  const Result<std::vector<UpsRecord>> final_units = setup.engine->units();
  UC_CHECK_MSG(final_units.ok() && !final_units.value().empty(), "the unit vanished");
  if (final_units.ok() && !final_units.value().empty()) {
    UC_CHECK_MSG(final_units.value().front().revision.value() >= kMutations + 1,
                 "the writer's mutations did not advance the unit revision");
  }
  (void)setup.engine->close();
}

UC_TEST(concurrency, the_adapter_never_observes_two_in_flight_attempts_for_one_unit) {
  constexpr std::size_t kUnits = 2;
  constexpr std::size_t kThreadsPerUnit = 4;
  Setup setup = build("conc-adapter", kUnits);
  if (!setup.ok) {
    UC_CHECK_MSG(false, setup.error);
    return;
  }
  std::vector<ControlCommand> commands;
  for (std::size_t unit = 0; unit < kUnits; ++unit) {
    for (std::size_t thread = 0; thread < kThreadsPerUnit; ++thread) {
      commands.push_back(command_for(setup, setup.units[unit], setup.grants[unit],
                                     "adapter-key-" + std::to_string(unit) + "-" +
                                         std::to_string(thread),
                                     Tick{kRaceTick}));
    }
  }
  const std::vector<Submission> results = race_submissions(setup.engine, commands);

  std::vector<std::size_t> accepted_per_unit(kUnits, 0);
  std::vector<std::size_t> refused_per_unit(kUnits, 0);
  std::size_t failed = 0;
  for (std::size_t index = 0; index < results.size(); ++index) {
    const std::size_t unit = index / kThreadsPerUnit;
    if (is_accepted(results[index])) {
      ++accepted_per_unit[unit];
    } else if (is_refused(results[index])) {
      ++refused_per_unit[unit];
      UC_CHECK_MSG(results[index].refusal == RefusalCode::TransitionInProgress,
                   "a losing submission was refused with " +
                       std::string(to_string(results[index].refusal)) +
                       " instead of transition_in_progress");
    } else {
      ++failed;
    }
  }
  UC_CHECK_MSG(failed == 0, std::to_string(failed) + " submissions failed outright");
  for (std::size_t unit = 0; unit < kUnits; ++unit) {
    UC_CHECK_MSG(accepted_per_unit[unit] == 1, "unit " + std::to_string(unit) + " accepted " +
                                                   std::to_string(accepted_per_unit[unit]) +
                                                   " submissions instead of one");
    UC_CHECK_MSG(refused_per_unit[unit] == kThreadsPerUnit - 1,
                 "unit " + std::to_string(unit) + " refused " +
                     std::to_string(refused_per_unit[unit]) + " submissions instead of " +
                     std::to_string(kThreadsPerUnit - 1));
  }

  const std::vector<AdapterPlan> plans = setup.adapter->plans();
  UC_CHECK_MSG(plans.size() == kUnits,
               "the adapter was handed " + std::to_string(plans.size()) +
                   " plans although exactly one attempt per unit may be in flight");
  for (std::size_t left = 0; left < plans.size(); ++left) {
    for (std::size_t right = left + 1; right < plans.size(); ++right) {
      const bool same_unit = plans[left].ups == plans[right].ups;
      const bool same_attempt = plans[left].attempt == plans[right].attempt;
      UC_CHECK_MSG(!(same_unit && same_attempt),
                   "the adapter was handed the same in-flight attempt twice");
      UC_CHECK_MSG(!same_unit,
                   "the adapter was handed two plans for unit '" + plans[left].ups.value() +
                       "' at the same time");
    }
  }
  for (std::size_t left = 0; left < plans.size(); ++left) {
    for (std::size_t right = left + 1; right < plans.size(); ++right) {
      UC_CHECK_MSG(!(plans[left].attempt == plans[right].attempt),
                   "the adapter was handed attempt " +
                       std::to_string(plans[left].attempt.value()) + " twice");
    }
  }
  UC_CHECK_MSG(setup.adapter->overlapped() == 0,
               "the adapter observed " + std::to_string(setup.adapter->overlapped()) +
                   " overlapping in-flight attempts for one unit");
  (void)setup.engine->close();
}


