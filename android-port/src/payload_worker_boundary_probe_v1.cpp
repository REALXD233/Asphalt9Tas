// Observation-only ARM64 guest hook for the native physics worker boundary.
//
// The hook wraps PhysicsWorld_run_three_phase_update and changes no vehicle
// state. It exposes an aligned active-call counter so a host-side read-only
// HWBP observer can prove whether native pose/velocity writes occur outside
// the complete three-phase worker call.

#include <android/log.h>
#include <elf.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kTag = "A9TAS_WORKER_BOUNDARY";
constexpr const char* kEnable =
    "/data/local/tmp/a9tas-enable-worker-boundary-v1";
constexpr const char* kStatus =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/"
    "a9tas-worker-boundary-v1.status";
constexpr std::uintptr_t kTargetRva = 0x5E08050;
constexpr std::uintptr_t kExpectedVtableRva = 0x9EDC378;
constexpr std::uint8_t kTargetSignature[16] = {
    0xff, 0xc3, 0x01, 0xd1, 0xfc, 0x6f, 0x02, 0xa9,
    0xfa, 0x67, 0x03, 0xa9, 0xf8, 0x5f, 0x04, 0xa9,
};
constexpr std::uint8_t kBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};

struct GameMapping {
    std::uintptr_t base{};
    char path[1024]{};
};

// The worker callback ABI uses x0..x7 plus two stack arguments. IDA's initial
// one-argument prototype was incomplete; the function prologue preserves all
// ten values before dispatching the three virtual phases.
using WorkerUpdateFn = double (*)(std::uintptr_t, std::uintptr_t,
                                  std::uintptr_t, std::uintptr_t,
                                  std::uintptr_t, std::uintptr_t,
                                  std::uintptr_t, std::uintptr_t,
                                  std::uintptr_t, std::uintptr_t);

extern "C" std::uintptr_t g_worker_boundary_continue = 0;
alignas(8) std::atomic<std::uint64_t> g_worker_active_calls{0};
std::atomic<std::uintptr_t> g_game_base{0};
std::atomic<std::uintptr_t> g_last_object{0};
std::atomic<std::uint64_t> g_entries{0};
std::atomic<std::uint64_t> g_exits{0};
std::atomic<std::uint64_t> g_overlap_entries{0};
std::atomic<std::uint64_t> g_counter_underflows{0};
std::atomic<std::uint64_t> g_vtable_mismatches{0};
std::atomic<std::uint64_t> g_tid_mismatches{0};
std::atomic<std::uint64_t> g_total_duration_ns{0};
std::atomic<std::uint64_t> g_min_duration_ns{UINT64_MAX};
std::atomic<std::uint64_t> g_max_duration_ns{0};
std::atomic<std::uint64_t> g_first_ns{0};
std::atomic<std::uint64_t> g_last_ns{0};
std::atomic<std::uint32_t> g_first_tid{0};
std::atomic<std::uint32_t> g_last_tid{0};
std::atomic<std::uint32_t> g_max_active_calls{0};
std::atomic<bool> g_installed{false};

std::uint64_t MonotonicNs() {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

void AtomicMinimum(std::atomic<std::uint64_t>* target, std::uint64_t value) {
    std::uint64_t current = target->load(std::memory_order_relaxed);
    while (value < current &&
           !target->compare_exchange_weak(current, value,
                                          std::memory_order_relaxed)) {}
}

void AtomicMaximum(std::atomic<std::uint64_t>* target, std::uint64_t value) {
    std::uint64_t current = target->load(std::memory_order_relaxed);
    while (value > current &&
           !target->compare_exchange_weak(current, value,
                                          std::memory_order_relaxed)) {}
}

void AtomicMaximum32(std::atomic<std::uint32_t>* target, std::uint32_t value) {
    std::uint32_t current = target->load(std::memory_order_relaxed);
    while (value > current &&
           !target->compare_exchange_weak(current, value,
                                          std::memory_order_relaxed)) {}
}

bool FindGame(GameMapping* result) {
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (!maps) return false;
    char line[2048]{};
    bool found = false;
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long start = 0, end = 0, offset = 0;
        char permissions[5]{}, path[1024]{};
        const int fields = std::sscanf(
            line, "%llx-%llx %4s %llx %*s %*s %1023[^\n]",
            &start, &end, permissions, &offset, path);
        if (fields != 5 || offset != 0 ||
            !std::strstr(path, "libAsphalt9.so"))
            continue;
        char* clean_path = path;
        while (*clean_path == ' ') ++clean_path;
        result->base = static_cast<std::uintptr_t>(start);
        std::snprintf(result->path, sizeof(result->path), "%s", clean_path);
        found = true;
        break;
    }
    std::fclose(maps);
    return found;
}

bool ReadBuildId(const char* path, std::uint8_t output[20]) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    Elf64_Ehdr header{};
    if (std::fread(&header, sizeof(header), 1, file) != 1 ||
        std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_machine != EM_AARCH64) {
        std::fclose(file);
        return false;
    }
    bool found = false;
    for (std::uint16_t i = 0; i < header.e_phnum && !found; ++i) {
        Elf64_Phdr program{};
        if (std::fseek(file, static_cast<long>(header.e_phoff) +
                                static_cast<long>(i) * sizeof(program),
                       SEEK_SET) != 0 ||
            std::fread(&program, sizeof(program), 1, file) != 1)
            break;
        if (program.p_type != PT_NOTE || program.p_filesz > 1024 * 1024)
            continue;
        std::uint64_t cursor = program.p_offset;
        const std::uint64_t end = program.p_offset + program.p_filesz;
        while (cursor + sizeof(Elf64_Nhdr) <= end) {
            Elf64_Nhdr note{};
            if (std::fseek(file, static_cast<long>(cursor), SEEK_SET) != 0 ||
                std::fread(&note, sizeof(note), 1, file) != 1)
                break;
            cursor += sizeof(note);
            const std::uint64_t name_size = (note.n_namesz + 3u) & ~3u;
            const std::uint64_t desc_size = (note.n_descsz + 3u) & ~3u;
            if (cursor + name_size + desc_size > end) break;
            char name[16]{};
            if (note.n_namesz < sizeof(name)) {
                if (std::fseek(file, static_cast<long>(cursor), SEEK_SET) != 0 ||
                    std::fread(name, 1, note.n_namesz, file) != note.n_namesz)
                    break;
            }
            cursor += name_size;
            if (note.n_type == NT_GNU_BUILD_ID && note.n_descsz == 20 &&
                std::strcmp(name, "GNU") == 0) {
                if (std::fseek(file, static_cast<long>(cursor), SEEK_SET) == 0)
                    found = std::fread(output, 20, 1, file) == 1;
                break;
            }
            cursor += desc_size;
        }
    }
    std::fclose(file);
    return found;
}

void AbsoluteJump(std::uint8_t output[16], std::uintptr_t destination) {
    constexpr std::uint32_t load_x17_literal = 0x58000051;
    constexpr std::uint32_t branch_x17 = 0xd61f0220;
    std::memcpy(output, &load_x17_literal, 4);
    std::memcpy(output + 4, &branch_x17, 4);
    std::memcpy(output + 8, &destination, 8);
}

bool ProcMem(std::uintptr_t address, void* buffer, std::size_t size,
             bool write) {
    const int fd = open("/proc/self/mem",
                        (write ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0) return false;
    const ssize_t transferred = write
        ? pwrite(fd, buffer, size, static_cast<off_t>(address))
        : pread(fd, buffer, size, static_cast<off_t>(address));
    close(fd);
    return transferred == static_cast<ssize_t>(size);
}

std::uintptr_t BuildContinuation(std::uintptr_t target) {
    auto* code = static_cast<std::uint8_t*>(
        mmap(nullptr, 32, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (code == MAP_FAILED) return 0;
    std::memcpy(code, reinterpret_cast<const void*>(target), 16);
    AbsoluteJump(code + 16, target + 16);
    __builtin___clear_cache(reinterpret_cast<char*>(code),
                            reinterpret_cast<char*>(code + 32));
    if (mprotect(code, 32, PROT_READ | PROT_EXEC) != 0) {
        munmap(code, 32);
        return 0;
    }
    return reinterpret_cast<std::uintptr_t>(code);
}

extern "C" __attribute__((noinline)) double WorkerBoundaryHook(
    std::uintptr_t arg0, std::uintptr_t arg1, std::uintptr_t arg2,
    std::uintptr_t arg3, std::uintptr_t arg4, std::uintptr_t arg5,
    std::uintptr_t arg6, std::uintptr_t arg7, std::uintptr_t arg8,
    std::uintptr_t arg9) {
    void* object = reinterpret_cast<void*>(arg0);
    const std::uint64_t entry_ns = MonotonicNs();
    const std::uint32_t tid =
        static_cast<std::uint32_t>(syscall(SYS_gettid));
    std::uint32_t expected_tid = 0;
    g_first_tid.compare_exchange_strong(expected_tid, tid,
                                        std::memory_order_relaxed);
    if (expected_tid != 0 && expected_tid != tid)
        g_tid_mismatches.fetch_add(1, std::memory_order_relaxed);
    g_last_tid.store(tid, std::memory_order_relaxed);
    g_last_object.store(reinterpret_cast<std::uintptr_t>(object),
                        std::memory_order_relaxed);
    const auto base = g_game_base.load(std::memory_order_acquire);
    if (!object || *reinterpret_cast<std::uintptr_t*>(object) !=
                       base + kExpectedVtableRva)
        g_vtable_mismatches.fetch_add(1, std::memory_order_relaxed);

    std::uint64_t zero = 0;
    g_first_ns.compare_exchange_strong(zero, entry_ns,
                                       std::memory_order_relaxed);
    g_last_ns.store(entry_ns, std::memory_order_relaxed);
    g_entries.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t previous_active =
        g_worker_active_calls.fetch_add(1, std::memory_order_release);
    if (previous_active != 0)
        g_overlap_entries.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t active_now = previous_active + 1;
    if (active_now <= UINT32_MAX)
        AtomicMaximum32(&g_max_active_calls,
                        static_cast<std::uint32_t>(active_now));

    const auto continuation = reinterpret_cast<WorkerUpdateFn>(
        g_worker_boundary_continue);
    const double result = continuation(arg0, arg1, arg2, arg3, arg4, arg5,
                                       arg6, arg7, arg8, arg9);

    const std::uint64_t previous_exit =
        g_worker_active_calls.fetch_sub(1, std::memory_order_release);
    if (previous_exit == 0) {
        g_worker_active_calls.store(0, std::memory_order_release);
        g_counter_underflows.fetch_add(1, std::memory_order_relaxed);
    }
    const std::uint64_t exit_ns = MonotonicNs();
    g_exits.fetch_add(1, std::memory_order_relaxed);
    g_last_ns.store(exit_ns, std::memory_order_relaxed);
    if (entry_ns != 0 && exit_ns >= entry_ns) {
        const std::uint64_t duration = exit_ns - entry_ns;
        g_total_duration_ns.fetch_add(duration, std::memory_order_relaxed);
        AtomicMinimum(&g_min_duration_ns, duration);
        AtomicMaximum(&g_max_duration_ns, duration);
    }
    return result;
}

void WriteStatus(const char* state) {
    const std::uint64_t entries = g_entries.load(std::memory_order_relaxed);
    const std::uint64_t exits = g_exits.load(std::memory_order_relaxed);
    const std::uint64_t first = g_first_ns.load(std::memory_order_relaxed);
    const std::uint64_t last = g_last_ns.load(std::memory_order_relaxed);
    const std::uint64_t minimum =
        g_min_duration_ns.load(std::memory_order_relaxed);
    const double observed_hz = entries > 1 && last > first
        ? static_cast<double>(entries - 1) * 1.0e9 /
              static_cast<double>(last - first)
        : 0.0;
    const double mean_us = exits != 0
        ? static_cast<double>(g_total_duration_ns.load(
              std::memory_order_relaxed)) / static_cast<double>(exits) / 1000.0
        : 0.0;
    FILE* file = std::fopen(kStatus, "w");
    if (!file) return;
    std::fprintf(
        file,
        "protocol=worker-boundary-v1\n"
        "state=%s\ninstalled=%u\nobservation_only=1\nvehicle_writes=0\n"
        "game_base=0x%" PRIxPTR "\ntarget=0x%" PRIxPTR
        "\ncontinuation=0x%" PRIxPTR "\nactive_address=0x%" PRIxPTR
        "\nactive_calls=%" PRIu64 "\nentries=%" PRIu64
        "\nexits=%" PRIu64 "\noverlap_entries=%" PRIu64
        "\nmax_active_calls=%u\ncounter_underflows=%" PRIu64
        "\nvtable_mismatches=%" PRIu64 "\ntid_mismatches=%" PRIu64
        "\nfirst_tid=%u\nlast_tid=%u\nlast_object=0x%" PRIxPTR
        "\nobserved_hz=%.6f\nmean_duration_us=%.3f"
        "\nmin_duration_us=%.3f\nmax_duration_us=%.3f\n",
        state, g_installed.load(std::memory_order_acquire) ? 1u : 0u,
        g_game_base.load(std::memory_order_relaxed),
        g_game_base.load(std::memory_order_relaxed) + kTargetRva,
        g_worker_boundary_continue,
        reinterpret_cast<std::uintptr_t>(&g_worker_active_calls),
        g_worker_active_calls.load(std::memory_order_relaxed), entries, exits,
        g_overlap_entries.load(std::memory_order_relaxed),
        g_max_active_calls.load(std::memory_order_relaxed),
        g_counter_underflows.load(std::memory_order_relaxed),
        g_vtable_mismatches.load(std::memory_order_relaxed),
        g_tid_mismatches.load(std::memory_order_relaxed),
        g_first_tid.load(std::memory_order_relaxed),
        g_last_tid.load(std::memory_order_relaxed),
        g_last_object.load(std::memory_order_relaxed), observed_hz, mean_us,
        minimum == UINT64_MAX ? 0.0 : static_cast<double>(minimum) / 1000.0,
        static_cast<double>(g_max_duration_ns.load(
            std::memory_order_relaxed)) / 1000.0);
    std::fclose(file);
}

bool Install(std::uintptr_t base) {
    const std::uintptr_t target = base + kTargetRva;
    std::uint8_t original[16]{};
    if (!ProcMem(target, original, sizeof(original), false) ||
        std::memcmp(original, kTargetSignature, sizeof(original)) != 0)
        return false;
    g_worker_boundary_continue = BuildContinuation(target);
    if (g_worker_boundary_continue == 0) return false;

    std::uint8_t patch[16]{};
    AbsoluteJump(patch,
                 reinterpret_cast<std::uintptr_t>(&WorkerBoundaryHook));
    if (!ProcMem(target, patch, sizeof(patch), true)) return false;
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + sizeof(patch)));
    std::uint8_t verify[16]{};
    if (!ProcMem(target, verify, sizeof(verify), false) ||
        std::memcmp(verify, patch, sizeof(verify)) != 0) {
        ProcMem(target, original, sizeof(original), true);
        __builtin___clear_cache(reinterpret_cast<char*>(target),
                                reinterpret_cast<char*>(target + sizeof(original)));
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    return true;
}

void* StatusWorker(void*) {
    GameMapping game{};
    for (int attempt = 0; attempt < 3000 && !FindGame(&game); ++attempt)
        usleep(10000);
    std::uint8_t build_id[20]{};
    if (game.base == 0 || access(kEnable, F_OK) != 0 ||
        !ReadBuildId(game.path, build_id) ||
        std::memcmp(build_id, kBuildId, sizeof(kBuildId)) != 0) {
        WriteStatus("precondition_failed");
        return nullptr;
    }
    g_game_base.store(game.base, std::memory_order_release);
    if (!Install(game.base)) {
        WriteStatus("install_failed");
        return nullptr;
    }
    __android_log_print(
        ANDROID_LOG_INFO, kTag,
        "installed observation_only=1 target=%p continuation=%p active=%p",
        reinterpret_cast<void*>(game.base + kTargetRva),
        reinterpret_cast<void*>(g_worker_boundary_continue),
        reinterpret_cast<void*>(&g_worker_active_calls));
    while (true) {
        WriteStatus("observing");
        sleep(1);
    }
}

__attribute__((constructor)) void OnLoad() {
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "loaded protocol=worker-boundary-v1 passive=1");
    pthread_t thread{};
    if (pthread_create(&thread, nullptr, StatusWorker, nullptr) == 0)
        pthread_detach(thread);
}

}  // namespace

extern "C" __attribute__((visibility("default")))
std::uint32_t a9tas_payload_protocol() {
    return 0x57425631;  // WBV1
}
