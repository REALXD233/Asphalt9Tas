// Host-side transaction controller for the passive ARM64 physics-interval
// getter payload.  No guest function is called and no executable page is
// modified.  The only game-owned mutation is the active step-options object's
// vptr; payload-owned storage holds the shadow table, continuation, protocol,
// intervals, and evidence.

#include "physics_interval_getter_payload_elf_resolver_v2.h"
#include "physics_interval_getter_payload_v2.h"
#include "physics_interval_getter_shadow_transaction_v2.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace payload = a9tas::physics_interval_payload_v2;
namespace resolver = a9tas::physics_interval_getter_elf_v2;
namespace shadow = a9tas::physics_interval_shadow_v2;

namespace {

constexpr char kAcknowledgement[] =
    "I_ACCEPT_PHYSICS_INTERVAL_SHADOW_V2";
constexpr char kGameBasename[] = "libAsphalt9.so";
constexpr std::uint64_t kStopTimeoutMs = 2000;
constexpr std::uint64_t kDrainTimeoutMs = 500;
constexpr int kNumericArgumentIndices[5] = {2, 3, 4, 5, 7};

enum class Action : std::uint32_t {
    kPreflight = 0,
    kInstall = 1,
    kStatus = 2,
    kFinalize = 3,
    kRollback = 4,
};

enum ReportFlag : std::uint32_t {
    kProcessIdentity = 1u << 0,
    kGameIdentity = 1u << 1,
    kPayloadResolved = 1u << 2,
    kPlanReady = 1u << 3,
    kPayloadConfigured = 1u << 4,
    kProcessStopped = 1u << 5,
    kShadowInstalled = 1u << 6,
    kPayloadEnabled = 1u << 7,
    kEvidenceComplete = 1u << 8,
    kPayloadDisabled = 1u << 9,
    kCallsDrained = 1u << 10,
    kOriginalVptrFinal = 1u << 11,
    kProcessResumed = 1u << 12,
    kOutputWritten = 1u << 13,
};

struct Report {
    char magic[8];
    std::uint32_t version;
    std::uint32_t size;
    std::uint32_t action;
    std::uint32_t flags;
    std::uint64_t pid;
    std::uint64_t start_ticks;
    std::uint64_t game_base;
    std::uint64_t step_options;
    std::uint64_t original_vptr;
    std::uint64_t shadow_vptr;
    std::uint64_t original_getter;
    std::uint64_t wrapper;
    std::uint64_t payload_base;
    std::uint64_t control_address;
    std::uint64_t evidence_address;
    std::uint64_t intervals_address;
    std::uint64_t events_address;
    std::uint32_t mode;
    std::uint32_t limit;
    std::uint64_t payload_write_attempts;
    std::uint64_t game_write_attempts;
    std::uint64_t rollback_attempts;
    std::uint64_t read_errors;
    payload::Control control;
    payload::Evidence evidence;
    std::uint8_t payload_sha256[32];
};

struct Mapping {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    std::uint64_t offset{};
    char perms[5]{};
    std::string path;
};

static_assert(sizeof(payload::Control) == 64);
static_assert(sizeof(payload::Evidence) == 128);
static_assert(sizeof(payload::Event) == 64);

bool ParseUnsigned(const char* text, int base, std::uint64_t* output) {
    if (!text || !output || *text == '\0') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, base);
    if (errno != 0 || end == text || *end != '\0') return false;
    *output = static_cast<std::uint64_t>(value);
    return true;
}

bool ParseAction(const char* text, Action* output) {
    if (!text || !output) return false;
    if (std::strcmp(text, "preflight") == 0) *output = Action::kPreflight;
    else if (std::strcmp(text, "install") == 0) *output = Action::kInstall;
    else if (std::strcmp(text, "status") == 0) *output = Action::kStatus;
    else if (std::strcmp(text, "finalize") == 0)
        *output = Action::kFinalize;
    else if (std::strcmp(text, "rollback") == 0)
        *output = Action::kRollback;
    else
        return false;
    return true;
}

bool ParseMode(const char* text, payload::Mode* output) {
    if (!text || !output) return false;
    if (std::strcmp(text, "record") == 0) *output = payload::Mode::kRecord;
    else if (std::strcmp(text, "replay") == 0)
        *output = payload::Mode::kReplay;
    else
        return false;
    return true;
}

bool ReadExact(int fd, std::uintptr_t address, void* output,
               std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(output);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t count = pread(fd, bytes + done, size - done,
                                    static_cast<off_t>(address + done));
        if (count <= 0) return false;
        done += static_cast<std::size_t>(count);
    }
    return true;
}

bool WriteExactVerified(int fd, std::uintptr_t address, const void* input,
                        std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(input);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t count = pwrite(fd, bytes + done, size - done,
                                     static_cast<off_t>(address + done));
        if (count <= 0) return false;
        done += static_cast<std::size_t>(count);
    }
    std::vector<std::uint8_t> observed(size);
    return ReadExact(fd, address, observed.data(), observed.size()) &&
           std::memcmp(observed.data(), input, size) == 0;
}

bool ReadStartTicks(pid_t pid, std::uint64_t* output) {
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(pid));
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buffer[4096]{};
    const ssize_t count = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (count <= 0) return false;
    buffer[count] = '\0';
    char* cursor = std::strrchr(buffer, ')');
    if (!cursor) return false;
    ++cursor;
    for (int field = 3; field <= 22; ++field) {
        while (*cursor == ' ') ++cursor;
        if (*cursor == '\0' || *cursor == '\n') return false;
        char* end = cursor;
        while (*end != '\0' && *end != '\n' && *end != ' ') ++end;
        if (field == 22) {
            errno = 0;
            char* parsed = nullptr;
            const unsigned long long value = std::strtoull(cursor, &parsed, 10);
            if (errno != 0 || parsed != end || value == 0) return false;
            *output = static_cast<std::uint64_t>(value);
            return true;
        }
        cursor = end;
    }
    return false;
}

bool ReadMappings(pid_t pid, std::vector<Mapping>* output) {
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/maps", static_cast<int>(pid));
    FILE* file = std::fopen(path, "re");
    if (!file) return false;
    std::vector<Mapping> maps;
    char line[2048]{};
    while (std::fgets(line, sizeof(line), file)) {
        unsigned long long begin = 0, end = 0, offset = 0;
        char perms[5]{}, raw_path[1024]{};
        const int fields = std::sscanf(
            line, "%llx-%llx %4s %llx %*s %*s %1023[^\n]", &begin, &end,
            perms, &offset, raw_path);
        if (fields < 4 || begin >= end) continue;
        std::string clean = fields == 5 ? std::string(raw_path) : std::string();
        while (!clean.empty() && clean.front() == ' ') clean.erase(0, 1);
        Mapping mapping{static_cast<std::uintptr_t>(begin),
                        static_cast<std::uintptr_t>(end), offset, {}, clean};
        std::memcpy(mapping.perms, perms, 4);
        maps.push_back(std::move(mapping));
    }
    std::fclose(file);
    if (maps.empty()) return false;
    *output = std::move(maps);
    return true;
}

const Mapping* MappingAt(const std::vector<Mapping>& maps,
                         std::uintptr_t address, std::size_t size) {
    if (size == 0 || address > UINTPTR_MAX - size) return nullptr;
    const std::uintptr_t end = address + size;
    for (const auto& mapping : maps)
        if (address >= mapping.begin && end <= mapping.end) return &mapping;
    return nullptr;
}

bool EndsWith(const std::string& text, const char* suffix) {
    const std::size_t length = std::strlen(suffix);
    return text.size() >= length &&
           text.compare(text.size() - length, length, suffix) == 0;
}

bool GuestCodeMappingIdentity(const Mapping* base_map,
                              const Mapping* getter_map) {
    return base_map && getter_map && getter_map->path == base_map->path &&
           getter_map->perms[0] == 'r' && getter_map->perms[1] != 'w' &&
           getter_map->perms[3] == 'p';
}

bool GameIdentity(const std::vector<Mapping>& maps, std::uintptr_t base,
                  std::uintptr_t object) {
    if (base == 0 || object == 0 || (object & 7u) != 0 ||
        base > UINTPTR_MAX - shadow::kStepOptionsVtableRva ||
        base > UINTPTR_MAX - shadow::kGetterThunkRva)
        return false;
    const Mapping* base_map = MappingAt(maps, base, 1);
    const Mapping* table_map = MappingAt(
        maps, base + shadow::kStepOptionsVtableRva -
                  shadow::kNegativeWords * sizeof(std::uintptr_t),
        shadow::kShadowWords * sizeof(std::uintptr_t));
    const Mapping* getter_map =
        MappingAt(maps, base + shadow::kGetterThunkRva, 16);
    const Mapping* object_map = MappingAt(maps, object, sizeof(std::uintptr_t));
    return base_map && base_map->begin == base && base_map->offset == 0 &&
           EndsWith(base_map->path, kGameBasename) &&
           base_map->path.find(" (deleted)") == std::string::npos &&
           table_map && table_map->path == base_map->path &&
           table_map->perms[0] == 'r' && table_map->perms[1] != 'w' &&
           // LDPlayer 9 exposes guest ARM64 PT_LOAD code ranges as r--p in
           // /proc/<pid>/maps; Houdini executes their translated host code
           // elsewhere.  Requiring an x bit here rejects the exact supported
           // runtime before any write.  Keep the identity check strict by
           // requiring the getter to be readable, non-writable, private, and
           // backed by the same pinned libAsphalt9.so mapping.  BuildPlan then
           // proves the exact getter RVA and all adjacent vtable words.
           GuestCodeMappingIdentity(base_map, getter_map) &&
           object_map && object_map->perms[0] == 'r' &&
           object_map->perms[1] == 'w' && object_map->perms[3] == 'p';
}

std::uint64_t MonotonicMs() {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return static_cast<std::uint64_t>(now.tv_sec) * 1000u +
           static_cast<std::uint64_t>(now.tv_nsec) / 1000000u;
}

bool ProcessStopped(pid_t pid) {
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/status", static_cast<int>(pid));
    FILE* file = std::fopen(path, "re");
    if (!file) return false;
    char line[256]{};
    bool stopped = false;
    while (std::fgets(line, sizeof(line), file)) {
        char state = 0;
        if (std::sscanf(line, "State:\t%c", &state) == 1) {
            stopped = state == 'T' || state == 't';
            break;
        }
    }
    std::fclose(file);
    return stopped;
}

bool StopProcess(pid_t pid) {
    if (kill(pid, SIGSTOP) != 0) return false;
    const std::uint64_t deadline = MonotonicMs() + kStopTimeoutMs;
    while (MonotonicMs() < deadline) {
        if (ProcessStopped(pid)) return true;
        usleep(1000);
    }
    return false;
}

bool ResumeProcess(pid_t pid) { return kill(pid, SIGCONT) == 0; }

bool PayloadHeaders(const payload::Control& control,
                    const payload::Evidence& evidence) {
    return std::memcmp(control.magic, payload::kControlMagic, 8) == 0 &&
           control.version == payload::kVersion &&
           control.size == sizeof(payload::Control) &&
           std::memcmp(evidence.magic, payload::kEvidenceMagic, 8) == 0 &&
           evidence.version == payload::kVersion &&
           evidence.size == sizeof(payload::Evidence);
}

bool Configured(const payload::Control& control, payload::Mode mode,
                std::uint32_t limit, const shadow::Plan& plan) {
    return std::memcmp(control.magic, payload::kControlMagic, 8) == 0 &&
           control.version == payload::kVersion &&
           control.size == sizeof(payload::Control) &&
           control.mode == static_cast<std::uint32_t>(mode) &&
           control.limit == limit && control.expected_object == plan.step_options &&
           control.expected_vptr == plan.shadow_vptr &&
           control.original_getter == plan.original_getter;
}

bool Complete(const payload::Control& control,
              const payload::Evidence& evidence, payload::Mode mode,
              std::uint32_t limit) {
    const std::uint64_t mode_calls = mode == payload::Mode::kRecord
        ? evidence.record_calls : evidence.replay_calls;
    return control.completed == 1 && control.enabled == 0 &&
           control.active_calls == 0 && control.cursor == limit &&
           evidence.status == payload::kStatusComplete &&
           evidence.calls == limit && mode_calls == limit &&
           evidence.semantic_errors == 0 && evidence.object_mismatches == 0;
}

bool FloatBitsValid(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return std::isfinite(value) && value >= 0.001f && value <= 0.1f;
}

bool LoadReplayInput(const char* path, std::uint32_t limit,
                     std::vector<std::uint32_t>* output) {
    if (!path || !output || std::strcmp(path, "-") == 0) return false;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat info {};
    const std::size_t bytes = static_cast<std::size_t>(limit) * sizeof(std::uint32_t);
    std::vector<std::uint32_t> values(limit);
    const bool valid_size = fstat(fd, &info) == 0 &&
                            info.st_size == static_cast<off_t>(bytes);
    std::size_t done = 0;
    while (valid_size && done < bytes) {
        const ssize_t count = read(fd,
            reinterpret_cast<std::uint8_t*>(values.data()) + done, bytes - done);
        if (count <= 0) break;
        done += static_cast<std::size_t>(count);
    }
    close(fd);
    if (!valid_size || done != bytes ||
        !std::all_of(values.begin(), values.end(), FloatBitsValid))
        return false;
    *output = std::move(values);
    return true;
}

bool BuildPlan(int mem, const std::vector<Mapping>& maps,
               std::uintptr_t game_base, std::uintptr_t object,
               const resolver::Layout& payload_layout, shadow::Plan* output,
               std::uintptr_t* current_vptr) {
    if (!GameIdentity(maps, game_base, object) || !output || !current_vptr)
        return false;
    shadow::Observation observation{};
    observation.library_base = game_base;
    observation.step_options = object;
    observation.observed_vptr = game_base + shadow::kStepOptionsVtableRva;
    observation.payload_wrapper = payload_layout.wrapper;
    observation.payload_shadow_storage = payload_layout.shadow;
    observation.payload_continue_storage = payload_layout.continuation;
    if (!ReadExact(mem, object, current_vptr, sizeof(*current_vptr)) ||
        !ReadExact(mem,
            observation.observed_vptr -
                shadow::kNegativeWords * sizeof(std::uintptr_t),
            observation.original_words, sizeof(observation.original_words)))
        return false;
    const shadow::Plan plan = shadow::BuildPlan(observation);
    if (plan.status != shadow::Status::kReady) return false;
    *output = plan;
    return true;
}

bool RevalidateStopped(pid_t pid, std::uint64_t ticks, int mem,
                       std::uintptr_t game_base, std::uintptr_t object,
                       const resolver::Layout& expected_payload,
                       shadow::Plan* plan, std::uintptr_t* current_vptr) {
    std::uint64_t observed_ticks = 0;
    std::vector<Mapping> maps;
    resolver::Layout payload_layout{};
    return ReadStartTicks(pid, &observed_ticks) && observed_ticks == ticks &&
           ReadMappings(pid, &maps) &&
           resolver::Resolve(pid, mem, &payload_layout) &&
           payload_layout.load_bias == expected_payload.load_bias &&
           payload_layout.wrapper == expected_payload.wrapper &&
           payload_layout.control == expected_payload.control &&
           BuildPlan(mem, maps, game_base, object, payload_layout, plan,
                     current_vptr);
}

bool DisableAndDrain(int mem, const resolver::Layout& layout,
                     Report* report) {
    const std::uint32_t disabled = 0;
    ++report->payload_write_attempts;
    if (!WriteExactVerified(mem,
            layout.control + offsetof(payload::Control, enabled),
            &disabled, sizeof(disabled)))
        return false;
    report->flags |= kPayloadDisabled;
    const std::uint64_t deadline = MonotonicMs() + kDrainTimeoutMs;
    std::uint32_t consecutive_zero = 0;
    while (MonotonicMs() < deadline) {
        std::uint32_t active = UINT32_MAX;
        if (!ReadExact(mem,
                layout.control + offsetof(payload::Control, active_calls),
                &active, sizeof(active)))
            return false;
        if (active == 0) {
            if (++consecutive_zero >= 2) {
                report->flags |= kCallsDrained;
                return true;
            }
        } else {
            consecutive_zero = 0;
        }
        usleep(1000);
    }
    return false;
}

bool WriteOutput(const char* path, Report* report, int mem,
                 const resolver::Layout& layout, std::uint32_t count,
                 bool include_data) {
    if (!path || *path == '\0' || access(path, F_OK) == 0) return false;
    FILE* file = std::fopen(path, "wb");
    if (!file) return false;
    report->flags |= kOutputWritten;
    bool ok = std::fwrite(report, sizeof(*report), 1, file) == 1;
    if (ok && include_data && count != 0) {
        std::array<std::uint32_t, 128> intervals{};
        std::array<payload::Event, 32> events{};
        std::uint32_t cursor = 0;
        while (ok && cursor < count) {
            const std::uint32_t amount =
                std::min<std::uint32_t>(count - cursor, intervals.size());
            const std::size_t bytes = amount * sizeof(std::uint32_t);
            ok = ReadExact(mem, layout.intervals + cursor * sizeof(std::uint32_t),
                           intervals.data(), bytes) &&
                 std::fwrite(intervals.data(), bytes, 1, file) == 1;
            cursor += amount;
        }
        cursor = 0;
        while (ok && cursor < count) {
            const std::uint32_t amount =
                std::min<std::uint32_t>(count - cursor, events.size());
            const std::size_t bytes = amount * sizeof(payload::Event);
            ok = ReadExact(mem, layout.events + cursor * sizeof(payload::Event),
                           events.data(), bytes) &&
                 std::fwrite(events.data(), bytes, 1, file) == 1;
            cursor += amount;
        }
    }
    ok = ok && std::fflush(file) == 0 && std::ferror(file) == 0;
    const bool close_ok = std::fclose(file) == 0;
    if (!ok || !close_ok) {
        report->flags &= ~kOutputWritten;
        std::remove(path);
        return false;
    }
    return true;
}

int FailStage(int code, const char* stage) {
    std::fprintf(stderr,
                 "PHYSICS_INTERVAL_SHADOW_ERROR code=%d stage=%s errno=%d\n",
                 code, stage ? stage : "unknown", errno);
    return code;
}

}  // namespace

#if !defined(A9TAS_PHYSICS_INTERVAL_SHADOW_CONTROLLER_SELFTEST)
int main(int argc, char** argv) {
    if (argc != 11 || std::strcmp(argv[10], kAcknowledgement) != 0) {
        std::fprintf(stderr,
            "usage: %s ACTION PID START_TICKS GAME_BASE_HEX "
            "STEP_OPTIONS_HEX MODE LIMIT INPUT_OR_DASH OUTPUT "
            "I_ACCEPT_PHYSICS_INTERVAL_SHADOW_V2\n", argv[0]);
        return 2;
    }
    Action action{};
    payload::Mode mode{};
    std::uint64_t values[5]{};
    const int bases[5] = {10, 10, 16, 16, 10};
    if (!ParseAction(argv[1], &action) || !ParseMode(argv[6], &mode)) return 2;
    for (int i = 0; i < 5; ++i)
        if (!ParseUnsigned(argv[kNumericArgumentIndices[i]], bases[i],
                           &values[i]))
            return FailStage(2, "numeric_arguments");
    if (values[0] == 0 || values[0] > INT32_MAX || values[1] == 0 ||
        values[2] == 0 || values[3] == 0 || values[4] == 0 ||
        values[4] > payload::kCapacity || access(argv[9], F_OK) == 0)
        return 2;
    if (mode == payload::Mode::kRecord && std::strcmp(argv[8], "-") != 0)
        return 2;

    const pid_t pid = static_cast<pid_t>(values[0]);
    const std::uint64_t expected_ticks = values[1];
    const auto game_base = static_cast<std::uintptr_t>(values[2]);
    const auto object = static_cast<std::uintptr_t>(values[3]);
    const auto limit = static_cast<std::uint32_t>(values[4]);

    std::vector<std::uint32_t> replay_values;
    if (action == Action::kInstall && mode == payload::Mode::kReplay &&
        !LoadReplayInput(argv[8], limit, &replay_values))
        return 2;
    if (action != Action::kInstall && std::strcmp(argv[8], "-") != 0)
        return 2;

    Report report{};
    std::memcpy(report.magic, "A9PGTR2", 8);
    report.version = 2;
    report.size = sizeof(report);
    report.action = static_cast<std::uint32_t>(action);
    report.pid = values[0];
    report.start_ticks = expected_ticks;
    report.game_base = game_base;
    report.step_options = object;
    report.mode = static_cast<std::uint32_t>(mode);
    report.limit = limit;

    std::uint64_t ticks = 0;
    if (!ReadStartTicks(pid, &ticks) || ticks != expected_ticks)
        return FailStage(3, "process_identity");
    report.flags |= kProcessIdentity;
    std::vector<Mapping> maps;
    if (!ReadMappings(pid, &maps) || !GameIdentity(maps, game_base, object))
        return FailStage(3, "game_identity");
    report.flags |= kGameIdentity;

    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const bool read_only = action == Action::kPreflight ||
                           action == Action::kStatus;
    const int mem = open(mem_path,
        (read_only ? O_RDONLY : O_RDWR) | O_CLOEXEC);
    if (mem < 0) return FailStage(4, "open_process_mem");

    resolver::Layout layout{};
    if (!resolver::Resolve(pid, mem, &layout)) {
        close(mem);
        return FailStage(3, "payload_resolve");
    }
    report.flags |= kPayloadResolved;
    report.payload_base = layout.load_bias;
    report.wrapper = layout.wrapper;
    report.control_address = layout.control;
    report.evidence_address = layout.evidence;
    report.intervals_address = layout.intervals;
    report.events_address = layout.events;
    std::memcpy(report.payload_sha256, layout.file_sha256,
                sizeof(report.payload_sha256));

    shadow::Plan plan{};
    std::uintptr_t current_vptr = 0;
    if (!BuildPlan(mem, maps, game_base, object, layout, &plan, &current_vptr)) {
        close(mem);
        return FailStage(3, "shadow_plan");
    }
    report.flags |= kPlanReady;
    report.original_vptr = plan.original_vptr;
    report.shadow_vptr = plan.shadow_vptr;
    report.original_getter = plan.original_getter;

    if (action == Action::kPreflight) {
        if (current_vptr != plan.original_vptr || ProcessStopped(pid)) {
            close(mem);
            return FailStage(5, "preflight_precondition");
        }
        const bool written = WriteOutput(argv[9], &report, mem, layout,
                                         0, false);
        close(mem);
        std::printf(
            "PHYSICS_INTERVAL_SHADOW_PREFLIGHT object=0x%" PRIx64
            " original=0x%" PRIx64 " shadow=0x%" PRIx64
            " game_writes=0\n",
            report.step_options, report.original_vptr, report.shadow_vptr);
        return written ? 0 : FailStage(10, "preflight_output");
    }

    payload::Control control{};
    payload::Evidence evidence{};
    if (action == Action::kInstall) {
        if (current_vptr != plan.original_vptr || ProcessStopped(pid)) {
            close(mem);
            return FailStage(5, "install_precondition");
        }
        if (!StopProcess(pid)) {
            (void)ResumeProcess(pid);
            close(mem);
            return FailStage(6, "install_stop");
        }
        report.flags |= kProcessStopped;
        shadow::Plan stopped_plan{};
        std::uintptr_t stopped_vptr = 0;
        bool ok = RevalidateStopped(pid, expected_ticks, mem, game_base, object,
                                    layout, &stopped_plan, &stopped_vptr) &&
                  stopped_vptr == plan.original_vptr &&
                  stopped_plan.shadow_vptr == plan.shadow_vptr &&
                  stopped_plan.wrapper == plan.wrapper;
        std::memcpy(control.magic, payload::kControlMagic, 8);
        control.version = payload::kVersion;
        control.size = sizeof(control);
        control.mode = static_cast<std::uint32_t>(mode);
        control.enabled = 0;
        control.limit = limit;
        control.expected_object = object;
        control.expected_vptr = plan.shadow_vptr;
        control.original_getter = plan.original_getter;
        std::memcpy(evidence.magic, payload::kEvidenceMagic, 8);
        evidence.version = payload::kVersion;
        evidence.size = sizeof(evidence);
        evidence.status = payload::kStatusArmed;
        std::vector<payload::Event> blank_events(limit);
        std::vector<std::uint32_t> intervals = mode == payload::Mode::kReplay
            ? replay_values : std::vector<std::uint32_t>(limit, 0);
        if (ok) {
            report.payload_write_attempts += 6;
            ok = WriteExactVerified(mem, layout.shadow, plan.shadow_words,
                                    sizeof(plan.shadow_words)) &&
                 WriteExactVerified(mem, layout.continuation,
                                    &plan.original_getter,
                                    sizeof(plan.original_getter)) &&
                 WriteExactVerified(mem, layout.evidence, &evidence,
                                    sizeof(evidence)) &&
                 WriteExactVerified(mem, layout.intervals, intervals.data(),
                                    intervals.size() * sizeof(intervals[0])) &&
                 WriteExactVerified(mem, layout.events, blank_events.data(),
                                    blank_events.size() * sizeof(blank_events[0])) &&
                 WriteExactVerified(mem, layout.control, &control,
                                    sizeof(control));
        }
        if (ok) report.flags |= kPayloadConfigured;
        if (ok) {
            ++report.game_write_attempts;
            ok = WriteExactVerified(mem, object, &plan.shadow_vptr,
                                    sizeof(plan.shadow_vptr));
        }
        if (ok) {
            std::array<std::uintptr_t, shadow::kShadowWords> observed_words{};
            std::uintptr_t observed_continue = 0, observed_vptr = 0;
            ok = ReadExact(mem, layout.shadow, observed_words.data(),
                           sizeof(observed_words)) &&
                 ReadExact(mem, layout.continuation, &observed_continue,
                           sizeof(observed_continue)) &&
                 ReadExact(mem, object, &observed_vptr,
                           sizeof(observed_vptr)) &&
                 shadow::VerifyArmed(plan, observed_vptr, observed_continue,
                                     observed_words.data());
        }
        if (ok) report.flags |= kShadowInstalled;
        const std::uint32_t enabled = 1;
        if (ok) {
            ++report.payload_write_attempts;
            ok = WriteExactVerified(mem,
                layout.control + offsetof(payload::Control, enabled),
                &enabled, sizeof(enabled));
        }
        if (ok) report.flags |= kPayloadEnabled;
        if (!ok) {
            std::uintptr_t observed_vptr = 0;
            if (ReadExact(mem, object, &observed_vptr, sizeof(observed_vptr)) &&
                observed_vptr == plan.shadow_vptr) {
                ++report.rollback_attempts;
                ++report.game_write_attempts;
                (void)WriteExactVerified(mem, object, &plan.original_vptr,
                                         sizeof(plan.original_vptr));
            }
            const std::uint32_t disabled = 0;
            (void)WriteExactVerified(mem,
                layout.control + offsetof(payload::Control, enabled),
                &disabled, sizeof(disabled));
            (void)ResumeProcess(pid);
            close(mem);
            return FailStage(7, "install_commit");
        }
        if (!ResumeProcess(pid)) {
            ++report.rollback_attempts;
            ++report.game_write_attempts;
            (void)WriteExactVerified(mem, object, &plan.original_vptr,
                                     sizeof(plan.original_vptr));
            const std::uint32_t disabled = 0;
            (void)WriteExactVerified(mem,
                layout.control + offsetof(payload::Control, enabled),
                &disabled, sizeof(disabled));
            (void)ResumeProcess(pid);
            close(mem);
            return FailStage(8, "install_resume");
        }
        report.flags |= kProcessResumed;
    } else {
        if (!ReadExact(mem, layout.control, &control, sizeof(control)) ||
            !ReadExact(mem, layout.evidence, &evidence, sizeof(evidence)) ||
            !PayloadHeaders(control, evidence) ||
            !Configured(control, mode, limit, plan) ||
            (current_vptr != plan.shadow_vptr &&
             current_vptr != plan.original_vptr)) {
            close(mem);
            return FailStage(9, "existing_transaction");
        }
        report.flags |= kPayloadConfigured;
        const bool complete = Complete(control, evidence, mode, limit);
        if (complete) report.flags |= kEvidenceComplete;
        if (action == Action::kStatus) {
            report.control = control;
            report.evidence = evidence;
            const bool written = WriteOutput(argv[9], &report, mem, layout,
                                             0, false);
            close(mem);
            std::printf(
                "PHYSICS_INTERVAL_SHADOW_STATUS complete=%u cursor=%u "
                "calls=%" PRIu64 " active=%u vptr=0x%" PRIx64 "\n",
                complete ? 1u : 0u, control.cursor, evidence.calls,
                control.active_calls, static_cast<std::uint64_t>(current_vptr));
            return written ? 0 : 10;
        }
        if (action == Action::kFinalize && !complete) {
            close(mem);
            return FailStage(11, "finalize_incomplete");
        }
        if (current_vptr == plan.shadow_vptr &&
            !DisableAndDrain(mem, layout, &report)) {
            close(mem);
            return FailStage(12, "disable_and_drain");
        }
        if (!StopProcess(pid)) {
            (void)ResumeProcess(pid);
            close(mem);
            return FailStage(6, "restore_stop");
        }
        report.flags |= kProcessStopped;
        shadow::Plan stopped_plan{};
        std::uintptr_t stopped_vptr = 0;
        bool restored = RevalidateStopped(
            pid, expected_ticks, mem, game_base, object, layout,
            &stopped_plan, &stopped_vptr) &&
            stopped_plan.shadow_vptr == plan.shadow_vptr;
        payload::Control stopped_control{};
        restored = restored &&
            ReadExact(mem, layout.control, &stopped_control,
                      sizeof(stopped_control)) &&
            Configured(stopped_control, mode, limit, plan) &&
            stopped_control.enabled == 0 && stopped_control.active_calls == 0;
        if (restored && stopped_vptr == plan.shadow_vptr) {
            ++report.game_write_attempts;
            ++report.rollback_attempts;
            restored = WriteExactVerified(mem, object, &plan.original_vptr,
                                          sizeof(plan.original_vptr));
        } else if (restored) {
            restored = stopped_vptr == plan.original_vptr;
        }
        std::uintptr_t final_vptr = 0;
        restored = restored && ReadExact(mem, object, &final_vptr,
                                         sizeof(final_vptr)) &&
                   shadow::VerifyRestored(plan, final_vptr);
        if (restored) report.flags |= kOriginalVptrFinal;
        const bool resumed = ResumeProcess(pid);
        if (resumed) report.flags |= kProcessResumed;
        if (!restored || !resumed) {
            close(mem);
            return FailStage(13, "restore_original_vptr");
        }
    }

    (void)ReadExact(mem, layout.control, &report.control,
                    sizeof(report.control));
    (void)ReadExact(mem, layout.evidence, &report.evidence,
                    sizeof(report.evidence));
    const bool include_data = action == Action::kFinalize;
    const std::uint32_t output_count = include_data
        ? std::min(report.control.cursor, limit) : 0;
    const bool written = WriteOutput(argv[9], &report, mem, layout,
                                     output_count, include_data);
    close(mem);
    std::printf(
        "PHYSICS_INTERVAL_SHADOW_TRANSACTION action=%u flags=0x%x "
        "object=0x%" PRIx64 " original=0x%" PRIx64
        " shadow=0x%" PRIx64 " game_writes=%" PRIu64 " output=%s\n",
        report.action, report.flags, report.step_options,
        report.original_vptr, report.shadow_vptr, report.game_write_attempts,
        argv[9]);
    return written ? 0 : 10;
}
#else
int main() {
    shadow::Plan plan{};
    plan.status = shadow::Status::kReady;
    plan.step_options = 0x10002000u;
    plan.original_vptr = 0x20003000u;
    plan.shadow_vptr = 0x30004020u;
    plan.original_getter = 0x20005000u;

    payload::Control control{};
    std::memcpy(control.magic, payload::kControlMagic, 8);
    control.version = payload::kVersion;
    control.size = sizeof(control);
    control.mode = static_cast<std::uint32_t>(payload::Mode::kRecord);
    control.limit = 5;
    control.cursor = 5;
    control.completed = 1;
    control.expected_object = plan.step_options;
    control.expected_vptr = plan.shadow_vptr;
    control.original_getter = plan.original_getter;

    payload::Evidence evidence{};
    std::memcpy(evidence.magic, payload::kEvidenceMagic, 8);
    evidence.version = payload::kVersion;
    evidence.size = sizeof(evidence);
    evidence.status = payload::kStatusComplete;
    evidence.calls = 5;
    evidence.record_calls = 5;

    const std::uint32_t valid_bits = 0x3c888889u;
    Mapping translated_game_code{};
    std::memcpy(translated_game_code.perms, "r--p", 4);
    translated_game_code.path = "/pinned/libAsphalt9.so";
    const bool accepts_houdini_guest =
        GuestCodeMappingIdentity(&translated_game_code, &translated_game_code);
    translated_game_code.perms[1] = 'w';
    const bool rejects_writable_guest =
        !GuestCodeMappingIdentity(&translated_game_code,
                                  &translated_game_code);
    const bool cli_layout = kNumericArgumentIndices[0] == 2 &&
        kNumericArgumentIndices[1] == 3 && kNumericArgumentIndices[2] == 4 &&
        kNumericArgumentIndices[3] == 5 && kNumericArgumentIndices[4] == 7;
    const bool baseline = PayloadHeaders(control, evidence) &&
        Configured(control, payload::Mode::kRecord, 5, plan) &&
        Complete(control, evidence, payload::Mode::kRecord, 5) &&
        FloatBitsValid(valid_bits) && accepts_houdini_guest &&
        rejects_writable_guest && cli_layout;
    ++evidence.semantic_errors;
    const bool rejects_fault =
        !Complete(control, evidence, payload::Mode::kRecord, 5);
    --evidence.semantic_errors;
    ++control.active_calls;
    const bool rejects_active =
        !Complete(control, evidence, payload::Mode::kRecord, 5);
    --control.active_calls;
    ++control.original_getter;
    const bool rejects_identity =
        !Configured(control, payload::Mode::kRecord, 5, plan);
    const bool passed = baseline && rejects_fault && rejects_active &&
                        rejects_identity && !FloatBitsValid(0x7fc00000u);
    std::printf(
        "PHYSICS_INTERVAL_SHADOW_CONTROLLER_V2_SELFTEST passed=%u "
        "runtime=disabled process_access=0 game_writes=0\n",
        passed ? 1u : 0u);
    return passed ? 0 : 1;
}
#endif
