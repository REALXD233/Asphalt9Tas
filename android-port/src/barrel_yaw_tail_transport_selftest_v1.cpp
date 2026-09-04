#include "barrel_yaw_tail_transport_v1.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace transport = a9tas::barrel_yaw_tail_transport_v1;
namespace host = a9tas::barrel_yaw_tail_host_v1;
namespace protocol = a9tas::barrel_yaw_tail_payload_v1;

namespace {

constexpr std::uintptr_t kPayloadBase = 0x10000000u;
constexpr std::uintptr_t kObjectRegionBase = 0x20000000u;
constexpr std::uintptr_t kObject = kObjectRegionBase + 0x2000u;
constexpr std::uintptr_t kLibraryBase = 0x30000000u;
constexpr std::uintptr_t kOriginalVptr =
    kLibraryBase + protocol::kPhysicsBackendVptrRva;
constexpr std::uintptr_t kOriginalTable =
    kOriginalVptr - protocol::kVptrPrefixSize;
constexpr std::uintptr_t kNativeBody = 0x40000000u;
constexpr std::uintptr_t kNativeAngular =
    kNativeBody + protocol::kNativeAngularOffset;
constexpr std::uint32_t kTid = 77;
constexpr std::uint32_t kGeneration = 9;

struct Region {
  std::uintptr_t base{};
  std::vector<std::uint8_t> bytes;
};

struct Memory {
  std::vector<Region> regions;
  std::vector<std::uintptr_t> writes;
  std::uintptr_t fail_address{};
  bool fail_after_mutation{};
  bool fail_once{};
};

Region* FindRegion(Memory* memory, std::uintptr_t address, std::size_t size) {
  if (memory == nullptr || size == 0) return nullptr;
  for (auto& region : memory->regions) {
    if (address < region.base) continue;
    const std::uintptr_t offset = address - region.base;
    if (offset <= region.bytes.size() &&
        size <= region.bytes.size() - static_cast<std::size_t>(offset))
      return &region;
  }
  return nullptr;
}

bool ReadMemory(void* context, std::uintptr_t address, void* output,
                std::size_t size) {
  auto* memory = static_cast<Memory*>(context);
  Region* region = FindRegion(memory, address, size);
  if (region == nullptr || output == nullptr) return false;
  std::memcpy(output, region->bytes.data() + (address - region->base), size);
  return true;
}

bool WriteMemory(void* context, std::uintptr_t address, const void* input,
                 std::size_t size) {
  auto* memory = static_cast<Memory*>(context);
  Region* region = FindRegion(memory, address, size);
  if (region == nullptr || input == nullptr) return false;
  memory->writes.push_back(address);
  if (memory->fail_once && address == memory->fail_address) {
    memory->fail_once = false;
    if (memory->fail_after_mutation)
      std::memcpy(region->bytes.data() + (address - region->base), input,
                  size);
    return false;
  }
  std::memcpy(region->bytes.data() + (address - region->base), input, size);
  return true;
}

template <typename T>
T* At(Memory* memory, std::uintptr_t address) {
  Region* region = FindRegion(memory, address, sizeof(T));
  return region == nullptr
             ? nullptr
             : reinterpret_cast<T*>(region->bytes.data() +
                                    (address - region->base));
}

std::uint8_t* BytesAt(Memory* memory, std::uintptr_t address,
                      std::size_t size) {
  Region* region = FindRegion(memory, address, size);
  return region == nullptr
             ? nullptr
             : region->bytes.data() + (address - region->base);
}

struct Fixture {
  Memory memory{};
  transport::Io io{};
  a9tas::barrel_yaw_tail_elf_v1::Layout layout{};
  std::array<protocol::FrameTarget, 2> targets{};
  transport::Session session{};
};

bool Setup(Fixture* fixture) {
  if (fixture == nullptr) return false;
  fixture->memory.regions.push_back(
      Region{kPayloadBase, std::vector<std::uint8_t>(0xB0000)});
  fixture->memory.regions.push_back(
      Region{kObjectRegionBase, std::vector<std::uint8_t>(0x4000)});
  fixture->memory.regions.push_back(
      Region{kOriginalTable,
             std::vector<std::uint8_t>(protocol::kShadowSize)});
  fixture->memory.regions.push_back(
      Region{kNativeBody, std::vector<std::uint8_t>(0x400)});
  fixture->io = {&fixture->memory, ReadMemory, WriteMemory};
  fixture->layout.boundary = kPayloadBase + 0x100;
  fixture->layout.shadow = kPayloadBase + 0x1000;
  fixture->layout.control = kPayloadBase + 0x2000;
  fixture->layout.targets = kPayloadBase + 0x3000;
  fixture->layout.audits = kPayloadBase + 0x20000;
  fixture->layout.evidence = kPayloadBase + 0xA0000;

  *At<std::uintptr_t>(&fixture->memory, kObject) = kOriginalVptr;
  *At<std::uintptr_t>(&fixture->memory,
                      kObject + protocol::kNativeBodyPointerOffset) =
      kNativeBody;
  std::uint8_t* original = BytesAt(&fixture->memory, kOriginalTable,
                                   protocol::kShadowSize);
  if (original == nullptr) return false;
  for (std::size_t index = 0; index < protocol::kShadowSize; ++index)
    original[index] = static_cast<std::uint8_t>((index * 37u + 11u) & 0xffu);
  const std::uintptr_t original_boundary =
      kLibraryBase + protocol::kOriginalBoundaryCallbackRva;
  std::memcpy(original + protocol::kVptrPrefixSize +
                  protocol::kBoundarySlotOffset,
              &original_boundary, sizeof(original_boundary));

  auto* control = At<protocol::Control>(&fixture->memory,
                                        fixture->layout.control);
  auto* evidence = At<protocol::Evidence>(&fixture->memory,
                                          fixture->layout.evidence);
  if (control == nullptr || evidence == nullptr) return false;
  std::memcpy(control->magic, protocol::kControlMagic, 8);
  control->version = protocol::kProtocolVersion;
  control->size = sizeof(*control);
  std::memcpy(evidence->magic, protocol::kEvidenceMagic, 8);
  evidence->version = protocol::kProtocolVersion;
  evidence->size = sizeof(*evidence);
  evidence->last_status = protocol::kStatusPassive;

  fixture->targets[0].angular_bits[0] = 0x3f800000u;
  fixture->targets[0].angular_bits[1] = 0xbf000000u;
  fixture->targets[0].angular_bits[2] = 0x3e800000u;
  fixture->targets[1].angular_bits[0] = 0x40000000u;
  fixture->targets[1].angular_bits[1] = 0x40400000u;
  fixture->targets[1].angular_bits[2] = 0xc0800000u;
  fixture->targets[1].skip_override_flags =
      a9tas::unified_tick_v1::kSkipBarrelAngular;
  std::uint8_t recording_sha256[32]{};
  for (std::size_t index = 0; index < sizeof(recording_sha256); ++index)
    recording_sha256[index] = static_cast<std::uint8_t>(0xA0u + index);
  return transport::Install(
      fixture->io, fixture->layout, kObject, kNativeAngular, kTid,
      kGeneration, recording_sha256, fixture->targets.data(),
      static_cast<std::uint32_t>(fixture->targets.size()),
      &fixture->session);
}

bool InstalledExactly(const Fixture& fixture) {
  const auto& writes = fixture.memory.writes;
  if (writes.size() != 4 || writes[0] != fixture.layout.targets ||
      writes[1] != fixture.layout.shadow ||
      writes[2] != fixture.layout.control ||
      writes[3] !=
          fixture.layout.control + offsetof(protocol::Control, flags))
    return false;
  const auto* object_vptr =
      At<std::uintptr_t>(const_cast<Memory*>(&fixture.memory), kObject);
  const auto* shadow = BytesAt(const_cast<Memory*>(&fixture.memory),
                               fixture.layout.shadow, protocol::kShadowSize);
  return object_vptr != nullptr && *object_vptr == kOriginalVptr &&
         shadow != nullptr &&
         std::memcmp(shadow, fixture.session.prepared.shadow.data(),
                     protocol::kShadowSize) == 0 &&
         fixture.session.prepared.original[protocol::kShadowSize - 1] ==
             *BytesAt(const_cast<Memory*>(&fixture.memory),
                      kOriginalTable + protocol::kShadowSize - 1, 1) &&
         fixture.session.prepared.published_control
                 .expected_first_caller_return ==
             kLibraryBase + protocol::kFirstBoundaryCallerReturnRva &&
         fixture.session.prepared.published_control
                 .expected_second_caller_return ==
             kLibraryBase + protocol::kSecondBoundaryCallerReturnRva;
}

bool ArmWritesInOrder(const Fixture& fixture, std::size_t first_write) {
  const auto& writes = fixture.memory.writes;
  return writes.size() == first_write + 4 &&
         writes[first_write] ==
             fixture.layout.control +
                 offsetof(protocol::Control, active_audit_index) &&
         writes[first_write + 1] ==
             fixture.layout.control +
                 offsetof(protocol::Control, active_frame_index) &&
         writes[first_write + 2] ==
             fixture.layout.control +
                 offsetof(protocol::Control, active_token) &&
         writes[first_write + 3] == kObject;
}

void SimulatePayloadSuccess(Fixture* fixture, bool skipped) {
  auto* control = At<protocol::Control>(&fixture->memory,
                                        fixture->layout.control);
  auto* evidence = At<protocol::Evidence>(&fixture->memory,
                                          fixture->layout.evidence);
  auto* audit = At<protocol::TransactionAudit>(
      &fixture->memory,
      fixture->layout.audits +
          sizeof(protocol::TransactionAudit) *
              fixture->session.armed_audit_index);
  *At<std::uintptr_t>(&fixture->memory, kObject) = kOriginalVptr;
  control->active_token = protocol::kDisarmedToken;

  *audit = {};
  audit->token = fixture->session.armed_token;
  audit->frame_index = fixture->session.armed_frame_index;
  audit->flags = protocol::kAuditFirstNaturalCallReturned |
                 protocol::kAuditSecondNaturalCallReturned |
                 protocol::kAuditImmediateExact |
                 protocol::kAuditTokenDisarmed |
                 protocol::kAuditVptrRestored |
                 (skipped ? protocol::kAuditSkipped
                          : protocol::kAuditOverridden);
  audit->tid = kTid;
  audit->natural_calls = 2;
  audit->before_bits[0] = 0x3dcccccd;
  audit->before_bits[1] = 0xbe4ccccd;
  audit->before_bits[2] = 0x3e99999a;
  const std::uint32_t* result_bits =
      skipped ? audit->before_bits
              : fixture->session.armed_target.angular_bits;
  std::memcpy(audit->immediate_bits, result_bits,
              sizeof(audit->immediate_bits));
  std::memcpy(audit->captured_bits, result_bits,
              sizeof(audit->captured_bits));

  *evidence = fixture->session.evidence_before_arm;
  evidence->wrapper_entries += 2;
  evidence->original_calls += 2;
  evidence->original_returns += 2;
  evidence->completed_transactions += 1;
  if (skipped)
    evidence->skipped_transactions += 1;
  else
    evidence->override_writes += 1;
  evidence->active_call_count = 0;
  evidence->audit_count = fixture->session.armed_audit_index + 1;
  evidence->last_status = protocol::kStatusComplete;
  evidence->last_tid = kTid;
  evidence->last_token = fixture->session.armed_token;
  evidence->last_object = kObject;
  evidence->final_vptr = kOriginalVptr;
}

bool HappyPath() {
  Fixture fixture{};
  if (!Setup(&fixture) || !InstalledExactly(fixture)) return false;
  const std::size_t writes_after_install = fixture.memory.writes.size();
  if (transport::BeginFrame(fixture.io, 0, kTid + 1u, &fixture.session) ||
      fixture.memory.writes.size() != writes_after_install ||
      fixture.session.faulted)
    return false;
  if (!transport::BeginFrame(fixture.io, 0, kTid, &fixture.session))
    return false;

  if (!transport::ArmFrame(fixture.io, 0, fixture.targets[0], kTid,
                           &fixture.session) ||
      !ArmWritesInOrder(fixture, writes_after_install) ||
      fixture.session.armed_sequence != 1 ||
      fixture.session.armed_token != protocol::ArmToken(kGeneration, 1) ||
      *At<std::uintptr_t>(&fixture.memory, kObject) !=
          fixture.session.prepared.shadow_vptr)
    return false;
  SimulatePayloadSuccess(&fixture, false);
  if (!transport::ReconcileCompletedWindow(fixture.io, kTid, false,
                                            &fixture.session) ||
      fixture.session.completed_transactions != 1 ||
      fixture.session.frame_armed || !fixture.session.frame_window_active)
    return false;

  const std::size_t second_arm_start = fixture.memory.writes.size();
  if (!transport::ArmFrame(fixture.io, 0, fixture.targets[0], kTid,
                           &fixture.session) ||
      !ArmWritesInOrder(fixture, second_arm_start) ||
      fixture.session.armed_sequence != 2 ||
      fixture.session.armed_token != protocol::ArmToken(kGeneration, 2))
    return false;
  SimulatePayloadSuccess(&fixture, false);
  if (!transport::ObserveFrameTerminalAtF64(fixture.io, kTid,
                                             &fixture.session) ||
      fixture.session.completed_transactions != 2 ||
      fixture.session.frame_armed || fixture.session.frame_window_active)
    return false;

  if (!transport::BeginFrame(fixture.io, 1, kTid, &fixture.session))
    return false;
  const std::size_t third_arm_start = fixture.memory.writes.size();
  if (!transport::ArmFrame(fixture.io, 1, fixture.targets[1], kTid,
                           &fixture.session) ||
      !ArmWritesInOrder(fixture, third_arm_start) ||
      fixture.session.armed_sequence != 3 ||
      fixture.session.armed_token != protocol::ArmToken(kGeneration, 3))
    return false;
  SimulatePayloadSuccess(&fixture, true);
  if (!transport::ObserveFrameTerminalAtF64(fixture.io, kTid,
                                             &fixture.session) ||
      fixture.session.completed_transactions != 3 ||
      fixture.session.frame_armed || fixture.session.frame_window_active)
    return false;
  const std::size_t writes_before_finish = fixture.memory.writes.size();
  return transport::Finish(fixture.io, kTid, true, &fixture.session) &&
         fixture.memory.writes.size() == writes_before_finish &&
         *At<std::uintptr_t>(&fixture.memory, kObject) == kOriginalVptr &&
         fixture.session.transaction.phase() == host::Phase::kRolledBack;
}

bool CancelAndLimitPath() {
  Fixture fixture{};
  if (!Setup(&fixture)) return false;
  const protocol::Evidence fresh =
      *At<protocol::Evidence>(&fixture.memory, fixture.layout.evidence);
  std::uint64_t first_token = 0;
  for (std::uint32_t attempt = 0; attempt < 2; ++attempt) {
    if (!transport::BeginFrame(fixture.io, 0, kTid, &fixture.session) ||
        !transport::ArmFrame(fixture.io, 0, fixture.targets[0], kTid,
                             &fixture.session))
      return false;
    if (attempt == 0) first_token = fixture.session.armed_token;
    const std::size_t before_cancel = fixture.memory.writes.size();
    if (!transport::CancelAbsentFrameAtF64(fixture.io, kTid,
                                            &fixture.session) ||
        fixture.memory.writes.size() != before_cancel + 2 ||
        fixture.memory.writes[before_cancel] != kObject ||
        fixture.memory.writes[before_cancel + 1] !=
            fixture.layout.control +
                offsetof(protocol::Control, active_token) ||
        *At<std::uintptr_t>(&fixture.memory, kObject) != kOriginalVptr ||
        !host::EvidenceUnchanged(
            fresh, *At<protocol::Evidence>(&fixture.memory,
                                            fixture.layout.evidence)))
      return false;
  }
  if (first_token != protocol::ArmToken(kGeneration, 1) ||
      fixture.session.issued_arm_sequences != 2)
    return false;
  if (!transport::BeginFrame(fixture.io, 0, kTid, &fixture.session))
    return false;
  const std::size_t before_limit = fixture.memory.writes.size();
  if (transport::ArmFrame(fixture.io, 0, fixture.targets[0], kTid,
                          &fixture.session) ||
      fixture.memory.writes.size() != before_limit || fixture.session.faulted)
    return false;
  if (!transport::ObserveIdleAtF64(fixture.io, kTid, &fixture.session))
    return false;
  const std::size_t before_finish = fixture.memory.writes.size();
  return transport::Finish(fixture.io, kTid, false, &fixture.session) &&
         fixture.memory.writes.size() == before_finish &&
         *At<std::uintptr_t>(&fixture.memory, kObject) == kOriginalVptr;
}

bool EarlyReconcileThenSettledF64Path() {
  Fixture fixture{};
  if (!Setup(&fixture) ||
      !transport::BeginFrame(fixture.io, 0, kTid, &fixture.session) ||
      !transport::ArmFrame(fixture.io, 0, fixture.targets[0], kTid,
                           &fixture.session))
    return false;
  SimulatePayloadSuccess(&fixture, false);
  if (!transport::ReconcileCompletedWindow(fixture.io, kTid, false,
                                            &fixture.session) ||
      fixture.session.frame_armed ||
      !fixture.session.frame_window_active ||
      !fixture.session.frame_window_ever_armed)
    return false;
  const std::size_t writes_before_f64 = fixture.memory.writes.size();
  return transport::ObserveSettledAtF64(fixture.io, kTid,
                                         &fixture.session) &&
         !fixture.session.frame_window_active &&
         fixture.memory.writes.size() == writes_before_f64 &&
         transport::Finish(fixture.io, kTid, true, &fixture.session);
}

bool PreflightDriftPaths() {
  Fixture table_fixture{};
  if (!Setup(&table_fixture) ||
      !transport::BeginFrame(table_fixture.io, 0, kTid,
                             &table_fixture.session))
    return false;
  *BytesAt(&table_fixture.memory,
           kOriginalTable + protocol::kShadowSize - 1, 1) ^= 0x5a;
  const std::size_t table_writes = table_fixture.memory.writes.size();
  if (transport::ArmFrame(table_fixture.io, 0, table_fixture.targets[0],
                          kTid, &table_fixture.session) ||
      !table_fixture.session.faulted ||
      table_fixture.memory.writes.size() != table_writes ||
      *At<std::uintptr_t>(&table_fixture.memory, kObject) != kOriginalVptr)
    return false;

  Fixture native_fixture{};
  if (!Setup(&native_fixture) ||
      !transport::BeginFrame(native_fixture.io, 0, kTid,
                             &native_fixture.session))
    return false;
  *At<std::uintptr_t>(&native_fixture.memory,
                      kObject + protocol::kNativeBodyPointerOffset) =
      kNativeBody + 0x1000;
  const std::size_t native_writes = native_fixture.memory.writes.size();
  return !transport::ArmFrame(native_fixture.io, 0,
                              native_fixture.targets[0], kTid,
                              &native_fixture.session) &&
         native_fixture.session.faulted &&
         native_fixture.memory.writes.size() == native_writes &&
         *At<std::uintptr_t>(&native_fixture.memory, kObject) == kOriginalVptr;
}

bool AlienAndPartialFailurePaths() {
  Fixture alien{};
  if (!Setup(&alien) ||
      !transport::BeginFrame(alien.io, 0, kTid, &alien.session) ||
      !transport::ArmFrame(alien.io, 0, alien.targets[0], kTid,
                           &alien.session))
    return false;
  const std::uintptr_t foreign_vptr = 0x55550000u;
  *At<std::uintptr_t>(&alien.memory, kObject) = foreign_vptr;
  const std::size_t before_alien_cancel = alien.memory.writes.size();
  if (transport::CancelAbsentFrameAtF64(alien.io, kTid, &alien.session) ||
      !alien.session.faulted ||
      alien.memory.writes.size() != before_alien_cancel ||
      *At<std::uintptr_t>(&alien.memory, kObject) != foreign_vptr)
    return false;

  Fixture partial{};
  if (!Setup(&partial) ||
      !transport::BeginFrame(partial.io, 0, kTid, &partial.session))
    return false;
  partial.memory.fail_address = kObject;
  partial.memory.fail_after_mutation = true;
  partial.memory.fail_once = true;
  if (transport::ArmFrame(partial.io, 0, partial.targets[0], kTid,
                          &partial.session) ||
      !partial.session.faulted || !partial.session.frame_armed ||
      *At<std::uintptr_t>(&partial.memory, kObject) !=
          partial.session.prepared.shadow_vptr ||
      At<protocol::Control>(&partial.memory, partial.layout.control)
              ->active_token != protocol::ArmToken(kGeneration, 1))
    return false;
  const std::size_t after_partial = partial.memory.writes.size();
  return !transport::CancelAbsentFrameAtF64(partial.io, kTid,
                                             &partial.session) &&
         !transport::Finish(partial.io, kTid, false, &partial.session) &&
         partial.memory.writes.size() == after_partial;
}

bool IdlePath() {
  Fixture fixture{};
  if (!Setup(&fixture) ||
      !transport::BeginFrame(fixture.io, 1, kTid, &fixture.session))
    return false;
  const protocol::Evidence snapshot = fixture.session.frame_start_evidence;
  const std::size_t writes_before_idle = fixture.memory.writes.size();
  if (!transport::ObserveIdleAtF64(fixture.io, kTid, &fixture.session) ||
      fixture.session.frame_window_active || fixture.session.frame_armed ||
      fixture.memory.writes.size() != writes_before_idle ||
      *At<std::uintptr_t>(&fixture.memory, kObject) != kOriginalVptr ||
      !host::EvidenceUnchanged(
          snapshot, *At<protocol::Evidence>(&fixture.memory,
                                             fixture.layout.evidence)))
    return false;
  return transport::Finish(fixture.io, kTid, false, &fixture.session);
}

}  // namespace

int main() {
  const bool happy = HappyPath();
  const bool cancel_limit = CancelAndLimitPath();
  const bool drift = PreflightDriftPaths();
  const bool fail_closed = AlienAndPartialFailurePaths();
  const bool idle = IdlePath();
  const bool settled = EarlyReconcileThenSettledF64Path();
  const bool passed = happy && cancel_limit && drift && fail_closed && idle &&
                      settled;
  std::printf(
      "BARREL_YAW_TAIL_TRANSPORT_SELFTEST passed=%u install_config_only=%u "
      "full_shadow_1c0=%u native_binding_each_arm=%u "
      "indices_then_token_then_shadow=%u exact_f64_receipt=%u "
      "vptr_restored_audit=%u absent_evidence_unchanged=%u "
      "restore_before_disarm=%u monotonic_sequence=%u per_frame_limit=%u "
      "same_frame_reconcile=%u idle_frame_receipt=%u "
      "settled_f64_receipt=%u "
      "alien_fail_closed=%u partial_write_fail_closed=%u "
      "finish_original_unarmed=%u runtime=disabled\n",
      passed ? 1u : 0u, happy ? 1u : 0u, happy ? 1u : 0u,
      drift ? 1u : 0u, happy ? 1u : 0u, happy ? 1u : 0u,
      happy ? 1u : 0u, cancel_limit ? 1u : 0u,
      cancel_limit ? 1u : 0u, cancel_limit ? 1u : 0u,
      cancel_limit ? 1u : 0u, happy ? 1u : 0u, idle ? 1u : 0u,
      settled ? 1u : 0u,
      fail_closed ? 1u : 0u,
      fail_closed ? 1u : 0u, happy ? 1u : 0u);
  return passed ? 0 : 1;
}
