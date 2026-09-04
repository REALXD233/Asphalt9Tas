// Gate PT-NB0: prove that the exact MainTimeSource fixed-delta producer can
// cross NativeBridge on the same Linux thread without touching gameplay state.
//
// The only guest call is the isolated ARM64 gettid probe.  There is no game
// function address, action dispatch, input injection, fixed-delta write or
// Nitro write in this controller.

#define RC_SELFTEST_CONTROLLER
#include "injector.cpp"

#include <dirent.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kAck =
    "I_ACCEPT_PRODUCER_THREAD_NATIVEBRIDGE_GETTID_PROBE_V1";
constexpr const char* kBootstrapPath =
    "/data/local/tmp/liba9tas_bootstrap_producer_thread_probe_v1.so";
constexpr const char* kPayloadPath =
    "/data/local/tmp/liba9tas_producer_thread_probe_v1.so";
constexpr unsigned long kDeltaWrite8Dr7 =
    1UL | (1UL << 16) | (2UL << 18);
constexpr std::uint32_t kRequiredProducerHits = 2;
constexpr std::int64_t kMaximumNaturalDeltaUs = 1000000;
constexpr std::uintptr_t kMainVtableRva = 0x7F3C1A8;
constexpr std::uintptr_t kEmbeddedTimeSourceVtableRva = 0x7F3C400;
constexpr std::uintptr_t kEmbeddedTimeSourceOffset = 0xB0;
constexpr std::uintptr_t kTimeScaleManagerOffset = 0xF8;
constexpr std::uintptr_t kAccumulatorOffset = 0x150;
constexpr std::uintptr_t kEnabledOffset = 0x158;
constexpr std::uintptr_t kPhaseSchedulerOffset = 0x178;
constexpr std::uintptr_t kTimeScaleValueOffset = 0x2D8;
constexpr std::size_t kValidatedObjectSize = 0x1C50;

struct BuildSignatureV1 {
    std::uintptr_t rva;
    const std::uint8_t* bytes;
    std::size_t size;
};

constexpr std::uint8_t kPhysicsTimeCallbackSignature[] = {
    0xFF, 0xC3, 0x00, 0xD1, 0xF4, 0x0B, 0x00, 0xF9,
    0xF3, 0x7B, 0x02, 0xA9, 0xF4, 0x03, 0x00, 0xAA,
    0x00, 0xF4, 0x41, 0xF9, 0xF3, 0x03, 0x02, 0xAA,
    0x40, 0x00, 0x00, 0xB4, 0xF3, 0x94, 0x36, 0x94,
};
constexpr std::uint8_t kInputPhaseSignature[] = {
    0xFF, 0x43, 0x03, 0xD1, 0xF7, 0x5B, 0x0A, 0xA9,
    0xF5, 0x53, 0x0B, 0xA9, 0xF3, 0x7B, 0x0C, 0xA9,
    0x0C, 0x66, 0x00, 0x94, 0xE0, 0xE3, 0x00, 0x91,
    0xE8, 0x4F, 0x00, 0xF9, 0x4A, 0xC9, 0x4D, 0x94,
};
constexpr std::uint8_t kPhysicsChunkerSignature[] = {
    0xFF, 0x43, 0x01, 0xD1, 0xF7, 0x5B, 0x02, 0xA9,
    0xF5, 0x53, 0x03, 0xA9, 0xF3, 0x7B, 0x04, 0xA9,
    0x68, 0x6D, 0x03, 0xD0, 0x08, 0x61, 0x1E, 0x91,
};
constexpr std::uint8_t kSetterC98Signature[] = {
    0x08, 0x00, 0x40, 0xF9, 0x29, 0x00, 0x40, 0xB9,
    0x08, 0x61, 0x0B, 0xD1, 0x08, 0x01, 0x40, 0xF9,
    0x08, 0x00, 0x08, 0x8B, 0x09, 0x99, 0x0C, 0xB9,
    0xC0, 0x03, 0x5F, 0xD6,
};
constexpr std::uint8_t kSetterC9CSignature[] = {
    0x08, 0x00, 0x40, 0xF9, 0x29, 0x00, 0x40, 0xB9,
    0x08, 0x81, 0x0B, 0xD1, 0x08, 0x01, 0x40, 0xF9,
    0x08, 0x00, 0x08, 0x8B, 0x09, 0x9D, 0x0C, 0xB9,
    0xC0, 0x03, 0x5F, 0xD6,
};
constexpr BuildSignatureV1 kBuildSignatures[] = {
    {0x3791618, kPhysicsTimeCallbackSignature,
     sizeof(kPhysicsTimeCallbackSignature)},
    {0x37918D0, kInputPhaseSignature, sizeof(kInputPhaseSignature)},
    {0x37949C8, kPhysicsChunkerSignature,
     sizeof(kPhysicsChunkerSignature)},
    {0x36934B8, kSetterC98Signature, sizeof(kSetterC98Signature)},
    {0x36934E0, kSetterC9CSignature, sizeof(kSetterC9CSignature)},
};

struct DebugSnapshot {
    unsigned long dr[4]{};
    unsigned long dr6{};
    unsigned long dr7{};
    bool valid{};
};

struct WatchedThread {
    pid_t tid{};
    bool live{};
    bool stopped{};
    DebugSnapshot debug{};
};

bool ParseU64V1(const char* text, int base, std::uint64_t* value) {
    if (text == nullptr || *text == '\0' || *text == '-' || value == nullptr)
        return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, base);
    if (errno != 0 || end == text || *end != '\0') return false;
    *value = static_cast<std::uint64_t>(parsed);
    return true;
}

int ReadTracerPidV1(pid_t pid) {
    std::ifstream status("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(status, line)) {
        int tracer = -1;
        if (std::sscanf(line.c_str(), "TracerPid:%d", &tracer) == 1)
            return tracer;
    }
    return -1;
}

bool HasExactMappedPathV1(pid_t pid, const std::string& exact_path) {
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(maps, line)) {
        const std::size_t slash = line.find('/');
        if (slash != std::string::npos && line.substr(slash) == exact_path)
            return true;
    }
    return false;
}

bool VerifySupportedBuildV1(pid_t pid, std::uintptr_t base) {
    Mapping base_mapping{};
    if (!FindMapping(pid, base, &base_mapping) || !base_mapping.readable ||
        base_mapping.path.find("libAsphalt9.so") == std::string::npos)
        return false;
    for (const auto& signature : kBuildSignatures) {
        if (base > UINTPTR_MAX - signature.rva) return false;
        const std::uintptr_t address = base + signature.rva;
        Mapping mapping{};
        std::uint8_t actual[32]{};
        if (signature.size > sizeof(actual) ||
            !FindMapping(pid, address, &mapping) || !mapping.readable ||
            address > mapping.end - signature.size ||
            !ReadProcessMemoryUnchecked(pid, address, actual, signature.size) ||
            std::memcmp(actual, signature.bytes, signature.size) != 0)
            return false;
    }
    return true;
}

bool ValidateMainObjectV1(pid_t pid, std::uintptr_t base,
                          std::uintptr_t object) {
    Mapping object_mapping{};
    if (object == 0 || object > UINTPTR_MAX - kValidatedObjectSize ||
        !FindMapping(pid, object, &object_mapping) || !object_mapping.readable ||
        !object_mapping.writable ||
        object + kValidatedObjectSize > object_mapping.end)
        return false;
    std::uintptr_t primary_vtable = 0;
    std::uintptr_t embedded_vtable = 0;
    std::uintptr_t manager = 0;
    std::uintptr_t scheduler = 0;
    std::uint8_t enabled = 0xff;
    std::uint32_t scale_bits = 0;
    std::int64_t accumulator = INT64_MIN;
    if (!ReadProcessMemoryUnchecked(pid, object, &primary_vtable,
                                    sizeof(primary_vtable)) ||
        !ReadProcessMemoryUnchecked(pid, object + kEmbeddedTimeSourceOffset,
                                    &embedded_vtable,
                                    sizeof(embedded_vtable)) ||
        !ReadProcessMemoryUnchecked(pid, object + kTimeScaleManagerOffset,
                                    &manager, sizeof(manager)) ||
        !ReadProcessMemoryUnchecked(pid, object + kPhaseSchedulerOffset,
                                    &scheduler, sizeof(scheduler)) ||
        !ReadProcessMemoryUnchecked(pid, object + kEnabledOffset, &enabled,
                                    sizeof(enabled)) ||
        !ReadProcessMemoryUnchecked(pid, object + kAccumulatorOffset,
                                    &accumulator, sizeof(accumulator)) ||
        primary_vtable != base + kMainVtableRva ||
        embedded_vtable != base + kEmbeddedTimeSourceVtableRva ||
        enabled > 1 || manager == 0 || scheduler == 0)
        return false;
    Mapping manager_mapping{};
    Mapping scheduler_mapping{};
    if (!FindMapping(pid, manager + kTimeScaleValueOffset, &manager_mapping) ||
        !manager_mapping.readable ||
        manager + kTimeScaleValueOffset >
            manager_mapping.end - sizeof(scale_bits) ||
        !FindMapping(pid, scheduler, &scheduler_mapping) ||
        !scheduler_mapping.readable ||
        !ReadProcessMemoryUnchecked(pid, manager + kTimeScaleValueOffset,
                                    &scale_bits, sizeof(scale_bits)))
        return false;
    float scale = 0.0f;
    std::memcpy(&scale, &scale_bits, sizeof(scale));
    return std::isfinite(scale) && scale >= 0.0f && scale <= 64.0f &&
           accumulator > INT64_MIN;
}

bool ResolveUniqueMainObjectV1(pid_t pid, std::uintptr_t base,
                               std::uintptr_t* object) {
    if (object == nullptr) return false;
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> ranges;
    std::string line;
    while (std::getline(maps, line)) {
        unsigned long long begin = 0, end = 0;
        char perms[5]{};
        if (std::sscanf(line.c_str(), "%llx-%llx %4s", &begin, &end,
                        perms) == 3 &&
            perms[0] == 'r' && perms[1] == 'w' && begin < end) {
            ranges.emplace_back(static_cast<std::uintptr_t>(begin),
                                static_cast<std::uintptr_t>(end));
        }
    }
    const std::string mem_path = "/proc/" + std::to_string(pid) + "/mem";
    const int mem = open(mem_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (mem < 0) return false;
    const std::uintptr_t expected_vtable = base + kMainVtableRva;
    std::vector<std::uintptr_t> candidates;
    std::vector<std::uint8_t> buffer(1u << 20);
    for (const auto& range : ranges) {
        for (std::uintptr_t cursor = range.first; cursor < range.second;) {
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uintptr_t>(buffer.size(), range.second - cursor));
            const ssize_t got = pread(mem, buffer.data(), want,
                                      static_cast<off_t>(cursor));
            if (got > 0) {
                for (std::size_t offset = 0;
                     offset + sizeof(std::uintptr_t) <=
                         static_cast<std::size_t>(got);
                     offset += alignof(std::uintptr_t)) {
                    std::uintptr_t value = 0;
                    std::memcpy(&value, buffer.data() + offset, sizeof(value));
                    if (value != expected_vtable) continue;
                    const std::uintptr_t candidate = cursor + offset;
                    if (ValidateMainObjectV1(pid, base, candidate))
                        candidates.push_back(candidate);
                }
            }
            cursor += want;
        }
    }
    close(mem);
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
    if (candidates.size() != 1) {
        std::fprintf(stderr,
                     "main object candidates=%zu (required exactly 1)\n",
                     candidates.size());
        return false;
    }
    *object = candidates.front();
    return true;
}

std::vector<pid_t> ListThreadsV1(pid_t pid) {
    std::vector<pid_t> tids;
    DIR* dir = opendir(("/proc/" + std::to_string(pid) + "/task").c_str());
    if (dir == nullptr) return tids;
    while (dirent* entry = readdir(dir)) {
        char* end = nullptr;
        const long parsed = std::strtol(entry->d_name, &end, 10);
        if (parsed > 0 && end != entry->d_name && *end == '\0')
            tids.push_back(static_cast<pid_t>(parsed));
    }
    closedir(dir);
    std::sort(tids.begin(), tids.end());
    return tids;
}

bool PeekDebugV1(pid_t tid, int index, unsigned long* value) {
    if (value == nullptr) return false;
    const auto offset = offsetof(user, u_debugreg) +
                        static_cast<std::size_t>(index) * sizeof(unsigned long);
    errno = 0;
    const long raw = ptrace(PTRACE_PEEKUSER, tid,
                            reinterpret_cast<void*>(offset), nullptr);
    if (raw == -1 && errno != 0) return false;
    *value = static_cast<unsigned long>(raw);
    return true;
}

bool PokeDebugV1(pid_t tid, int index, unsigned long value) {
    const auto offset = offsetof(user, u_debugreg) +
                        static_cast<std::size_t>(index) * sizeof(unsigned long);
    return ptrace(PTRACE_POKEUSER, tid, reinterpret_cast<void*>(offset),
                  reinterpret_cast<void*>(value)) != -1;
}

bool ReadDebugV1(pid_t tid, DebugSnapshot* output) {
    if (output == nullptr) return false;
    bool ok = true;
    for (int index = 0; index < 4; ++index)
        ok = PeekDebugV1(tid, index, &output->dr[index]) && ok;
    ok = PeekDebugV1(tid, 6, &output->dr6) && ok;
    ok = PeekDebugV1(tid, 7, &output->dr7) && ok;
    output->valid = ok;
    return ok;
}

bool DebugIsUnusedV1(const DebugSnapshot& snapshot) {
    return snapshot.valid && snapshot.dr[0] == 0 && snapshot.dr[1] == 0 &&
           snapshot.dr[2] == 0 && snapshot.dr[3] == 0 && snapshot.dr7 == 0;
}

bool RestoreDebugV1(pid_t tid, const DebugSnapshot& snapshot) {
    if (!snapshot.valid) return false;
    bool ok = PokeDebugV1(tid, 7, 0);
    for (int index = 0; index < 4; ++index)
        ok = PokeDebugV1(tid, index, snapshot.dr[index]) && ok;
    ok = PokeDebugV1(tid, 6, 0) && ok;
    ok = PokeDebugV1(tid, 7, snapshot.dr7) && ok;
    DebugSnapshot actual{};
    if (!ReadDebugV1(tid, &actual)) return false;
    bool equal = actual.dr7 == snapshot.dr7 && (actual.dr6 & 0xFUL) == 0;
    for (int index = 0; index < 4; ++index)
        equal = equal && actual.dr[index] == snapshot.dr[index];
    return ok && equal;
}

bool StopThreadV1(pid_t tid) {
    if (ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) == -1) return false;
    int status = 0;
    return waitpid(tid, &status, __WALL) == tid && WIFSTOPPED(status);
}

bool ContinueThreadV1(pid_t tid, int signal = 0) {
    return ptrace(PTRACE_CONT, tid, nullptr,
                  reinterpret_cast<void*>(static_cast<intptr_t>(signal))) != -1;
}

bool ArmThreadV1(pid_t tid, std::uintptr_t delta,
                 WatchedThread* output) {
    if (output == nullptr || ptrace(PTRACE_SEIZE, tid, nullptr, nullptr) == -1)
        return false;
    output->tid = tid;
    output->live = true;
    if (!StopThreadV1(tid)) {
        ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
        output->live = false;
        return false;
    }
    output->stopped = true;
    if (!ReadDebugV1(tid, &output->debug) ||
        !DebugIsUnusedV1(output->debug) ||
        !PokeDebugV1(tid, 0, static_cast<unsigned long>(delta)) ||
        !PokeDebugV1(tid, 6, 0) ||
        !PokeDebugV1(tid, 7, kDeltaWrite8Dr7) ||
        !ContinueThreadV1(tid)) {
        (void)RestoreDebugV1(tid, output->debug);
        ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
        output->live = false;
        output->stopped = false;
        return false;
    }
    output->stopped = false;
    return true;
}

WatchedThread* FindWatchedV1(std::vector<WatchedThread>* threads, pid_t tid) {
    for (auto& thread : *threads)
        if (thread.tid == tid) return &thread;
    return nullptr;
}

bool StopAndDisableAllV1(std::vector<WatchedThread>* threads,
                         pid_t already_stopped_tid) {
    bool ok = true;
    for (auto& thread : *threads) {
        if (!thread.live) continue;
        if (thread.tid != already_stopped_tid && !thread.stopped) {
            if (!StopThreadV1(thread.tid)) {
                ok = false;
                continue;
            }
            thread.stopped = true;
        }
        ok = PokeDebugV1(thread.tid, 7, 0) && ok;
        ok = PokeDebugV1(thread.tid, 6, 0) && ok;
    }
    return ok;
}

bool DetachAllV1(std::vector<WatchedThread>* threads) {
    bool ok = true;
    for (auto& thread : *threads) {
        if (!thread.live) continue;
        if (!thread.stopped) {
            if (!StopThreadV1(thread.tid)) {
                ok = false;
                continue;
            }
            thread.stopped = true;
        }
        ok = RestoreDebugV1(thread.tid, thread.debug) && ok;
        ok = ptrace(PTRACE_DETACH, thread.tid, nullptr, nullptr) != -1 && ok;
        thread.live = false;
        thread.stopped = false;
    }
    return ok;
}

bool ProducerNameAllowedV1(const std::string& name) {
    return name.rfind("Thread-", 0) == 0 && name != "FrameThread 0";
}

int OfflineSelftestV1() {
    DebugSnapshot empty{};
    empty.valid = true;
    DebugSnapshot busy = empty;
    busy.dr7 = 1;
    const bool ok = kDeltaWrite8Dr7 == 0x90001UL &&
                    kRequiredProducerHits == 2 &&
                    kMaximumNaturalDeltaUs == 1000000 &&
                    DebugIsUnusedV1(empty) && !DebugIsUnusedV1(busy) &&
                    ProducerNameAllowedV1("Thread-905") &&
                    !ProducerNameAllowedV1("FrameThread 0") &&
                    !ProducerNameAllowedV1("Signal Catcher");
    std::printf(
        "PRODUCER_THREAD_NATIVEBRIDGE_PROBE_V1_SELFTEST passed=%d "
        "dr7=0x%lx required_hits=%u guest_calls=0 gameplay_writes=0\n",
        ok ? 1 : 0, kDeltaWrite8Dr7, kRequiredProducerHits);
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--selftest") == 0)
        return OfflineSelftestV1();
    if (argc != 8) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX MAIN_OBJECT_HEX "
                     "TRAMPOLINE_HEX "
                     "TIMEOUT_MS EXPECTED_RIP_BIAS ACK\n",
                     argv[0]);
        return 2;
    }
    std::uint64_t pid_value = 0, base_value = 0, object_value = 0;
    std::uint64_t trampoline_value = 0;
    std::uint64_t timeout_value = 0, bias_value = 0;
    if (!ParseU64V1(argv[1], 10, &pid_value) ||
        !ParseU64V1(argv[2], 16, &base_value) ||
        !ParseU64V1(argv[3], 16, &object_value) ||
        !ParseU64V1(argv[4], 16, &trampoline_value) ||
        !ParseU64V1(argv[5], 10, &timeout_value) ||
        !ParseU64V1(argv[6], 10, &bias_value) ||
        std::strcmp(argv[7], kAck) != 0 || pid_value == 0 ||
        pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        base_value == 0 || trampoline_value == 0 ||
        timeout_value < 1000 ||
        timeout_value > 15000 || bias_value != 0) {
        std::fprintf(stderr, "invalid or unacknowledged arguments\n");
        return 2;
    }

    const pid_t pid = static_cast<pid_t>(pid_value);
    const auto base = static_cast<std::uintptr_t>(base_value);
    auto main_object = static_cast<std::uintptr_t>(object_value);
    const auto trampoline = static_cast<std::uintptr_t>(trampoline_value);
    if (main_object == 0 &&
        !ResolveUniqueMainObjectV1(pid, base, &main_object)) {
        std::fprintf(stderr, "unique main-object resolution failed\n");
        return 3;
    }
    if (main_object > UINTPTR_MAX - kAccumulatorOffset) return 2;
    const auto delta = main_object + kAccumulatorOffset;
    Mapping trampoline_mapping{};
    Mapping delta_mapping{};
    if (!IsPidTrulyAlive(pid) || ReadTracerPidV1(pid) != 0 ||
        !VerifySupportedBuildV1(pid, base) ||
        !ValidateMainObjectV1(pid, base, main_object) ||
        !HasExactMappedPathV1(pid, kBootstrapPath) ||
        !HasExactMappedPathV1(pid, kPayloadPath) ||
        !FindMapping(pid, trampoline, &trampoline_mapping) ||
        !trampoline_mapping.readable || !trampoline_mapping.executable ||
        !FindMapping(pid, delta, &delta_mapping) ||
        !delta_mapping.readable || !delta_mapping.writable ||
        delta > delta_mapping.end - sizeof(std::int64_t)) {
        std::fprintf(stderr, "process/module/address precondition failed\n");
        return 3;
    }

    std::vector<WatchedThread> threads;
    for (pid_t tid : ListThreadsV1(pid)) {
        WatchedThread thread{};
        if (!ArmThreadV1(tid, delta, &thread)) {
            (void)DetachAllV1(&threads);
            std::fprintf(stderr,
                         "strict all-thread arm failed tid=%d; detached\n", tid);
            return 4;
        }
        threads.push_back(thread);
    }
    if (threads.empty()) return 4;

    bool passed = false;
    bool transport_attempted = false;
    bool all_stopped = false;
    pid_t producer_tid = 0;
    std::string producer_name;
    std::uint32_t producer_hits = 0;
    std::int64_t delta_before = 0;
    std::int64_t delta_after = 0;
    std::uint64_t returned_tid = 0;
    std::uint64_t unexpected_stops = 0;
    RemoteCallReport call_report{};
    call_report.result = CallResult::kInternalError;
    user_regs_struct accepted_regs{};
    bool accepted_regs_valid = false;

    std::printf(
        "PRODUCER_THREAD_NATIVEBRIDGE_PROBE_V1_ARMED pid=%d main=%p delta=%p "
        "trampoline=%p threads=%zu timeout_ms=%llu bias=0 "
        "required_hits=%u host_calls=0 game_calls=0 gameplay_writes=0\n",
        pid, reinterpret_cast<void*>(main_object),
        reinterpret_cast<void*>(delta),
        reinterpret_cast<void*>(trampoline), threads.size(),
        static_cast<unsigned long long>(timeout_value), kRequiredProducerHits);
    std::fflush(stdout);

    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_value);
        while (std::chrono::steady_clock::now() < deadline && !all_stopped) {
            int status = 0;
            const pid_t tid = waitpid(-1, &status, __WALL | WNOHANG);
            if (tid == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                continue;
            }
            if (tid < 0) {
                if (errno == EINTR) continue;
                break;
            }
            WatchedThread* tracked = FindWatchedV1(&threads, tid);
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                if (tracked) {
                    tracked->live = false;
                    tracked->stopped = false;
                }
                continue;
            }
            if (!tracked || !WIFSTOPPED(status)) continue;
            tracked->stopped = true;
            unsigned long dr6 = 0;
            const int signal = WSTOPSIG(status);
            if (signal != SIGTRAP || !PeekDebugV1(tid, 6, &dr6) ||
                (dr6 & 1UL) == 0) {
                ++unexpected_stops;
                break;
            }
            std::int64_t observed_delta = 0;
            const std::string name = ThreadName(pid, tid);
            const bool qualifies =
                ReadProcessMemoryUnchecked(pid, delta, &observed_delta,
                                           sizeof(observed_delta)) &&
                observed_delta > 0 &&
                observed_delta <= kMaximumNaturalDeltaUs &&
                ProducerNameAllowedV1(name);
            if (qualifies) {
                if (producer_tid == 0) {
                    producer_tid = tid;
                    producer_name = name;
                    producer_hits = 1;
                } else if (producer_tid == tid && producer_name == name) {
                    ++producer_hits;
                } else {
                    std::fprintf(stderr,
                                 "producer affinity conflict first=%d/%s "
                                 "current=%d/%s\n",
                                 producer_tid, producer_name.c_str(), tid,
                                 name.c_str());
                    break;
                }
            }
            if (qualifies && producer_hits == kRequiredProducerHits) {
                delta_before = observed_delta;
                accepted_regs_valid =
                    ptrace(PTRACE_GETREGS, tid, nullptr, &accepted_regs) != -1;
                all_stopped = accepted_regs_valid &&
                              StopAndDisableAllV1(&threads, tid);
                break;
            }
            if (!PokeDebugV1(tid, 6, 0) || !ContinueThreadV1(tid)) break;
            tracked->stopped = false;
        }
    }

    if (all_stopped) {
        WatchedThread* producer = FindWatchedV1(&threads, producer_tid);
        user_regs_struct before_call{};
        RemoteCallSession session{};
        const std::uint64_t zero_args[6] = {0, 0, 0, 0, 0, 0};
        RcSetRipBias(0);
        const bool snapshot_ok = producer != nullptr && producer->stopped &&
            accepted_regs_valid &&
            ptrace(PTRACE_GETREGS, producer_tid, nullptr, &before_call) != -1 &&
            std::memcmp(&before_call, &accepted_regs,
                        sizeof(before_call)) == 0 &&
            ReadProcessMemoryUnchecked(pid, delta, &delta_before,
                                       sizeof(delta_before)) &&
            delta_before > 0 && delta_before <= kMaximumNaturalDeltaUs &&
            RemoteCallSessionInit(producer_tid, FindInt3Stub(pid), &session) &&
            std::memcmp(&session.original, &accepted_regs,
                        sizeof(accepted_regs)) == 0;
        if (snapshot_ok) {
            transport_attempted = true;
            const bool call_ok = RemoteCallSessionCall(
                &session, trampoline, zero_args, &returned_tid, &call_report);
            producer->stopped = call_report.stop_confirmed;
            if (!call_report.stop_confirmed &&
                !IsPidTrulyAlive(producer_tid)) {
                producer->live = false;
                producer->stopped = false;
            }
            user_regs_struct restored_regs{};
            const bool restored_regs_ok =
                producer->live && producer->stopped &&
                ptrace(PTRACE_GETREGS, producer_tid, nullptr,
                       &restored_regs) != -1 &&
                std::memcmp(&restored_regs, &accepted_regs,
                            sizeof(restored_regs)) == 0;
            const bool delta_ok =
                ReadProcessMemoryUnchecked(pid, delta, &delta_after,
                                           sizeof(delta_after)) &&
                delta_after == delta_before;
            passed = call_ok &&
                     returned_tid == static_cast<std::uint64_t>(producer_tid) &&
                     call_report.rollback_succeeded &&
                     call_report.detach_safe && restored_regs_ok && delta_ok;
        }
    }

    const bool detached = DetachAllV1(&threads);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const bool alive = IsPidTrulyAlive(pid);
    const int tracer = ReadTracerPidV1(pid);
    passed = passed && detached && alive && tracer == 0;
    std::printf(
        "PRODUCER_THREAD_NATIVEBRIDGE_PROBE_V1_RESULT passed=%d pid=%d "
        "producer_tid=%d producer_name=%s producer_hits=%u guest_tid=%llu "
        "delta_before=%lld delta_after=%lld transport_attempted=%u "
        "call_result=%s rollback=%u detached=%u unexpected_stops=%llu "
        "alive=%u tracer_pid=%d host_calls=0 game_calls=0 "
        "gameplay_writes=0\n",
        passed ? 1 : 0, pid, producer_tid,
        producer_name.empty() ? "none" : producer_name.c_str(), producer_hits,
        static_cast<unsigned long long>(returned_tid),
        static_cast<long long>(delta_before),
        static_cast<long long>(delta_after), transport_attempted ? 1u : 0u,
        RemoteCallResultName(call_report.result),
        call_report.rollback_succeeded ? 1u : 0u, detached ? 1u : 0u,
        static_cast<unsigned long long>(unexpected_stops), alive ? 1u : 0u,
        tracer);
    return passed ? 0 : 1;
}
