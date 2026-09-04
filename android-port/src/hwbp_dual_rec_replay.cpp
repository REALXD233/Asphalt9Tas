// HISTORICAL EXPERIMENT — NOT AN ALUTASV2-PARITY REPLAY PATH.
//
// This file preserves an early experiment for evidence only.  Its assumptions
// that each write event is a tick, final_owner+0x2C0 is a race-state anchor,
// and PTRACE_SINGLESTEP is acceptable have all been disproved by later live
// work.  Its float transform/velocity snapshot is also not the certified
// native 64+12 final-correction protocol.  Do not build or deploy this file as
// the current solution.  See evidence/EXPERIMENT_QUARANTINE_AND_MAINLINE_20260817.md
// and src/native_physics_recording_v1.h.
//
// HWBP dual-field record/replay for the Asphalt 9 final control fields.
//
// Zero code modification: hardware write watchpoints (DR0/DR1, w4) on
//   final_owner+0xC98 (S/brake pulse) and final_owner+0xC9C (steering).
// The game writes these fields once per logic tick; each write traps the
// writing thread. On trap:
//   record: snapshot both fields (one 8-byte read) into a frame.
//   replay: pre-write the frame values, single-step the original write,
//           then re-write the frame values and continue. The game's per-tick
//           consumers read our values -> setter-override semantics, exactly
//           the upstream AluTasV2 approach, without patching any code.
// Event sequence (write events) is the alignment key: frame N <-> trap N.
//
// usage:
//   a9tas_hwbp_dual PID LIB_BASE_HEX record DURATION_MS OUT_PATH
//   a9tas_hwbp_dual PID LIB_BASE_HEX replay REC_PATH [START_DELAY_MS] [GO_FILE]
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr const char* kMagic = "A9HDPV2";
constexpr std::uint32_t kFrameSize = 112;

struct Mapping {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    char perms[5]{};
    std::string path;
};

bool ReadMaps(pid_t pid, std::vector<Mapping>* maps) {
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/maps", static_cast<int>(pid));
    FILE* file = std::fopen(path, "re");
    if (!file) return false;
    char line[2048]{};
    while (std::fgets(line, sizeof(line), file)) {
        unsigned long long begin = 0, end = 0;
        char perms[5]{}, pathbuf[1024]{};
        const int fields =
            std::sscanf(line, "%llx-%llx %4s %*llx %*s %*s %1023[^\n]",
                        &begin, &end, perms, pathbuf);
        Mapping m{static_cast<std::uintptr_t>(begin),
                  static_cast<std::uintptr_t>(end), {}, ""};
        std::memcpy(m.perms, perms, 4);
        if (fields == 5) m.path = pathbuf;
        while (!m.path.empty() && m.path.front() == ' ') m.path.erase(0, 1);
        maps->push_back(m);
    }
    std::fclose(file);
    return !maps->empty();
}

bool ReadExact(int fd, std::uintptr_t address, void* output, std::size_t size) {
    auto* cursor = static_cast<std::uint8_t*>(output);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n =
            pread(fd, cursor + done, size - done, static_cast<off_t>(address + done));
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

std::uint64_t MonotonicMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec) / 1000000ULL;
}

std::vector<pid_t> ListThreads(pid_t pid) {
    std::vector<pid_t> result;
    const std::string path = "/proc/" + std::to_string(pid) + "/task";
    DIR* dir = opendir(path.c_str());
    if (!dir) return result;
    while (dirent* entry = readdir(dir)) {
        char* end = nullptr;
        const long value = std::strtol(entry->d_name, &end, 10);
        if (value > 0 && end != entry->d_name && *end == '\0')
            result.push_back(static_cast<pid_t>(value));
    }
    closedir(dir);
    return result;
}

bool PokeDebug(pid_t tid, int index, unsigned long value) {
    const auto offset = offsetof(user, u_debugreg) +
                        static_cast<std::size_t>(index) * sizeof(unsigned long);
    return ptrace(PTRACE_POKEUSER, tid, reinterpret_cast<void*>(offset),
                  reinterpret_cast<void*>(value)) != -1;
}

unsigned long PeekDebug(pid_t tid, int index) {
    const auto offset = offsetof(user, u_debugreg) +
                        static_cast<std::size_t>(index) * sizeof(unsigned long);
    errno = 0;
    return static_cast<unsigned long>(ptrace(
        PTRACE_PEEKUSER, tid, reinterpret_cast<void*>(offset), nullptr));
}

bool StopThread(pid_t tid) {
    if (ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) == -1) return false;
    int status = 0;
    return waitpid(tid, &status, __WALL) == tid && WIFSTOPPED(status);
}

void ClearAndDetach(pid_t tid, bool stopped) {
    if (!stopped && !StopThread(tid)) return;
    PokeDebug(tid, 7, 0);
    PokeDebug(tid, 6, 0);
    PokeDebug(tid, 0, 0);
    PokeDebug(tid, 1, 0);
    ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
}

bool ReadQword(int mem_fd, std::uintptr_t addr, std::uint64_t* out) {
    return ReadExact(mem_fd, addr, out, sizeof(*out));
}

// Frame mirrors upstream AluTasV2 Replay::Frame (Replay.h):
//   tick index (write-event seq), steer (c9c), brake (c98), accelerator
//   (constant 1.0f: the game is auto-throttle), and recorded result state
//   (racer transform mat4x4 + velocity vec3, sampled when addresses given).
struct Frame {
    std::uint64_t t_ms;      // ms relative to race start
    std::uint32_t seq;       // write-event sequence (tick equivalent, 0-based)
    std::uint32_t c9c;       // steering float bits
    std::uint32_t c98;       // brake/S float bits
    std::uint32_t flags;     // bit0 = c9c trap, bit1 = c98 trap
    std::uint32_t accel;     // 0x3F800000 (1.0f) - auto throttle
    float transform[16];     // racer world transform (mat4x4)
    float velocity[3];       // racer velocity
    std::uint32_t reserved;  // pad to 112 bytes
};
static_assert(sizeof(Frame) == 112, "frame size");

struct RecHeader {
    char magic[8];
    std::uint64_t base;
    std::uint64_t final_owner;
    std::uint64_t start_ms;
    std::uint64_t count;
    std::uint32_t c9c_off;
    std::uint32_t c98_off;
    std::uint32_t frame_size;
    std::uint32_t reserved[2];
};
static_assert(sizeof(RecHeader) == 64, "header size");

// Resolve the mode-agnostic final owner via the inner vtable scan.
bool ResolveFinalOwner(pid_t pid, std::uintptr_t base,
                       std::uintptr_t* final_owner) {
    std::vector<Mapping> maps;
    if (!ReadMaps(pid, &maps)) return false;
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) return false;

    const std::uintptr_t inner_vtable = base + 0x7EED9E0;
    std::uintptr_t inner = 0;
    std::vector<std::uint8_t> buffer(1u << 20);
    for (const auto& map : maps) {
        if (map.perms[0] != 'r' || map.perms[1] != 'w') continue;
        if (map.path == "[vvar]" || map.path == "[vdso]") continue;
        for (std::uintptr_t cursor = map.begin; cursor < map.end;) {
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uintptr_t>(buffer.size(), map.end - cursor));
            const ssize_t got = pread(mem, buffer.data(), want,
                                      static_cast<off_t>(cursor));
            if (got <= 0) {
                cursor += want;
                continue;
            }
            for (std::size_t off = 0;
                 off + sizeof(std::uintptr_t) <= static_cast<std::size_t>(got);
                 off += alignof(std::uintptr_t)) {
                std::uintptr_t value = 0;
                std::memcpy(&value, buffer.data() + off, sizeof(value));
                if (value == inner_vtable) {
                    inner = cursor + off;
                    break;
                }
            }
            if (inner) break;
            cursor += got;
        }
        if (inner) break;
    }
    if (!inner) {
        close(mem);
        return false;
    }
    std::int64_t adj = 0;
    if (inner_vtable < 0x2D8 ||
        !ReadExact(mem, inner_vtable - 0x2D8, &adj, sizeof(adj))) {
        close(mem);
        return false;
    }
    *final_owner = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(inner) + adj);
    close(mem);
    return true;
}

// Arm DR0 (c9c), DR1 (c98) and DR2 (race state) write watchpoints (w4) on
// all threads. DR2 fires when the game flips race state 0->1 (race start);
// frames only count from that moment (auto start anchor, no human sync).
bool ArmWatchpoints(pid_t pid, std::uintptr_t c9c_addr, std::uintptr_t c98_addr,
                    std::uintptr_t race_addr, std::vector<pid_t>* traced) {
    const std::vector<pid_t> tids = ListThreads(pid);
    for (pid_t tid : tids) {
        if (ptrace(PTRACE_SEIZE, tid, nullptr, nullptr) == -1) continue;
        if (!StopThread(tid)) {
            ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
            continue;
        }
        // DR7: local DR0+DR1+DR2 enabled, RW=write (01), LEN=4 bytes (11).
        const unsigned long dr7 = 1UL | (1UL << 16) | (3UL << 18) |   // DR0
                                  (1UL << 2) | (1UL << 20) | (3UL << 22) |  // DR1
                                  (1UL << 4) | (1UL << 24) | (3UL << 26);   // DR2
        if (!PokeDebug(tid, 0, c9c_addr) || !PokeDebug(tid, 1, c98_addr) ||
            !PokeDebug(tid, 2, race_addr) || !PokeDebug(tid, 6, 0) ||
            !PokeDebug(tid, 7, dr7)) {
            ClearAndDetach(tid, true);
            continue;
        }
        traced->push_back(tid);
    }
    for (pid_t tid : *traced) ptrace(PTRACE_CONT, tid, nullptr, nullptr);
    return !traced->empty();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 6 || argc > 10) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX record DURATION_MS OUT_PATH "
                     "[TRANSFORM_ADDR VELOCITY_ADDR]\n"
                     "       %s PID LIB_BASE_HEX replay REC_PATH "
                     "[START_DELAY_MS] [TRANSFORM_ADDR VELOCITY_ADDR]\n",
                     argv[0], argv[0]);
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(std::strtol(argv[1], nullptr, 10));
    const auto base = static_cast<std::uintptr_t>(std::strtoull(argv[2], nullptr, 16));
    const bool is_record = std::strcmp(argv[3], "record") == 0;
    const bool is_replay = std::strcmp(argv[3], "replay") == 0;
    if (pid <= 0 || !base || (!is_record && !is_replay)) return 2;

    std::uintptr_t transform_addr = 0, velocity_addr = 0;
    if (is_record) {
        if (argc == 8) {
            transform_addr = std::strtoull(argv[6], nullptr, 16);
            velocity_addr = std::strtoull(argv[7], nullptr, 16);
        } else if (argc != 6) {
            return 2;
        }
    } else {
        if (argc == 9) {
            transform_addr = std::strtoull(argv[7], nullptr, 16);
            velocity_addr = std::strtoull(argv[8], nullptr, 16);
        } else if (argc == 8) {
            transform_addr = std::strtoull(argv[6], nullptr, 16);
            velocity_addr = std::strtoull(argv[7], nullptr, 16);
        } else if (argc != 6 && argc != 7) {
            return 2;
        }
    }

    std::uintptr_t final_owner = 0;
    if (!ResolveFinalOwner(pid, base, &final_owner)) {
        std::fprintf(stderr, "final owner resolution failed\n");
        return 3;
    }
    const std::uintptr_t c9c_addr = final_owner + 0xC9C;
    const std::uintptr_t c98_addr = final_owner + 0xC98;
    const std::uintptr_t race_addr = final_owner + 0x2C0;  // 0 -> 1 at race start
    const std::uintptr_t pair_addr = final_owner + 0xC98;  // 8-byte pair

    if (is_record) {
        const long dur = std::strtol(argv[4], nullptr, 10);
        const char* out_path = argv[5];
        if (dur <= 0 || dur > 300000) return 2;
        const auto duration_ms = static_cast<std::uint64_t>(dur);

        std::vector<pid_t> traced;
        if (!ArmWatchpoints(pid, c9c_addr, c98_addr, race_addr, &traced)) {
            std::fprintf(stderr, "no threads armed\n");
            return 4;
        }
        std::printf("HWBP_REC pid=%d owner=0x%" PRIxPTR
                    " c9c=0x%" PRIxPTR " c98=0x%" PRIxPTR
                    " race=0x%" PRIxPTR
                    " threads=%zu duration_ms=%" PRIu64
                    " (waiting for race start 0->1)\n",
                    static_cast<int>(pid), final_owner, c9c_addr, c98_addr,
                    race_addr, traced.size(), duration_ms);
        std::fflush(stdout);

        FILE* out = std::fopen(out_path, "wb");
        if (!out) {
            std::perror("fopen output");
            for (pid_t tid : traced) ClearAndDetach(tid, false);
            return 5;
        }
        RecHeader header{};
        std::memcpy(header.magic, kMagic, 8);
        header.base = base;
        header.final_owner = final_owner;
        header.start_ms = MonotonicMs();
        header.count = 0;
        header.c9c_off = 0xC9C;
        header.c98_off = 0xC98;
        header.frame_size = sizeof(Frame);
        if (std::fwrite(&header, sizeof(header), 1, out) != 1) {
            std::fclose(out);
            for (pid_t tid : traced) ClearAndDetach(tid, false);
            return 6;
        }

        char mem_path[64]{};
        std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                      static_cast<int>(pid));
        const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);

        const std::uint64_t start = MonotonicMs();
        std::uint64_t frames = 0, c9c_hits = 0, c98_hits = 0;
        bool in_race = false;
        std::uint64_t race_start_ms = 0;
        const std::uint64_t race_deadline = start + 300000;  // wait up to 5min
        std::uint64_t record_deadline = 0;  // set at race start
        while (true) {
            if (in_race && MonotonicMs() >= record_deadline) break;
            if (!in_race && MonotonicMs() >= race_deadline) {
                std::fprintf(stderr, "race start never observed\n");
                break;
            }
            int status = 0;
            const pid_t tid = waitpid(-1, &status, __WALL | WNOHANG);
            if (tid <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (!WIFSTOPPED(status)) continue;
            const int sig = WSTOPSIG(status);
            const unsigned long dr6 = PeekDebug(tid, 6);
            if (sig == SIGTRAP && (dr6 & 4) != 0) {
                // Race start anchor: 0 -> 1 flip of the race state field.
                in_race = true;
                race_start_ms = MonotonicMs();
                record_deadline = race_start_ms + duration_ms;
                std::printf("RACE_START t_ms=%" PRIu64 "\n",
                            MonotonicMs() - start);
                std::fflush(stdout);
                PokeDebug(tid, 6, 0);
                ptrace(PTRACE_CONT, tid, nullptr, nullptr);
            } else if (sig == SIGTRAP && (dr6 & 3) != 0) {
                if (!in_race) {
                    PokeDebug(tid, 6, 0);
                    ptrace(PTRACE_CONT, tid, nullptr, nullptr);
                    continue;
                }
                std::uint64_t pair = 0;
                if (mem >= 0) ReadQword(mem, pair_addr, &pair);
                Frame frame{};
                frame.t_ms = MonotonicMs() - race_start_ms;
                frame.seq = static_cast<std::uint32_t>(frames);
                frame.c9c = static_cast<std::uint32_t>(pair >> 32);
                frame.c98 = static_cast<std::uint32_t>(pair & 0xffffffffULL);
                frame.accel = 0x3F800000;  // 1.0f, auto throttle
                if (dr6 & 1) { frame.flags |= 1; ++c9c_hits; }
                if (dr6 & 2) { frame.flags |= 2; ++c98_hits; }
                if (mem >= 0 && transform_addr) {
                    ReadExact(mem, transform_addr, frame.transform,
                              sizeof(frame.transform));
                }
                if (mem >= 0 && velocity_addr) {
                    ReadExact(mem, velocity_addr, frame.velocity,
                              sizeof(frame.velocity));
                }
                if (std::fwrite(&frame, sizeof(frame), 1, out) == 1) ++frames;
                PokeDebug(tid, 6, 0);
                ptrace(PTRACE_CONT, tid, nullptr, nullptr);
            } else {
                const int deliver = sig == SIGTRAP ? 0 : sig;
                ptrace(PTRACE_CONT, tid, nullptr,
                       reinterpret_cast<void*>(static_cast<intptr_t>(deliver)));
            }
        }
        std::fseek(out, 0, SEEK_SET);
        header.count = frames;
        std::fwrite(&header, sizeof(header), 1, out);
        std::fclose(out);
        if (mem >= 0) close(mem);
        for (pid_t tid : traced) ClearAndDetach(tid, false);
        std::printf("HWBP_REC_DONE frames=%" PRIu64 " c9c=%" PRIu64
                    " c98=%" PRIu64 " path=%s\n",
                    frames, c9c_hits, c98_hits, out_path);
        return 0;
    }

    // ---------------- replay ----------------
    const char* rec_path = argv[4];
    long delay_long = 0;
    if (argc == 7) delay_long = std::strtol(argv[6], nullptr, 10);
    const auto start_delay_ms = static_cast<std::uint64_t>(delay_long > 0 ? delay_long : 0);

    FILE* rec = std::fopen(rec_path, "rb");
    if (!rec) {
        std::perror("fopen rec");
        return 7;
    }
    RecHeader header{};
    if (std::fread(&header, sizeof(header), 1, rec) != 1 ||
        std::memcmp(header.magic, kMagic, 8) != 0 || header.count == 0 ||
        header.count > 500000 || header.frame_size != sizeof(Frame)) {
        std::fprintf(stderr, "bad recording header (frame_size=%u want=%zu)\n",
                     header.frame_size, sizeof(Frame));
        std::fclose(rec);
        return 8;
    }
    std::vector<Frame> frames(header.count);
    for (std::uint64_t i = 0; i < header.count; ++i) {
        if (std::fread(&frames[i], sizeof(Frame), 1, rec) != 1) {
            std::fprintf(stderr, "short recording at frame %" PRIu64 "\n", i);
            std::fclose(rec);
            return 9;
        }
    }
    std::fclose(rec);

    std::vector<pid_t> traced;
    if (!ArmWatchpoints(pid, c9c_addr, c98_addr, race_addr, &traced)) {
        std::fprintf(stderr, "no threads armed\n");
        return 10;
    }
    std::printf("HWBP_PLAY pid=%d owner=0x%" PRIxPTR " frames=%zu"
                " delay_ms=%" PRIu64 " threads=%zu\n",
                static_cast<int>(pid), final_owner, frames.size(),
                start_delay_ms, traced.size());
    std::fflush(stdout);

    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDWR | O_CLOEXEC);

    // Optional read-only pre-delay (observe only, no override).
    const std::uint64_t begin = MonotonicMs();
    while (MonotonicMs() - begin < start_delay_ms) {
        int status = 0;
        const pid_t tid = waitpid(-1, &status, __WALL | WNOHANG);
        if (tid <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (!WIFSTOPPED(status)) continue;
        const int sig = WSTOPSIG(status);
        const unsigned long dr6 = PeekDebug(tid, 6);
        if (sig == SIGTRAP && (dr6 & 3) != 0) {
            PokeDebug(tid, 6, 0);
            ptrace(PTRACE_CONT, tid, nullptr, nullptr);
        } else {
            const int deliver = sig == SIGTRAP ? 0 : sig;
            ptrace(PTRACE_CONT, tid, nullptr,
                   reinterpret_cast<void*>(static_cast<intptr_t>(deliver)));
        }
    }

    // Override loop: waits for the race-start anchor (DR2 flip 0->1), then
    // frame N on write-event N: single-step + rewrite (setter override).
    const std::uint64_t play_start = MonotonicMs();
    std::uint64_t served = 0, events = 0;
    bool in_race = false;
    const std::uint64_t race_deadline = play_start + 120000;
    std::uint64_t final_deadline = play_start + 180000;
    while (MonotonicMs() < final_deadline) {
        int status = 0;
        const pid_t tid = waitpid(-1, &status, __WALL | WNOHANG);
        if (tid <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!in_race && MonotonicMs() > race_deadline) {
                std::fprintf(stderr, "race start never observed (replay)\n");
                break;
            }
            continue;
        }
        if (!WIFSTOPPED(status)) continue;
        const int sig = WSTOPSIG(status);
        const unsigned long dr6 = PeekDebug(tid, 6);
        if (sig == SIGTRAP && (dr6 & 4) != 0) {
            in_race = true;
            final_deadline = MonotonicMs() + 180000;
            std::printf("RACE_START (replay) t_ms=%" PRIu64 "\n",
                        MonotonicMs() - play_start);
            std::fflush(stdout);
            PokeDebug(tid, 6, 0);
            ptrace(PTRACE_CONT, tid, nullptr, nullptr);
        } else if (sig == SIGTRAP && (dr6 & 3) != 0) {
            if (!in_race) {
                PokeDebug(tid, 6, 0);
                ptrace(PTRACE_CONT, tid, nullptr, nullptr);
                continue;
            }
            const std::size_t idx = served < frames.size() ? served : frames.size() - 1;
            const Frame& frame = frames[idx];
            if (idx < frames.size() && frame.seq != served) {
                std::printf("SEQ MISMATCH served=%" PRIu64 " frame.seq=%u\n",
                            served, frame.seq);
                std::fflush(stdout);
            }
            ++served;
            ++events;
            // Pre-write the frame pair (C98 low, C9C high).
            const std::uint64_t pair =
                (static_cast<std::uint64_t>(frame.c9c) << 32) |
                static_cast<std::uint64_t>(frame.c98);
            if (mem >= 0) {
                (void)!pwrite(mem, &pair, sizeof(pair), static_cast<off_t>(pair_addr));
            }
            // Single-step the original write, then rewrite the pair.
            PokeDebug(tid, 6, 0);
            ptrace(PTRACE_SINGLESTEP, tid, nullptr, nullptr);
            int sstatus = 0;
            if (waitpid(tid, &sstatus, __WALL) == tid && WIFSTOPPED(sstatus)) {
                if (mem >= 0) {
                    (void)!pwrite(mem, &pair, sizeof(pair), static_cast<off_t>(pair_addr));
                }
            }
            PokeDebug(tid, 6, 0);
            ptrace(PTRACE_CONT, tid, nullptr, nullptr);
            if ((events % 60) == 0) {
                std::printf("PLAY events=%" PRIu64 " served=%" PRIu64
                            " t_ms=%" PRIu64 " C9C=%08x C98=%08x\n",
                            events, served, MonotonicMs() - play_start,
                            frame.c9c, frame.c98);
                std::fflush(stdout);
            }
        } else {
            const int deliver = sig == SIGTRAP ? 0 : sig;
            ptrace(PTRACE_CONT, tid, nullptr,
                   reinterpret_cast<void*>(static_cast<intptr_t>(deliver)));
        }
    }
    if (mem >= 0) close(mem);
    for (pid_t tid : traced) ClearAndDetach(tid, false);
    std::printf("HWBP_PLAY_DONE events=%" PRIu64 " served=%" PRIu64
                " frames=%zu\n",
                events, served, frames.size());
    return 0;
}
