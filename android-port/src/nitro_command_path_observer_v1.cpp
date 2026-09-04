// Read-only observer for the action-0x07 command boundary.
//
// It never ptraces or writes the game.  It polls the already-resolved race
// sink and GameplayInputController so a real Space press can be correlated
// with the deferred command vector and its direct-dispatch mode byte.
//
// usage:
//   a9tas_nitro_command_observer_v1 PID OWNER_HEX CONTROLLER_HEX DURATION_MS


#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr std::uintptr_t kInterfaceOffset = 0x30;
constexpr std::uintptr_t kQueueOffset = 0x1360;
constexpr std::uintptr_t kDirectModeOffset = 0x1378;
constexpr std::uintptr_t kControllerFlagsOffset = 0x100;

struct Snapshot {
    std::uintptr_t interface_pointer{};
    std::uintptr_t queue_begin{};
    std::uintptr_t queue_end{};
    std::uintptr_t queue_capacity{};
    std::uint8_t direct_mode{};
    std::uint8_t controller_flags[10]{};
};

bool ParseUnsigned(const char* value, int base, std::uint64_t* out) {
    if (!value || *value == '\0' || *value == '-') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, base);
    if (errno != 0 || end == value || *end != '\0') return false;
    *out = static_cast<std::uint64_t>(parsed);
    return true;
}

std::uint64_t MonotonicNs() {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

bool ReadExact(int fd, std::uintptr_t address, void* output, std::size_t size) {
    auto* cursor = static_cast<std::uint8_t*>(output);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t count = pread64(fd, cursor + done, size - done,
                                      static_cast<off64_t>(address + done));
        if (count <= 0) return false;
        done += static_cast<std::size_t>(count);
    }
    return true;
}

bool ReadSnapshot(int fd, std::uintptr_t sink, std::uintptr_t controller,
                  Snapshot* snapshot) {
    return ReadExact(fd, sink + kInterfaceOffset,
                     &snapshot->interface_pointer,
                     sizeof(snapshot->interface_pointer)) &&
           ReadExact(fd, sink + kQueueOffset, &snapshot->queue_begin,
                     sizeof(snapshot->queue_begin)) &&
           ReadExact(fd, sink + kQueueOffset + 8, &snapshot->queue_end,
                     sizeof(snapshot->queue_end)) &&
           ReadExact(fd, sink + kQueueOffset + 16,
                     &snapshot->queue_capacity,
                     sizeof(snapshot->queue_capacity)) &&
           ReadExact(fd, sink + kDirectModeOffset, &snapshot->direct_mode,
                     sizeof(snapshot->direct_mode)) &&
           ReadExact(fd, controller + kControllerFlagsOffset,
                     snapshot->controller_flags,
                     sizeof(snapshot->controller_flags));
}

bool IsSane(const Snapshot& snapshot) {
    if ((snapshot.queue_begin | snapshot.queue_end |
         snapshot.queue_capacity) == 0)
        return true;
    return snapshot.queue_begin != 0 &&
           snapshot.queue_begin <= snapshot.queue_end &&
           snapshot.queue_end <= snapshot.queue_capacity &&
           ((snapshot.queue_end - snapshot.queue_begin) %
                sizeof(std::uintptr_t) ==
            0);
}

void PrintSnapshot(std::uint64_t elapsed_ns, const Snapshot& snapshot,
                   std::uintptr_t last_entry, bool entry_read_ok) {
    const std::uint64_t count =
        IsSane(snapshot) && snapshot.queue_begin != 0
            ? (snapshot.queue_end - snapshot.queue_begin) /
                  sizeof(std::uintptr_t)
            : 0;
    std::printf("NITRO_PATH t_ns=%" PRIu64
                " interface=0x%" PRIxPTR
                " begin=0x%" PRIxPTR " end=0x%" PRIxPTR
                " capacity=0x%" PRIxPTR " count=%" PRIu64
                " direct=%u flags=",
                elapsed_ns, snapshot.interface_pointer,
                snapshot.queue_begin, snapshot.queue_end,
                snapshot.queue_capacity, count,
                static_cast<unsigned>(snapshot.direct_mode));
    for (std::uint8_t value : snapshot.controller_flags)
        std::printf("%02x", static_cast<unsigned>(value));
    if (entry_read_ok)
        std::printf(" last_entry=0x%" PRIxPTR, last_entry);
    std::printf(" sane=%u\n", IsSane(snapshot) ? 1u : 0u);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr,
                     "usage: %s PID OWNER_HEX CONTROLLER_HEX DURATION_MS\n",
                     argv[0]);
        return 2;
    }
    std::uint64_t pid_value = 0, sink_value = 0, controller_value = 0;
    std::uint64_t duration_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &sink_value) ||
        !ParseUnsigned(argv[3], 16, &controller_value) ||
        !ParseUnsigned(argv[4], 10, &duration_value) || pid_value == 0 ||
        sink_value == 0 || controller_value == 0 || duration_value < 100 ||
        duration_value > 60000) {
        std::fprintf(stderr, "invalid arguments\n");
        return 2;
    }

    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%" PRIu64 "/mem", pid_value);
    const int mem = open(path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) {
        std::perror("open target mem read-only");
        return 3;
    }
    const auto sink = static_cast<std::uintptr_t>(sink_value);
    const auto controller = static_cast<std::uintptr_t>(controller_value);
    Snapshot current{}, previous{};
    if (!ReadSnapshot(mem, sink, controller, &previous) ||
        !IsSane(previous)) {
        std::fprintf(stderr, "initial snapshot failed or queue header invalid\n");
        close(mem);
        return 4;
    }

    const std::uint64_t started = MonotonicNs();
    const std::uint64_t deadline = started + duration_value * 1000000ULL;
    std::uint64_t samples = 0, changes = 0, read_errors = 0;
    PrintSnapshot(0, previous, 0, false);
    while (MonotonicNs() < deadline) {
        if (!ReadSnapshot(mem, sink, controller, &current)) {
            ++read_errors;
            break;
        }
        ++samples;
        if (std::memcmp(&current, &previous, sizeof(current)) != 0) {
            std::uintptr_t last_entry = 0;
            bool entry_read_ok = false;
            if (IsSane(current) && current.queue_end > current.queue_begin) {
                entry_read_ok = ReadExact(
                    mem, current.queue_end - sizeof(std::uintptr_t),
                    &last_entry, sizeof(last_entry));
                if (!entry_read_ok) ++read_errors;
            }
            PrintSnapshot(MonotonicNs() - started, current, last_entry,
                          entry_read_ok);
            previous = current;
            ++changes;
        }
        timespec delay{0, 50000};
        nanosleep(&delay, nullptr);
    }
    close(mem);
    std::printf("NITRO_PATH_DONE samples=%" PRIu64 " changes=%" PRIu64
                " read_errors=%" PRIu64 " write_scope=none\n",
                samples, changes, read_errors);
    return read_errors == 0 ? 0 : 5;
}
