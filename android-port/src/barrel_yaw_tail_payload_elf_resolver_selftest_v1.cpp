#include "barrel_yaw_tail_payload_elf_resolver_v1.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace resolver = a9tas::barrel_yaw_tail_elf_v1;
namespace detail = a9tas::barrel_yaw_tail_elf_v1::detail;
namespace protocol = a9tas::barrel_yaw_tail_payload_v1;

namespace {

constexpr std::uintptr_t kBias = 0x7000000000ull;
constexpr std::uint64_t kFileSize = 0x3A68;

Elf64_Ehdr ExactHeader() {
  Elf64_Ehdr header{};
  std::memcpy(header.e_ident, ELFMAG, SELFMAG);
  header.e_ident[EI_CLASS] = ELFCLASS64;
  header.e_ident[EI_DATA] = ELFDATA2LSB;
  header.e_ident[EI_VERSION] = EV_CURRENT;
  header.e_type = ET_DYN;
  header.e_machine = EM_AARCH64;
  header.e_version = EV_CURRENT;
  header.e_ehsize = sizeof(Elf64_Ehdr);
  header.e_phoff = sizeof(Elf64_Ehdr);
  header.e_phentsize = sizeof(Elf64_Phdr);
  header.e_phnum = 10;
  return header;
}

std::vector<Elf64_Phdr> ExactPrograms() {
  std::vector<Elf64_Phdr> programs(10);
  programs[1] = {PT_LOAD, PF_R, 0x0000, 0x0000, 0x0000,
                 0x0D64, 0x0D64, 0x1000};
  programs[2] = {PT_LOAD, PF_R | PF_X, 0x0D70, 0x1D70, 0x1D70,
                 0x0F90, 0x0F90, 0x1000};
  programs[3] = {PT_LOAD, PF_R | PF_W, 0x1D00, 0x3D00, 0x3D00,
                 0x0228, 0x0300, 0x1000};
  programs[4] = {PT_LOAD, PF_R | PF_W,
                 resolver::kFinalRwOffset, resolver::kFinalRwVaddr,
                 resolver::kFinalRwVaddr, resolver::kFinalRwFileSize,
                 resolver::kFinalRwMemorySize, 0x1000};
  return programs;
}

bool ParseLines(const std::array<const char*, 5>& lines,
                std::vector<resolver::Mapping>* maps) {
  if (maps == nullptr) return false;
  maps->clear();
  for (const char* line : lines) {
    resolver::Mapping mapping{};
    if (!detail::ParseMappingLine(line, &mapping)) return false;
    maps->push_back(std::move(mapping));
  }
  return true;
}

std::array<const char*, 5> NativeSplitMapLines() {
  return {
      "7000000000-7000001000 r--p 00000000 fd:01 424242 "
      "/data/local/tmp/liba9tas_barrel_yaw_tail_v1_build_only.so",
      "7000001000-7000003000 r-xp 00000000 fd:01 424242 "
      "/data/local/tmp/liba9tas_barrel_yaw_tail_v1_build_only.so",
      "7000003000-7000004000 r--p 00001000 fd:01 424242 "
      "/data/local/tmp/liba9tas_barrel_yaw_tail_v1_build_only.so",
      "7000004000-7000006000 rw-p 00001000 fd:01 424242 "
      "/data/local/tmp/liba9tas_barrel_yaw_tail_v1_build_only.so",
      "7000006000-7000084000 rw-p 00000000 00:00 0",
  };
}

std::array<const char*, 5> HoudiniSplitMapLines() {
  auto lines = NativeSplitMapLines();
  lines[1] =
      "7000001000-7000003000 r--p 00000000 fd:01 424242 "
      "/data/local/tmp/liba9tas_barrel_yaw_tail_v1_build_only.so";
  lines[4] =
      "7000006000-7000084000 rw-p 00000000 00:00 0 [anon:.bss]";
  return lines;
}

bool PositiveSplit(const std::array<const char*, 5>& lines) {
  resolver::LoadPlan plan{};
  if (!detail::BuildLoadPlan(ExactHeader(), ExactPrograms(), kFileSize,
                             &plan) ||
      plan.final_rw.p_vaddr != resolver::kFinalRwVaddr ||
      plan.file_page_end_rva != 0x6000 ||
      plan.logical_end_rva != resolver::kFinalRwMemoryEndRva ||
      plan.memory_page_end_rva != 0x84000)
    return false;
  std::vector<resolver::Mapping> maps;
  if (!ParseLines(lines, &maps)) return false;
  resolver::FileIdentity identity{};
  std::uintptr_t bias = 0;
  if (!detail::SelectPayloadFile(maps, &identity) ||
      identity.dev_major != 0xFD || identity.dev_minor != 1 ||
      identity.inode != 424242 ||
      !detail::DeriveAndValidateLoadBias(maps, identity, plan, &bias) ||
      bias != kBias ||
      !detail::ValidateFinalRwSplit(maps, identity, bias, plan) ||
      !detail::ExecutableRange(maps, bias + resolver::kBoundaryRva,
                               resolver::kBoundarySize, identity, bias,
                               plan) ||
      !detail::FileBackedRwRange(
          maps, bias + resolver::kEvidenceLocatorRva, sizeof(std::uintptr_t),
          identity, bias, plan))
    return false;
  std::uintptr_t storage[5] = {
      bias + resolver::kShadowStorageRva,
      bias + resolver::kControlStorageRva,
      bias + resolver::kTargetsStorageRva,
      bias + resolver::kAuditsStorageRva,
      bias + resolver::kEvidenceStorageRva,
  };
  if (!detail::ValidateStorageLayout(storage, bias, plan)) return false;
  storage[2] -= 0x40;
  if (detail::ValidateStorageLayout(storage, bias, plan)) return false;
  return detail::WritableRange(
             maps, bias + resolver::kShadowStorageRva,
             protocol::kShadowSize, identity, bias, plan) &&
         detail::WritableRange(
             maps, bias + resolver::kAuditsStorageRva,
             protocol::kMaximumTransactions *
                 sizeof(protocol::TransactionAudit),
             identity, bias, plan) &&
         detail::WritableRange(
             maps, bias + resolver::kTargetsStorageRva,
             protocol::kMaximumFrames * sizeof(protocol::FrameTarget),
             identity, bias, plan) &&
         !detail::WritableRange(
             maps, bias + resolver::kFinalRwMemoryEndRva - 8, 16,
             identity, bias, plan);
}

bool LocatorModes() {
  constexpr std::uintptr_t expected[5] = {
      kBias + resolver::kShadowStorageRva,
      kBias + resolver::kControlStorageRva,
      kBias + resolver::kTargetsStorageRva,
      kBias + resolver::kAuditsStorageRva,
      kBias + resolver::kEvidenceStorageRva,
  };
  std::uintptr_t zero[5]{};
  std::uintptr_t relocated[5]{};
  std::copy(std::begin(expected), std::end(expected), relocated);
  std::uintptr_t resolved[5]{};
  if (!detail::ResolveLocatorStorage(zero, kBias, resolved) ||
      !std::equal(std::begin(expected), std::end(expected), resolved) ||
      !detail::ResolveLocatorStorage(relocated, kBias, resolved) ||
      !std::equal(std::begin(expected), std::end(expected), resolved))
    return false;
  relocated[2] = 0;
  if (detail::ResolveLocatorStorage(relocated, kBias, resolved)) return false;
  relocated[2] = expected[2] + 8;
  return !detail::ResolveLocatorStorage(relocated, kBias, resolved);
}

bool StartTimeParser() {
  constexpr char good[] =
      "123 (worker name) R 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 "
      "424242 23\n";
  constexpr char zero[] =
      "123 (worker) R 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 0\n";
  constexpr char malformed[] = "123 worker R 1 2 3\n";
  std::uint64_t value = 0;
  return detail::ParseProcessStartTime(good, &value) && value == 424242 &&
         !detail::ParseProcessStartTime(zero, &value) &&
         !detail::ParseProcessStartTime(malformed, &value);
}

bool RejectProgramHeaderDrift() {
  auto programs = ExactPrograms();
  programs[4].p_memsz += 0x1000;
  resolver::LoadPlan plan{};
  if (detail::BuildLoadPlan(ExactHeader(), programs, kFileSize, &plan))
    return false;
  programs = ExactPrograms();
  programs[2].p_flags = PF_R;
  return !detail::BuildLoadPlan(ExactHeader(), programs, kFileSize, &plan);
}

bool RejectMappingDrift() {
  resolver::LoadPlan plan{};
  if (!detail::BuildLoadPlan(ExactHeader(), ExactPrograms(), kFileSize,
                             &plan))
    return false;
  std::vector<resolver::Mapping> exact;
  if (!ParseLines(HoudiniSplitMapLines(), &exact)) return false;
  resolver::FileIdentity identity{};
  std::uintptr_t bias = 0;
  if (!detail::SelectPayloadFile(exact, &identity) ||
      !detail::DeriveAndValidateLoadBias(exact, identity, plan, &bias))
    return false;

  auto gap = exact;
  gap.back().begin += 0x1000;
  auto executable_anon = exact;
  executable_anon.back().perms[2] = 'x';
  auto overrun = exact;
  overrun.back().end += 0x1000;
  auto wrong_named_anon = exact;
  wrong_named_anon.back().path = "[anon:evil]";
  auto wrong_inode = exact;
  wrong_inode[3].inode += 1;
  auto writable_boundary = exact;
  writable_boundary[1].perms[1] = 'w';
  return !detail::ValidateFinalRwSplit(gap, identity, bias, plan) &&
         !detail::ValidateFinalRwSplit(executable_anon, identity, bias,
                                       plan) &&
         !detail::ValidateFinalRwSplit(overrun, identity, bias, plan) &&
         !detail::ValidateFinalRwSplit(wrong_named_anon, identity, bias,
                                       plan) &&
         !detail::DeriveAndValidateLoadBias(wrong_inode, identity, plan,
                                            &bias) &&
         !detail::ExecutableRange(writable_boundary,
                                  kBias + resolver::kBoundaryRva,
                                  resolver::kBoundarySize, identity, kBias,
                                  plan);
}

bool RejectPathSpoof() {
  std::vector<resolver::Mapping> maps;
  if (!ParseLines(NativeSplitMapLines(), &maps)) return false;
  maps[0].path += " (deleted)";
  resolver::FileIdentity identity{};
  return !detail::SelectPayloadFile(maps, &identity);
}

}  // namespace

int main() {
  const bool native_positive = PositiveSplit(NativeSplitMapLines());
  const bool houdini_positive = PositiveSplit(HoudiniSplitMapLines());
  const bool positive = native_positive && houdini_positive;
  const bool program_drift = RejectProgramHeaderDrift();
  const bool mapping_drift = RejectMappingDrift();
  const bool path_spoof = RejectPathSpoof();
  const bool locator_modes = LocatorModes();
  const bool start_time = StartTimeParser();
  const bool passed = positive && program_drift && mapping_drift && path_spoof &&
                      locator_modes && start_time;
  std::printf(
      "BARREL_YAW_TAIL_ELF_RESOLVER_SELFTEST passed=%u "
      "hash_pinned_phdr=%u real_file_anon_split=%u dev_inode_path=%u "
      "anonymous_rw_private_no_x=%u contiguous_no_hole=%u "
      "logical_end_83bd0=%u exact_nonoverlap_storage=%u "
      "boundary_full_484_guest_code=%u native_rx=%u houdini_ro=%u "
      "named_bss_exact=%u locator_zero_or_relocated=%u "
      "pid_start_time=%u runtime=disabled\n",
      passed ? 1u : 0u, program_drift ? 1u : 0u,
      positive ? 1u : 0u, mapping_drift ? 1u : 0u,
      mapping_drift ? 1u : 0u, mapping_drift ? 1u : 0u,
      positive ? 1u : 0u, positive ? 1u : 0u,
      mapping_drift ? 1u : 0u, native_positive ? 1u : 0u,
      houdini_positive ? 1u : 0u, houdini_positive ? 1u : 0u,
      locator_modes ? 1u : 0u, start_time ? 1u : 0u);
  return passed ? 0 : 1;
}
