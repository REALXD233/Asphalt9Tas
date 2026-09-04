#define A9TAS_HABI1_CONTROLLER_CORE_ONLY 1
#include "habi1_one_shot_controller.cpp"

#include "g2_physics_interval_passthrough_v1.h"
#include "physics_interval_getter_shadow_transaction_v2.h"
#include "ptrace_stable_freeze_v1.h"
#include "remote_command_call_contract_v1.h"
#if defined(__aarch64__) && defined(__ANDROID__)
#include "native_arm64_command_backend_v1.h"
#include "native_arm64_remote_call_v1.h"
#endif

#include <sys/ptrace.h>

namespace g2_controller_v1 {

namespace protocol = a9tas::g2_physics_interval_v1;
namespace interval = a9tas::physics_interval_shadow_v2;
namespace command_contract = a9tas::remote_command_call_contract_v1;
namespace stable_freeze = a9tas::ptrace_stable_freeze_v1;

constexpr char kAcknowledgement[] = "I_ACCEPT_G2_PHYSICS_INTERVAL_V1";
constexpr char kBootstrapLocatorSymbol[] =
    "a9tas_bootstrap_g2_physics_interval_locator_v1";
constexpr char kBootstrapTrapSymbol[] =
    "a9tas_bootstrap_g2_physics_interval_return_trap_v1";
constexpr char kBootstrapCalibrateSymbol[] =
    "a9tas_bootstrap_g2_physics_interval_calibrate_tid_v1";
constexpr char kCommandSymbol[] = "a9tas_g2_physics_interval_command_v1";
constexpr char kControlLocatorSymbol[] =
    "a9tas_g2_physics_interval_control_data_v1";
constexpr char kEvidenceLocatorSymbol[] =
    "a9tas_g2_physics_interval_evidence_data_v1";
constexpr char kEventsLocatorSymbol[] =
    "a9tas_g2_physics_interval_events_data_v1";
constexpr char kWrapperLocatorSymbol[] =
    "a9tas_g2_physics_interval_wrapper_data_v1";
constexpr std::uint64_t kLocatorMagic = 0x47325049314c4f43ULL;
constexpr std::uintptr_t kTargetRva = 0x3695474;
constexpr std::int64_t kStepOptionsThisAdjustment = -0x2A78;
constexpr std::uintptr_t kImplementationOwnerVtableRva = 0x7EECD88;
constexpr std::uint32_t kInstallTag = 0x47324901;
constexpr std::uint32_t kRestoreTag = 0x47324902;

constexpr std::uint8_t kExpectedPrologue[16] = {
    0xff, 0xc3, 0x00, 0xd1, 0xf5, 0x53, 0x01, 0xa9,
    0xf3, 0x7b, 0x02, 0xa9, 0x35, 0x11, 0x91, 0x52,
};

void AbsoluteJump(std::uint8_t output[16], std::uintptr_t destination);

enum class Action : std::uint32_t { kInstall = 1, kStatus = 2, kRestore = 3 };

struct Runtime {
    std::uintptr_t game_base{};
    std::uintptr_t target{};
    std::uintptr_t outer_owner{};
    std::uintptr_t outer_owner_vptr{};
    std::uintptr_t owner{};
    std::uintptr_t owner_vptr{};
    std::uintptr_t payload_base{};
    std::uintptr_t control{};
    std::uintptr_t evidence{};
    std::uintptr_t events{};
    std::uintptr_t wrapper{};
    std::uintptr_t bootstrap_base{};
    Locator bootstrap{};
    std::uintptr_t guest_trampoline{};
#if defined(__aarch64__) && defined(__ANDROID__)
    std::uintptr_t native_command{};
    std::uintptr_t native_trap{};
#endif
    std::vector<Mapping> maps;
};

using FrozenSet = stable_freeze::FrozenSet;

struct Report {
    Action action{};
    pid_t pid{};
    pid_t tid{};
    std::uint64_t start_ticks{};
    std::uint32_t limit{};
    std::uint32_t freeze_passes{};
    std::uint32_t frozen_threads{};
    std::uintptr_t game_base{};
    std::uintptr_t owner{};
    std::uintptr_t owner_vptr{};
    std::uintptr_t target{};
    std::uintptr_t wrapper{};
    std::uintptr_t control{};
    std::uintptr_t evidence{};
    std::uint64_t guest_return{};
    long rip_bias{};
    protocol::Control control_value{};
    protocol::Evidence evidence_value{};
    bool process_identity{};
    bool artifact_identity{};
    bool runtime_identity{};
    bool stable_freeze{};
    bool guest_called{};
    bool detach_complete{};
    bool target_expected{};
    bool passed{};
};

bool ParseAction(const char* text, Action* output) {
    if (!text || !output) return false;
    if (std::strcmp(text, "install") == 0) *output = Action::kInstall;
    else if (std::strcmp(text, "status") == 0) *output = Action::kStatus;
    else if (std::strcmp(text, "restore") == 0) *output = Action::kRestore;
    else return false;
    return true;
}

bool ParseNumber(const char* text, int base, std::uint64_t* output) {
    if (!text || !*text || !output) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(text, &end, base);
    if (errno != 0 || !end || *end != '\0') return false;
    *output = value;
    return true;
}

// Kept as the internal compatibility surface used by the G4 controller.
// The implementation is shared with the native ARM64 backend.
bool FreezeStable(pid_t pid, pid_t call_tid, FrozenSet* frozen) {
    return stable_freeze::FreezeStable(pid, call_tid, frozen);
}

bool DetachAll(FrozenSet* frozen) {
    return stable_freeze::DetachAll(frozen);
}

bool WriteExact(int mem, std::uintptr_t address, const void* data,
                std::size_t size) {
    if (mem < 0 || address == 0 || !data || size == 0) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t amount = pwrite(mem, bytes + done, size - done,
                                      static_cast<off_t>(address + done));
        if (amount <= 0) return false;
        done += static_cast<std::size_t>(amount);
    }
    std::vector<std::uint8_t> readback(size);
    return pread(mem, readback.data(), size, static_cast<off_t>(address)) ==
               static_cast<ssize_t>(size) &&
           std::memcmp(readback.data(), data, size) == 0;
}

template <typename T>
bool ReadAt(int mem, std::uintptr_t address, T* output) {
    return output && pread(mem, output, sizeof(T), static_cast<off_t>(address)) ==
                         static_cast<ssize_t>(sizeof(T));
}

template <typename T>
bool ReadStableAt(int mem, std::uintptr_t address, T* output) {
    T first{}, second{};
    if (!ReadAt(mem, address, &first)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (!ReadAt(mem, address, &second) ||
        std::memcmp(&first, &second, sizeof(T)) != 0) return false;
    *output = first;
    return true;
}

bool DataAddressValid(const std::vector<Mapping>& maps,
                      std::uintptr_t address, std::size_t size) {
    if (address == 0 || size == 0 || address > UINTPTR_MAX - size)
        return false;
    const std::uintptr_t end = address + size;
    std::uintptr_t cursor = address;
    while (cursor < end) {
        const Mapping* mapping = FindMapping(maps, cursor, 1);
        if (!mapping || !mapping->readable || !mapping->writable ||
            mapping->executable || !mapping->private_mapping ||
            mapping->end <= cursor)
            return false;
        cursor = std::min(end, mapping->end);
    }
    return true;
}

bool ResolveLocatorAddress(const ElfImage& elf, pid_t pid,
                           std::uintptr_t base, const char* symbol,
                           std::uintptr_t* output) {
    Elf64_Sym entry{};
    std::uintptr_t value = 0;
    if (!output || !elf.ResolveUnique(symbol, true, &entry) ||
        ELF64_ST_TYPE(entry.st_info) != STT_OBJECT || entry.st_size != 8 ||
        !ReadProcessValue(pid, base + entry.st_value, &value) || value == 0)
        return false;
    *output = value;
    return true;
}

bool ResolveRuntime(pid_t pid, std::uint64_t start_ticks,
                    std::uintptr_t requested_game_base,
                    std::uintptr_t owner, Runtime* runtime) {
    if (!runtime || !IsAlive(pid, start_ticks) ||
        TracerPid(pid) != 0 || owner == 0 || (owner & 7u) != 0)
        return false;
    FileImage payload_file{}, bootstrap_file{};
    if (!ReadPinnedRegularFile(kPayloadPath, kPayloadSha256, &payload_file) ||
        !ReadPinnedRegularFile(kBootstrapPath, kBootstrapSha256,
                               &bootstrap_file)) return false;
    ElfImage payload_elf{}, bootstrap_elf{};
    if (!payload_elf.Parse(payload_file, EM_AARCH64) ||
        payload_elf.build_id() != kPayloadBuildId ||
        !bootstrap_elf.Parse(bootstrap_file, EM_X86_64) ||
        bootstrap_elf.build_id() != kBootstrapBuildId) return false;
    runtime->maps = ReadMaps(pid);
    if (runtime->maps.empty() ||
        !UniqueOffsetZeroBase(runtime->maps, kPayloadPath, payload_file.status,
                              &runtime->payload_base) ||
        !UniqueOffsetZeroBase(runtime->maps, kBootstrapPath,
                              bootstrap_file.status,
                              &runtime->bootstrap_base)) return false;

    Elf64_Sym locator_symbol{}, trap_symbol{}, calibrate_symbol{};
    Elf64_Sym stage_symbol{}, probe_symbol{}, trampoline_symbol{};
    if (!bootstrap_elf.ResolveUnique(kBootstrapLocatorSymbol, true,
                                     &locator_symbol) ||
        !bootstrap_elf.ResolveUnique(kBootstrapTrapSymbol, true,
                                     &trap_symbol) ||
        !bootstrap_elf.ResolveUnique(kBootstrapCalibrateSymbol, true,
                                     &calibrate_symbol) ||
        !bootstrap_elf.ResolveUnique(kStageSymbol, false, &stage_symbol) ||
        !bootstrap_elf.ResolveUnique(kProbeStatusSymbol, false,
                                     &probe_symbol) ||
        !bootstrap_elf.ResolveUnique(kTrampolineSymbol, false,
                                     &trampoline_symbol) ||
        !ReadStableLocator(pid, runtime->bootstrap_base +
                                locator_symbol.st_value,
                           &runtime->bootstrap) ||
        runtime->bootstrap.magic != kLocatorMagic ||
        runtime->bootstrap.version != 1 ||
        runtime->bootstrap.size != sizeof(Locator) ||
        !ExactString(runtime->bootstrap.payload_path,
                     sizeof(runtime->bootstrap.payload_path), kPayloadPath) ||
        !ExactString(runtime->bootstrap.payload_sha256,
                     sizeof(runtime->bootstrap.payload_sha256),
                     kPayloadSha256) ||
        !ExactString(runtime->bootstrap.payload_build_id,
                     sizeof(runtime->bootstrap.payload_build_id),
                     kPayloadBuildId) ||
        !ExactString(runtime->bootstrap.payload_source_sha256,
                     sizeof(runtime->bootstrap.payload_source_sha256),
                     kPayloadSourceSha256) ||
        !ExactString(runtime->bootstrap.run_symbol,
                     sizeof(runtime->bootstrap.run_symbol), kCommandSymbol) ||
        !ExactString(runtime->bootstrap.shorty,
                     sizeof(runtime->bootstrap.shorty), "JJ") ||
        runtime->bootstrap.stage_address !=
            runtime->bootstrap_base + stage_symbol.st_value ||
        runtime->bootstrap.probe_status_address !=
            runtime->bootstrap_base + probe_symbol.st_value ||
        runtime->bootstrap.trampoline_address !=
            runtime->bootstrap_base + trampoline_symbol.st_value ||
        runtime->bootstrap.stage_size != 4 ||
        runtime->bootstrap.probe_status_size != 4 ||
        runtime->bootstrap.trampoline_size != 8 ||
        runtime->bootstrap.reserved != 0 ||
        runtime->bootstrap.return_trap_address !=
            runtime->bootstrap_base + trap_symbol.st_value ||
        runtime->bootstrap.calibrate_tid_address !=
            runtime->bootstrap_base + calibrate_symbol.st_value)
        return false;
    int stage = 0, probe_status = 0;
    if (!ReadProcessValue(pid, runtime->bootstrap.stage_address, &stage) ||
        stage != 5 ||
        !ReadProcessValue(pid, runtime->bootstrap.probe_status_address,
                          &probe_status) || probe_status != 1 ||
        !ReadProcessValue(pid, runtime->bootstrap.trampoline_address,
                          &runtime->guest_trampoline) ||
        runtime->guest_trampoline == 0) return false;
    const Mapping* guest_trampoline_map =
        FindMapping(runtime->maps, runtime->guest_trampoline, 1);
    const Mapping* return_trap_map = FindMapping(
        runtime->maps, runtime->bootstrap.return_trap_address, 2);
    if (!guest_trampoline_map || !guest_trampoline_map->readable ||
        !guest_trampoline_map->executable || guest_trampoline_map->writable ||
        !guest_trampoline_map->private_mapping || !return_trap_map ||
        !return_trap_map->readable || !return_trap_map->executable ||
        return_trap_map->writable) return false;

    if (!ResolveLocatorAddress(payload_elf, pid, runtime->payload_base,
                               kControlLocatorSymbol, &runtime->control) ||
        !ResolveLocatorAddress(payload_elf, pid, runtime->payload_base,
                               kEvidenceLocatorSymbol, &runtime->evidence) ||
        !ResolveLocatorAddress(payload_elf, pid, runtime->payload_base,
                               kEventsLocatorSymbol, &runtime->events) ||
        !ResolveLocatorAddress(payload_elf, pid, runtime->payload_base,
                               kWrapperLocatorSymbol, &runtime->wrapper) ||
        !DataAddressValid(runtime->maps, runtime->control,
                          sizeof(protocol::Control)) ||
        !DataAddressValid(runtime->maps, runtime->evidence,
                          sizeof(protocol::Evidence)) ||
        !DataAddressValid(runtime->maps, runtime->events,
                          sizeof(protocol::Event) * protocol::kCapacity))
        return false;

    const Mapping* game = FindMapping(runtime->maps, requested_game_base, 1);
    if (!game || game->start != requested_game_base || game->offset != 0 ||
        !game->readable || game->writable ||
        game->path.find("libAsphalt9.so") == std::string::npos)
        return false;
    FileImage game_file{};
    if (!ReadPinnedRegularFile(game->path.c_str(),
                               "671522d4614abcce5c4da16ff8a177423fa67f3eace7b6f0652e9754403008f0",
                               &game_file)) return false;
    ElfImage game_elf{};
    if (!game_elf.Parse(game_file, EM_AARCH64) ||
        game_elf.build_id() !=
            "e5dd7ef24f52dff0e0040dc3b1320f267a3c3b3b") return false;
    runtime->game_base = requested_game_base;
    runtime->target = requested_game_base + kTargetRva;
    runtime->outer_owner = owner;
    if (!ReadProcessValue(pid, owner, &runtime->outer_owner_vptr) ||
        runtime->outer_owner_vptr != requested_game_base +
                                         interval::kStepOptionsVtableRva ||
        !DataAddressValid(runtime->maps, owner, sizeof(std::uintptr_t)))
        return false;
    std::int64_t this_adjustment = 0;
    if (runtime->outer_owner_vptr < sizeof(std::uintptr_t) * 4 ||
        !ReadProcessValue(pid, runtime->outer_owner_vptr -
                                  sizeof(std::uintptr_t) * 4,
                          &this_adjustment) ||
        this_adjustment != kStepOptionsThisAdjustment ||
        owner < static_cast<std::uintptr_t>(-kStepOptionsThisAdjustment))
        return false;
    runtime->owner = owner -
        static_cast<std::uintptr_t>(-kStepOptionsThisAdjustment);
    if (!ReadProcessValue(pid, runtime->owner, &runtime->owner_vptr) ||
        runtime->owner_vptr != requested_game_base +
                                   kImplementationOwnerVtableRva ||
        !DataAddressValid(runtime->maps, runtime->owner,
                          sizeof(std::uintptr_t)))
        return false;
    std::uint8_t observed[16]{}, patch[16]{};
    AbsoluteJump(patch, runtime->wrapper);
    return ReadProcessMemory(pid, runtime->target, observed,
                             sizeof(observed)) &&
           (std::memcmp(observed, kExpectedPrologue, sizeof(observed)) == 0 ||
            std::memcmp(observed, patch, sizeof(observed)) == 0);
}

void AbsoluteJump(std::uint8_t output[16], std::uintptr_t destination) {
    const std::uint32_t load = 0x58000051;
    const std::uint32_t branch = 0xd61f0220;
    std::memcpy(output, &load, 4);
    std::memcpy(output + 4, &branch, 4);
    std::memcpy(output + 8, &destination, 8);
}

bool CallGuest(pid_t pid, Runtime* runtime, FrozenSet* frozen,
               std::uint64_t command, std::uint32_t expected_tag,
               std::uint64_t* guest_return, long* selected_bias) {
    if (!runtime || !frozen || !guest_return || !selected_bias) return false;
#if defined(__aarch64__) && defined(__ANDROID__)
    (void)pid;
    if (runtime->native_command == 0 ||
        !a9tas::native_arm64_remote_call_v1::ReturnStopAddress(
            runtime->native_trap) ||
        frozen->call_tid <= 0) return false;
    a9tas::native_arm64_remote_call_v1::RegisterImage original{};
    if (!a9tas::native_arm64_remote_call_v1::GetRegisters(
            frozen->call_tid, &original)) return false;
    const command_contract::Request request{
        command, expected_tag, static_cast<std::uint32_t>(frozen->call_tid)};
    a9tas::native_arm64_command_backend_v1::Report report{};
    if (!a9tas::native_arm64_command_backend_v1::InvokeStopped(
            frozen->call_tid, original, runtime->native_command,
            runtime->native_trap, request, &report)) return false;
    *guest_return = report.call.return_value;
    *selected_bias = 0;
    return report.contract_match;
#else
    const std::vector<Mapping> maps = ReadMaps(pid);
    user_regs_struct original{};
    if (!GetRegs(frozen->call_tid, &original)) return false;
    const std::uint64_t zero_args[6]{};
    constexpr long biases[] = {0, 2, -2, 4};
    bool calibrated = false;
    CallReport call{};
    for (const long bias : biases) {
        if (RemoteCallOnce(frozen->call_tid, maps, original, bias,
                           runtime->bootstrap.calibrate_tid_address,
                           runtime->bootstrap.return_trap_address,
                           zero_args, &call) &&
            call.result == static_cast<std::uint64_t>(frozen->call_tid)) {
            *selected_bias = bias;
            calibrated = true;
            break;
        }
        if (!call.detach_safe) return false;
    }
    if (!calibrated) return false;
    const command_contract::Request request{
        command, expected_tag, static_cast<std::uint32_t>(frozen->call_tid)};
    std::uint64_t args[6]{};
    if (!command_contract::BuildJniArguments(request, args)) return false;
    if (!RemoteCallOnce(frozen->call_tid, maps, original, *selected_bias,
                        runtime->guest_trampoline,
                        runtime->bootstrap.return_trap_address,
                        args, &call) || !call.detach_safe)
        return false;
    *guest_return = call.result;
    return command_contract::Matches(call.result, request);
#endif
}

bool ReadReceipt(int mem, const Runtime& runtime, Report* report) {
    return report &&
           ReadStableAt(mem, runtime.control, &report->control_value) &&
           ReadStableAt(mem, runtime.evidence, &report->evidence_value);
}

bool WriteReport(const char* path, const Report& report) {
    if (!path || !*path || access(path, F_OK) == 0) return false;
    FILE* file = std::fopen(path, "wb");
    if (!file) return false;
    const bool ok = std::fwrite(&report, sizeof(report), 1, file) == 1 &&
                    std::fflush(file) == 0 && std::ferror(file) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!ok || !closed) {
        std::remove(path);
        return false;
    }
    return true;
}

int Fail(pid_t pid, FrozenSet* frozen, bool uncertain, const char* stage,
         int code) {
    bool detached = true;
    if (frozen) detached = stable_freeze::DetachAll(frozen);
    bool killed = false;
    if (uncertain || !detached) killed = KillUncertainProcess(pid);
    std::fprintf(stderr,
                 "G2_PHYSICS_INTERVAL_CONTROLLER passed=0 stage=%s code=%d "
                 "uncertain=%d detached=%d process_killed=%d errno=%d\n",
                 stage, code, uncertain ? 1 : 0, detached ? 1 : 0,
                 killed ? 1 : 0, errno);
    return code;
}

}  // namespace g2_controller_v1

#if !defined(A9TAS_G4_NATIVE_ARM64_CONTROLLER)
int main(int argc, char** argv) {
    using namespace g2_controller_v1;
    if (argc != 9 || std::strcmp(argv[8], kAcknowledgement) != 0) {
        std::fprintf(stderr,
            "usage: %s ACTION PID START_TICKS GAME_BASE_HEX OWNER_HEX LIMIT "
            "OUTPUT I_ACCEPT_G2_PHYSICS_INTERVAL_V1\n", argv[0]);
        return 2;
    }
    Action action{};
    std::uint64_t pid_raw = 0, ticks = 0, game_base = 0, owner = 0, limit = 0;
    if (!ParseAction(argv[1], &action) ||
        !ParseNumber(argv[2], 10, &pid_raw) ||
        !ParseNumber(argv[3], 10, &ticks) ||
        !ParseNumber(argv[4], 16, &game_base) ||
        !ParseNumber(argv[5], 16, &owner) ||
        !ParseNumber(argv[6], 10, &limit) ||
        pid_raw == 0 || pid_raw > INT32_MAX || ticks == 0 || game_base == 0 ||
        owner == 0 || limit == 0 || limit > protocol::kCapacity ||
        access(argv[7], F_OK) == 0) return 2;
    const pid_t pid = static_cast<pid_t>(pid_raw);
    Report report{};
    report.action = action;
    report.pid = pid;
    report.start_ticks = ticks;
    report.limit = static_cast<std::uint32_t>(limit);
    report.game_base = game_base;
    report.owner = owner;
    report.process_identity = IsAlive(pid, ticks) &&
                              TracerPid(pid) == 0;
    if (!report.process_identity) return Fail(pid, nullptr, false,
                                              "process_identity", 3);

    Runtime runtime{};
    if (!ResolveRuntime(pid, ticks, game_base, owner, &runtime))
        return Fail(pid, nullptr, false, "runtime_identity", 4);
    report.artifact_identity = true;
    report.runtime_identity = true;
    report.owner = runtime.owner;
    report.owner_vptr = runtime.owner_vptr;
    report.target = runtime.target;
    report.wrapper = runtime.wrapper;
    report.control = runtime.control;
    report.evidence = runtime.evidence;

    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid);
    const int mem = open(mem_path,
                         (action == Action::kStatus ? O_RDONLY : O_RDWR) |
                         O_CLOEXEC);
    if (mem < 0) return Fail(pid, nullptr, false, "open_mem", 5);

    if (action == Action::kStatus) {
        const bool read = ReadReceipt(mem, runtime, &report);
        std::uint8_t observed[16]{};
        const bool bytes = pread(mem, observed, sizeof(observed),
                                 static_cast<off_t>(runtime.target)) ==
                           static_cast<ssize_t>(sizeof(observed));
        std::uint8_t patch[16]{};
        AbsoluteJump(patch, runtime.wrapper);
        report.target_expected = bytes &&
            (std::memcmp(observed, patch, sizeof(patch)) == 0 ||
             std::memcmp(observed, kExpectedPrologue,
                         sizeof(kExpectedPrologue)) == 0);
        report.passed = read && report.target_expected;
        const bool written = WriteReport(argv[7], report);
        close(mem);
        std::printf("G2_PHYSICS_INTERVAL_STATUS passed=%d status=%d cursor=%u "
                    "completed=%u active=%u wrapper_returns=%llu "
                    "qualified=%llu unqualified=%llu valid_outputs=%llu "
                    "semantic_errors=%llu identity_errors=%llu "
                    "tid_changes=%llu\n", report.passed ? 1 : 0,
                    report.evidence_value.status, report.control_value.cursor,
                    report.control_value.completed,
                    report.control_value.active_calls,
                    static_cast<unsigned long long>(
                        report.evidence_value.wrapper_returns),
                    static_cast<unsigned long long>(
                        report.evidence_value.qualified_returns),
                    static_cast<unsigned long long>(
                        report.evidence_value.unqualified_returns),
                    static_cast<unsigned long long>(
                        report.evidence_value.valid_outputs),
                    static_cast<unsigned long long>(
                        report.evidence_value.semantic_errors),
                    static_cast<unsigned long long>(
                        report.evidence_value.identity_errors),
                    static_cast<unsigned long long>(
                        report.evidence_value.tid_changes));
        return report.passed && written ? 0 : 10;
    }

    const pid_t call_tid = UniqueSignalCatcher(pid);
    FrozenSet frozen{};
    if (call_tid <= 0 ||
        !stable_freeze::FreezeStable(pid, call_tid, &frozen)) {
        close(mem);
        return Fail(pid, &frozen, false, "freeze", 6);
    }
    report.tid = call_tid;
    report.freeze_passes = frozen.passes;
    report.frozen_threads = static_cast<std::uint32_t>(
        frozen.other_tids.size() + (frozen.call_attached ? 1 : 0));
    report.stable_freeze = true;

    Runtime stopped{};
    // TracerPid is intentionally nonzero while frozen, so revalidate the
    // immutable addresses and owner identity directly under the stop.
    bool stopped_identity = IsAlive(pid, ticks) &&
        ReadProcessValue(pid, runtime.outer_owner,
                         &stopped.outer_owner_vptr) &&
        stopped.outer_owner_vptr == runtime.outer_owner_vptr &&
        ReadProcessValue(pid, runtime.owner, &stopped.owner_vptr) &&
        stopped.owner_vptr == runtime.owner_vptr;
    std::uint8_t stopped_bytes[16]{};
    stopped_identity = stopped_identity &&
        pread(mem, stopped_bytes, sizeof(stopped_bytes),
              static_cast<off_t>(runtime.target)) ==
            static_cast<ssize_t>(sizeof(stopped_bytes));
    if (!stopped_identity) {
        close(mem);
        return Fail(pid, &frozen, false, "stopped_identity", 7);
    }

    bool command_ok = false;
    if (action == Action::kInstall) {
        if (std::memcmp(stopped_bytes, kExpectedPrologue,
                        sizeof(kExpectedPrologue)) != 0) {
            close(mem);
            return Fail(pid, &frozen, false, "install_precondition", 8);
        }
        protocol::Control control{};
        std::memcpy(control.magic, protocol::kControlMagic,
                    sizeof(protocol::kControlMagic));
        control.version = protocol::kVersion;
        control.size = sizeof(control);
        control.limit = static_cast<std::uint32_t>(limit);
        control.generation = 1;
        control.expected_owner = runtime.owner;
        control.expected_owner_vptr = runtime.owner_vptr;
        std::vector<protocol::Event> blank(static_cast<std::size_t>(limit));
        if (!WriteExact(mem, runtime.events, blank.data(),
                        blank.size() * sizeof(blank[0])) ||
            !WriteExact(mem, runtime.control, &control, sizeof(control))) {
            close(mem);
            return Fail(pid, &frozen, false, "configure", 9);
        }
        command_ok = CallGuest(pid, &runtime, &frozen,
                               static_cast<std::uint64_t>(
                                   protocol::Command::kInstallAndArm),
                               kInstallTag, &report.guest_return,
                               &report.rip_bias);
        report.guest_called = command_ok;
        std::uint8_t patch[16]{};
        AbsoluteJump(patch, runtime.wrapper);
        std::uint8_t readback[16]{};
        command_ok = command_ok &&
            pread(mem, readback, sizeof(readback),
                  static_cast<off_t>(runtime.target)) ==
                static_cast<ssize_t>(sizeof(readback)) &&
            std::memcmp(readback, patch, sizeof(patch)) == 0 &&
            ReadReceipt(mem, runtime, &report) &&
            report.control_value.enabled == 1 &&
            report.control_value.cursor == 0 &&
            report.control_value.active_calls == 0 &&
            report.evidence_value.status == protocol::kStatusArmed;
        report.target_expected = command_ok;
    } else {
        std::uint8_t patch[16]{};
        AbsoluteJump(patch, runtime.wrapper);
        if (std::memcmp(stopped_bytes, patch, sizeof(patch)) != 0) {
            close(mem);
            return Fail(pid, &frozen, false, "restore_precondition", 8);
        }
        command_ok = CallGuest(pid, &runtime, &frozen,
                               static_cast<std::uint64_t>(
                                   protocol::Command::kRestore),
                               kRestoreTag, &report.guest_return,
                               &report.rip_bias);
        report.guest_called = command_ok;
        std::uint8_t readback[16]{};
        command_ok = command_ok &&
            pread(mem, readback, sizeof(readback),
                  static_cast<off_t>(runtime.target)) ==
                static_cast<ssize_t>(sizeof(readback)) &&
            std::memcmp(readback, kExpectedPrologue,
                        sizeof(kExpectedPrologue)) == 0 &&
            ReadReceipt(mem, runtime, &report) &&
            report.control_value.enabled == 0 &&
            report.control_value.active_calls == 0 &&
            report.evidence_value.status == protocol::kStatusRestored;
        report.target_expected = command_ok;
    }

    if (!command_ok) {
        close(mem);
        return Fail(pid, &frozen, true, "guest_command", 10);
    }
    report.detach_complete = stable_freeze::DetachAll(&frozen);
    if (!report.detach_complete) {
        close(mem);
        return Fail(pid, nullptr, true, "detach", 11);
    }
    report.passed = true;
    const bool written = WriteReport(argv[7], report);
    close(mem);
    std::printf("G2_PHYSICS_INTERVAL_CONTROLLER passed=%d action=%u pid=%d "
                "tid=%d frozen=%u freeze_passes=%u cursor=%u qualified=%llu "
                "rollback=1 detach=1\n", written ? 1 : 0,
                static_cast<unsigned>(action), pid, call_tid,
                report.frozen_threads, report.freeze_passes,
                report.control_value.cursor,
                static_cast<unsigned long long>(
                    report.evidence_value.qualified_returns));
    return written ? 0 : 12;
}
#endif
