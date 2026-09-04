#define A9TAS_SAME_THREAD_PROBE_CONTROLLER_LIBRARY
#include "same_thread_probe_controller_v1.cpp"

#include <set>

namespace {

constexpr std::uintptr_t kExpectedTargetOffset = 0x38B74DC;
constexpr std::uint32_t kExpectedCapacity = 4096;
constexpr std::uint32_t kExpectedCaptureLimit = 300;
constexpr std::uint32_t kFlagPassiveBuild = 1u << 0;
constexpr std::uint32_t kFlagRuntimeArmingCompiled = 1u << 1;
constexpr std::uint32_t kFlagInstalled = 1u << 2;
constexpr std::uint32_t kFlagAtomicBranchPatch = 1u << 3;
constexpr char kAcknowledgement[] =
    "I_ACCEPT_P1_EXECUTOR_AFFINITY_LIVE_CANDIDATE_V1";

struct Event {
    std::uint64_t sequence;
    std::uint64_t monotonic_ns;
    std::uint64_t token_us;
    std::uint64_t context;
    std::uint64_t output;
    std::uint32_t tid;
    std::uint32_t flags;
    std::uint64_t commit_sequence;
};

struct Header {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t event_size;
    std::uint32_t capacity;
    std::uint32_t flags;
    std::uint32_t reserved;
    std::uint64_t events;
    std::uint64_t dropped;
    std::uint64_t first_ns;
    std::uint64_t last_ns;
    std::uint32_t first_tid;
    std::uint32_t last_tid;
    std::uint32_t tid_changes;
    std::uint32_t null_tokens;
    std::uint64_t zero_tokens;
    std::uint64_t nonzero_tokens;
    std::uint64_t first_token_us;
    std::uint64_t last_token_us;
    std::uint64_t guest_base;
    std::uint64_t target;
};

struct alignas(64) Report {
    Header header;
    Event events[kExpectedCapacity];
};

struct alignas(64) Control {
    char magic[8];
    std::uint32_t version;
    std::uint32_t size;
    std::uint32_t capture_enabled;
    std::uint32_t completed;
    std::uint32_t active_captures;
    std::uint32_t capture_limit;
    std::uint64_t reserved[4];
};

static_assert(sizeof(Event) == 56, "P1 event ABI");
static_assert(sizeof(Header) == 128, "P1 header ABI");
static_assert(sizeof(Report) == 229504, "P1 report ABI");
static_assert(sizeof(Control) == 64, "P1 control ABI");

bool ResolveArm64ObjectSymbol(const char* path, const char* wanted,
                              std::uint64_t* value,
                              std::uint64_t* size) {
    if (path == nullptr || wanted == nullptr || value == nullptr ||
        size == nullptr)
        return false;
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    Elf64_Ehdr ehdr{};
    bool ok = std::fread(&ehdr, sizeof(ehdr), 1, file) == 1 &&
              std::memcmp(ehdr.e_ident, ELFMAG, SELFMAG) == 0 &&
              ehdr.e_ident[EI_CLASS] == ELFCLASS64 &&
              ehdr.e_ident[EI_DATA] == ELFDATA2LSB &&
              ehdr.e_machine == EM_AARCH64 && ehdr.e_type == ET_DYN &&
              ehdr.e_shentsize == sizeof(Elf64_Shdr);
    std::vector<Elf64_Shdr> sections;
    if (ok) {
        sections.resize(ehdr.e_shnum);
        ok = std::fseek(file, static_cast<long>(ehdr.e_shoff), SEEK_SET) == 0 &&
             std::fread(sections.data(), sizeof(Elf64_Shdr), sections.size(),
                        file) == sections.size();
    }
    if (ok) {
        for (const Elf64_Shdr& symbols : sections) {
            if (symbols.sh_type != SHT_DYNSYM ||
                symbols.sh_link >= sections.size() ||
                symbols.sh_entsize < sizeof(Elf64_Sym))
                continue;
            const Elf64_Shdr& strings = sections[symbols.sh_link];
            std::vector<char> string_data(strings.sh_size);
            if (std::fseek(file, static_cast<long>(strings.sh_offset),
                           SEEK_SET) != 0 ||
                (strings.sh_size != 0 &&
                 std::fread(string_data.data(), 1, string_data.size(), file) !=
                     string_data.size()))
                continue;
            const std::size_t count = symbols.sh_size / symbols.sh_entsize;
            for (std::size_t index = 0; index < count; ++index) {
                Elf64_Sym symbol{};
                if (std::fseek(file,
                               static_cast<long>(symbols.sh_offset +
                                   index * symbols.sh_entsize),
                               SEEK_SET) != 0 ||
                    std::fread(&symbol, sizeof(symbol), 1, file) != 1)
                    break;
                if (symbol.st_name >= string_data.size() ||
                    symbol.st_shndx == SHN_UNDEF ||
                    ELF64_ST_TYPE(symbol.st_info) != STT_OBJECT)
                    continue;
                if (std::strcmp(string_data.data() + symbol.st_name,
                                wanted) == 0) {
                    *value = symbol.st_value;
                    *size = symbol.st_size;
                    std::fclose(file);
                    return true;
                }
            }
        }
    }
    std::fclose(file);
    return false;
}

template <typename T>
bool ReadObject(pid_t pid, std::uintptr_t address, T* output) {
    return output != nullptr && address != 0 &&
           ReadProcessMemoryUnchecked(pid, address, output, sizeof(T));
}

bool ReadExportedPointer(pid_t pid, std::uintptr_t module_base,
                         const char* module_path, const char* symbol,
                         std::uintptr_t* pointer) {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint64_t value = 0;
    if (!ResolveArm64ObjectSymbol(module_path, symbol, &offset, &size) ||
        size != sizeof(value) ||
        !ReadObject(pid, module_base + offset, &value) || value == 0)
        return false;
    *pointer = static_cast<std::uintptr_t>(value);
    return true;
}

bool ControlLooksInitial(const Control& control) {
    const char magic[8] = {'A', '9', 'P', 'E', 'C', '1', 0, 0};
    return std::memcmp(control.magic, magic, sizeof(magic)) == 0 &&
           control.version == 1 && control.size == sizeof(Control) &&
           control.capture_enabled == 0 && control.completed == 0 &&
           control.active_captures == 0 &&
           control.capture_limit == kExpectedCaptureLimit;
}

bool IsReadableRange(pid_t pid, std::uintptr_t address, std::size_t size) {
    if (address == 0 || size == 0 || address > UINTPTR_MAX - size)
        return false;
    const std::uintptr_t end = address + size;
    std::uintptr_t cursor = address;
    while (cursor < end) {
        Mapping mapping{};
        if (!FindMapping(pid, cursor, &mapping) || !mapping.readable ||
            mapping.end <= cursor)
            return false;
        cursor = std::min(mapping.end, end);
    }
    return true;
}

bool ResolveUniquePayloadState(pid_t pid, const std::string& payload_path,
                               std::uintptr_t* payload_base,
                               std::uintptr_t* report_address,
                               std::uintptr_t* control_address,
                               Control* initial_control) {
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    std::vector<std::uintptr_t> candidates;
    while (std::getline(maps, line)) {
        unsigned long long start = 0, end = 0, offset = 0;
        char perms[5]{};
        char path[1024]{};
        const int fields = std::sscanf(
            line.c_str(), "%llx-%llx %4s %llx %*s %*s %1023[^\n]",
            &start, &end, perms, &offset, path);
        if (fields != 5 || offset != 0) continue;
        std::string mapped = path;
        mapped.erase(mapped.begin(),
                     std::find_if(mapped.begin(), mapped.end(),
                                  [](unsigned char c) {
                                      return c != ' ' && c != '\t';
                                  }));
        if (mapped == payload_path)
            candidates.push_back(static_cast<std::uintptr_t>(start));
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());

    struct Match {
        std::uintptr_t base;
        std::uintptr_t report;
        std::uintptr_t control;
        Control initial;
    };
    std::vector<Match> matches;
    for (const std::uintptr_t base : candidates) {
        std::uintptr_t report = 0;
        std::uintptr_t control = 0;
        Control initial{};
        if (!ReadExportedPointer(
                pid, base, payload_path.c_str(),
                "a9tas_physics_executor_affinity_v1_report_storage",
                &report) ||
            !ReadExportedPointer(
                pid, base, payload_path.c_str(),
                "a9tas_physics_executor_affinity_v1_control_storage",
                &control) ||
            (report & 63u) != 0 || (control & 63u) != 0 || report < base ||
            report - base > 16 * 1024 * 1024 || control < base ||
            control - base > 16 * 1024 * 1024 ||
            !IsReadableRange(pid, report, sizeof(Report)) ||
            !IsReadableRange(pid, control, sizeof(Control)) ||
            !ReadObject(pid, control, &initial) ||
            !ControlLooksInitial(initial))
            continue;
        matches.push_back({base, report, control, initial});
    }
    if (matches.size() != 1) return false;
    *payload_base = matches[0].base;
    *report_address = matches[0].report;
    *control_address = matches[0].control;
    *initial_control = matches[0].initial;
    return true;
}

bool InvokeArmOnSignalCatcher(pid_t pid, std::uintptr_t bootstrap_base,
                              const char* bootstrap_path,
                              std::uint64_t* returned,
                              std::uint64_t* trampoline_out,
                              std::uintptr_t* control_out) {
    std::uint64_t status_offset = 0;
    std::uint64_t getter_offset = 0;
    if (!ResolveElfSymbolValue(
            bootstrap_path, "a9tas_bootstrap_same_thread_probe_status",
            &status_offset) ||
        !ResolveElfSymbolValue(
            bootstrap_path,
            "a9tas_bootstrap_same_thread_probe_trampoline", &getter_offset))
        return false;
    const std::uintptr_t status_fn = bootstrap_base + status_offset;
    const std::uintptr_t getter_fn = bootstrap_base + getter_offset;
    Mapping status_mapping{};
    Mapping getter_mapping{};
    if (!FindMapping(pid, status_fn, &status_mapping) ||
        !status_mapping.readable || !status_mapping.executable ||
        !FindMapping(pid, getter_fn, &getter_mapping) ||
        !getter_mapping.readable || !getter_mapping.executable)
        return false;

    const pid_t tid = FindUniqueThreadByName(pid, "Signal Catcher");
    const std::uintptr_t trap = FindInt3Stub(pid);
    const std::uintptr_t remote_gettid = RemoteSymbolByName(pid, "gettid");
    if (tid <= 0 || trap == 0 || remote_gettid == 0) return false;
    if (ptrace(PTRACE_ATTACH, tid, nullptr, nullptr) == -1) return false;
    bool passed = false;
    int wait_status = 0;
    if (waitpid(tid, &wait_status, __WALL) == tid &&
        WIFSTOPPED(wait_status)) {
        RemoteCallSession session{};
        const std::uint64_t zero_args[6] = {0, 0, 0, 0, 0, 0};
        std::uint64_t status = 0;
        std::uint64_t trampoline = 0;
        RemoteCallReport status_report{};
        RemoteCallReport getter_report{};
        RemoteCallReport low_report{};
        RemoteCallReport high_report{};
        RemoteCallReport permission_report{};
        RemoteCallReport arm_report{};
        if (RemoteCallSessionInit(tid, trap, &session) &&
            CalibrateRipBias(&session, remote_gettid) &&
            RemoteCallSessionCall(&session, status_fn, zero_args, &status,
                                  &status_report) &&
            status == 1 &&
            RemoteCallSessionCall(&session, getter_fn, zero_args, &trampoline,
                                  &getter_report) &&
            trampoline != 0) {
            Mapping trampoline_mapping{};
            std::uint64_t low_result = 0;
            std::uint64_t high_result = 0;
            if (FindMapping(pid, static_cast<std::uintptr_t>(trampoline),
                            &trampoline_mapping) &&
                trampoline_mapping.readable && trampoline_mapping.executable) {
                std::printf(
                    "P1_PREFLIGHT_LOW_CALL_BEGIN pid=%d tid=%d trampoline=%p\n",
                    pid, tid, reinterpret_cast<void*>(trampoline));
                std::fflush(stdout);
                if (RemoteCallSessionCall(
                        &session, static_cast<std::uintptr_t>(trampoline),
                        zero_args, &low_result, &low_report)) {
                    std::printf(
                        "P1_PREFLIGHT_HIGH_CALL_BEGIN pid=%d tid=%d trampoline=%p\n",
                        pid, tid, reinterpret_cast<void*>(trampoline));
                    std::fflush(stdout);
                    if (RemoteCallSessionCall(
                            &session, static_cast<std::uintptr_t>(trampoline),
                            zero_args, &high_result, &high_report)) {
                        const auto control_address =
                            static_cast<std::uintptr_t>(
                                (static_cast<std::uint64_t>(
                                     static_cast<std::uint32_t>(high_result))
                                 << 32u) |
                                static_cast<std::uint32_t>(low_result));
                        Control initial{};
                        Header report_header{};
                        const char control_magic[8] = {
                            'A', '9', 'P', 'E', 'C', '1', 0, 0};
                        const char report_magic[8] = {
                            'A', '9', 'P', 'E', 'A', '1', 0, 0};
                        bool preflight_ok =
                            (control_address & 63u) == 0 &&
                            IsReadableRange(pid, control_address,
                                            sizeof(Control)) &&
                            ReadObject(pid, control_address, &initial) &&
                            std::memcmp(initial.magic, control_magic,
                                        sizeof(control_magic)) == 0 &&
                            initial.version == 1 &&
                            initial.size == sizeof(Control) &&
                            initial.capture_enabled == 0 &&
                            initial.completed == 0 &&
                            initial.active_captures == 0 &&
                            initial.capture_limit == kExpectedCaptureLimit &&
                            initial.reserved[0] == 0 &&
                            initial.reserved[1] == 0 &&
                            initial.reserved[2] == 0 &&
                            initial.reserved[3] != 0 &&
                            (initial.reserved[3] & 63u) == 0 &&
                            IsReadableRange(pid, initial.reserved[3],
                                            sizeof(Report)) &&
                            ReadObject(pid, initial.reserved[3],
                                       &report_header) &&
                            std::memcmp(report_header.magic, report_magic,
                                        sizeof(report_magic)) == 0 &&
                            report_header.version == 1 &&
                            report_header.header_size == sizeof(Header) &&
                            report_header.event_size == sizeof(Event) &&
                            report_header.capacity == kExpectedCapacity &&
                            (report_header.flags & kFlagPassiveBuild) == 0 &&
                            (report_header.flags &
                             kFlagRuntimeArmingCompiled) != 0 &&
                            (report_header.flags & kFlagInstalled) == 0;
                        std::printf(
                            "P1_PREFLIGHT_RESULT passed=%d control=%p report=%p\n",
                            preflight_ok ? 1 : 0,
                            reinterpret_cast<void*>(control_address),
                            reinterpret_cast<void*>(initial.reserved[3]));
                        std::fflush(stdout);
                        if (preflight_ok) {
                            std::printf(
                                "P1_PERMISSION_PREFLIGHT_CALL_BEGIN pid=%d tid=%d trampoline=%p\n",
                                pid, tid, reinterpret_cast<void*>(trampoline));
                            std::fflush(stdout);
                            std::uint64_t permission_result = 0;
                            if (RemoteCallSessionCall(
                                    &session,
                                    static_cast<std::uintptr_t>(trampoline),
                                    zero_args, &permission_result,
                                    &permission_report)) {
                                *returned = permission_result;
                                std::printf(
                                    "P1_PERMISSION_PREFLIGHT_RESULT passed=%d result=%d\n",
                                    static_cast<std::int32_t>(permission_result) == 0
                                        ? 1
                                        : 0,
                                    static_cast<std::int32_t>(permission_result));
                                std::fflush(stdout);
                                if (static_cast<std::int32_t>(permission_result) ==
                                    0) {
                                    std::printf(
                                        "P1_ARM_CALL_BEGIN pid=%d tid=%d trampoline=%p\n",
                                        pid, tid,
                                        reinterpret_cast<void*>(trampoline));
                                    std::fflush(stdout);
                                    std::uint64_t arm_result = 0;
                                    if (RemoteCallSessionCall(
                                            &session,
                                            static_cast<std::uintptr_t>(
                                                trampoline),
                                            zero_args, &arm_result,
                                            &arm_report)) {
                                        *returned = arm_result;
                                        *trampoline_out = trampoline;
                                        *control_out = control_address;
                                        passed = true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    if (ptrace(PTRACE_DETACH, tid, nullptr, nullptr) == -1) passed = false;
    usleep(10000);
    return passed && IsPidTrulyAlive(pid) && ReadTracerPid(pid) == 0;
}

bool ValidateReport(const Report& report, std::string* error,
                    std::set<std::uint32_t>* tids) {
    const char magic[8] = {'A', '9', 'P', 'E', 'A', '1', 0, 0};
    auto fail = [&](const char* text) {
        if (error != nullptr) *error = text;
        return false;
    };
    if (std::memcmp(report.header.magic, magic, sizeof(magic)) != 0 ||
        report.header.version != 1 || report.header.header_size != sizeof(Header) ||
        report.header.event_size != sizeof(Event) ||
        report.header.capacity != kExpectedCapacity)
        return fail("report ABI mismatch");
    if ((report.header.flags & kFlagPassiveBuild) != 0 ||
        (report.header.flags & kFlagRuntimeArmingCompiled) == 0 ||
        (report.header.flags & kFlagInstalled) == 0 ||
        (report.header.flags & kFlagAtomicBranchPatch) == 0)
        return fail("report mode/install flags invalid");
    if (report.header.events != kExpectedCaptureLimit ||
        report.header.dropped != 0)
        return fail("report event count/drop mismatch");
    if (report.header.guest_base == 0 ||
        report.header.target != report.header.guest_base + kExpectedTargetOffset)
        return fail("report game base/target mismatch");
    if (report.header.first_ns == 0 ||
        report.header.last_ns < report.header.first_ns)
        return fail("report monotonic range invalid");
    for (std::uint64_t index = 0; index < report.header.events; ++index) {
        const Event& event = report.events[index];
        if (event.sequence != index || event.commit_sequence != index + 1 ||
            event.tid == 0)
            return fail("event commit/sequence/tid invalid");
        if (tids != nullptr) tids->insert(event.tid);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 7) {
        std::fprintf(stderr,
                     "usage: %s PID /absolute/bootstrap.so "
                     "/absolute/arm64_payload.so /absolute/report.bin "
                     "TIMEOUT_MS ACK\n",
                     argv[0]);
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(std::strtol(argv[1], nullptr, 10));
    const std::string bootstrap_path = argv[2];
    const std::string payload_path = argv[3];
    const std::string output_path = argv[4];
    const long timeout_ms = std::strtol(argv[5], nullptr, 10);
    if (pid <= 0 || bootstrap_path.empty() || payload_path.empty() ||
        output_path.empty() || bootstrap_path.front() != '/' ||
        payload_path.front() != '/' || output_path.front() != '/' ||
        timeout_ms < 1000 || timeout_ms > 30000 ||
        std::strcmp(argv[6], kAcknowledgement) != 0)
        return 2;
    if (!IsPidTrulyAlive(pid) || ReadTracerPid(pid) != 0) {
        std::fprintf(stderr, "precondition failed: pid dead or already traced\n");
        return 3;
    }

    std::uintptr_t bootstrap_base = 0;
    if (!FindOffsetZeroModuleBase(pid, bootstrap_path, &bootstrap_base) ||
        !HasExactMappedPath(pid, payload_path)) {
        std::fprintf(stderr, "required isolated modules are not mapped\n");
        return 4;
    }
    std::uint64_t arm_result = 0;
    std::uint64_t trampoline = 0;
    std::uintptr_t control_address = 0;
    if (!InvokeArmOnSignalCatcher(pid, bootstrap_base, argv[2], &arm_result,
                                  &trampoline, &control_address) ||
        static_cast<std::int32_t>(arm_result) != 0 || control_address == 0) {
        std::fprintf(stderr,
                     "P1 staged guest operation failed result=%lld; restart "
                     "the game process before any retry\n",
                     static_cast<long long>(
                         static_cast<std::int32_t>(arm_result)));
        return 8;
    }

    Mapping control_mapping{};
    Control control{};
    const char control_magic[8] = {'A', '9', 'P', 'E', 'C', '1', 0, 0};
    if ((control_address & 63u) != 0 ||
        !FindMapping(pid, control_address, &control_mapping) ||
        !control_mapping.readable ||
        !IsReadableRange(pid, control_address, sizeof(Control)) ||
        !ReadObject(pid, control_address, &control) ||
        std::memcmp(control.magic, control_magic, sizeof(control_magic)) != 0 ||
        control.version != 1 || control.size != sizeof(Control) ||
        control.capture_limit != kExpectedCaptureLimit ||
        control.reserved[3] == 0) {
        std::fprintf(stderr,
                     "guest-returned P1 control address/state invalid; restart "
                     "the game process before any retry\n");
        return 9;
    }
    const std::uintptr_t report_address =
        static_cast<std::uintptr_t>(control.reserved[3]);
    Mapping report_mapping{};
    if ((report_address & 63u) != 0 ||
        !FindMapping(pid, report_address, &report_mapping) ||
        !report_mapping.readable ||
        !IsReadableRange(pid, report_address, sizeof(Report))) {
        std::fprintf(stderr,
                     "guest-returned P1 report address invalid; restart the "
                     "game process before any retry\n");
        return 9;
    }

    const long polls = timeout_ms / 10;
    bool completed = false;
    for (long poll = 0; poll < polls; ++poll) {
        if (!IsPidTrulyAlive(pid) ||
            !ReadObject(pid, control_address, &control))
            break;
        if (control.completed == 1 && control.capture_enabled == 0 &&
            control.active_captures == 0 &&
            control.capture_limit == kExpectedCaptureLimit) {
            completed = true;
            break;
        }
        usleep(10000);
    }
    if (!completed) {
        std::fprintf(stderr,
                     "P1 capture did not freeze cleanly; restart the game "
                     "process before any retry\n");
        return 9;
    }
    if (control.reserved[0] == 0 || control.reserved[1] == 0 ||
        control.reserved[2] != 4 ||
        control.reserved[3] != report_address) {
        std::fprintf(stderr, "P1 atomic-branch control proof invalid\n");
        return 9;
    }
    Mapping bridge_mapping{};
    Mapping continuation_mapping{};
    if (!FindMapping(pid, control.reserved[0], &bridge_mapping) ||
        !bridge_mapping.readable || !bridge_mapping.executable ||
        !FindMapping(pid, control.reserved[1], &continuation_mapping) ||
        !continuation_mapping.readable || !continuation_mapping.executable) {
        std::fprintf(stderr, "P1 bridge/continuation mappings invalid\n");
        return 9;
    }

    Report first{};
    Report second{};
    if (!ReadObject(pid, report_address, &first)) return 10;
    usleep(5000);
    if (!ReadObject(pid, report_address, &second) ||
        std::memcmp(&first, &second, sizeof(Report)) != 0) {
        std::fprintf(stderr, "frozen report is not byte-stable\n");
        return 10;
    }
    std::string validation_error;
    std::set<std::uint32_t> tids;
    if (!ValidateReport(second, &validation_error, &tids)) {
        std::fprintf(stderr, "report validation failed: %s\n",
                     validation_error.c_str());
        return 11;
    }
    Mapping target_mapping{};
    if (!FindMapping(pid, second.header.target, &target_mapping) ||
        !target_mapping.readable || !target_mapping.executable) {
        std::fprintf(stderr, "reported target is not executable\n");
        return 12;
    }
    FILE* output = std::fopen(output_path.c_str(), "wb");
    if (output == nullptr) {
        std::fprintf(stderr, "report output write failed\n");
        return 13;
    }
    const bool wrote = std::fwrite(&second, sizeof(second), 1, output) == 1;
    const bool closed = std::fclose(output) == 0;
    if (!wrote || !closed) {
        std::fprintf(stderr, "report output write failed\n");
        return 13;
    }
    const bool alive = IsPidTrulyAlive(pid);
    const int tracer = ReadTracerPid(pid);
    std::printf(
        "PHYSICS_EXECUTOR_AFFINITY_V1_RESULT passed=%d pid=%d "
        "trampoline=%p events=%llu unique_tids=%zu first_tid=%u "
        "last_tid=%u tid_changes=%u target=%p alive=%d tracer_pid=%d\n",
        alive && tracer == 0 ? 1 : 0, pid,
        reinterpret_cast<void*>(trampoline),
        static_cast<unsigned long long>(second.header.events), tids.size(),
        second.header.first_tid, second.header.last_tid,
        second.header.tid_changes,
        reinterpret_cast<void*>(second.header.target), alive ? 1 : 0, tracer);
    return alive && tracer == 0 ? 0 : 1;
}
