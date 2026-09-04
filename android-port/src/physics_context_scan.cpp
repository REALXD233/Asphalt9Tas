#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace {
struct Mapping { std::uintptr_t begin{}, end{}; char perms[5]{}; std::string path; };

bool ReadExact(int fd, std::uintptr_t address, void* out, std::size_t size) {
    auto* p = static_cast<std::uint8_t*>(out);
    for (std::size_t done = 0; done < size;) {
        const ssize_t n = pread64(fd, p + done, size - done,
                                  static_cast<off64_t>(address + done));
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

std::vector<Mapping> ReadMaps(pid_t pid) {
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* f = std::fopen(path, "re");
    std::vector<Mapping> maps;
    char line[2048]{};
    while (f && std::fgets(line, sizeof(line), f)) {
        unsigned long long a{}, b{}, off{}; char perms[5]{}, name[1400]{};
        const int n = std::sscanf(line, "%llx-%llx %4s %llx %*s %*s %1399[^\n]",
                                  &a, &b, perms, &off, name);
        if (n < 4) continue;
        Mapping m{}; m.begin = a; m.end = b; std::memcpy(m.perms, perms, 5);
        if (n == 5) { const char* p = name; while (*p == ' ') ++p; m.path = p; }
        maps.push_back(std::move(m));
    }
    if (f) std::fclose(f);
    return maps;
}

template <typename T> T Read(int fd, std::uintptr_t a) {
    T v{}; ReadExact(fd, a, &v, sizeof(v)); return v;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s PID LIB_BASE_HEX\n", argv[0]); return 2;
    }
    const pid_t pid = static_cast<pid_t>(std::strtol(argv[1], nullptr, 10));
    const auto base = static_cast<std::uintptr_t>(std::strtoull(argv[2], nullptr, 16));
    const auto target = base + 0x8103830;
    const auto maps = ReadMaps(pid);
    char mempath[64]{}; std::snprintf(mempath, sizeof(mempath), "/proc/%d/mem", pid);
    const int fd = open(mempath, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { std::fprintf(stderr, "open mem: %s\n", std::strerror(errno)); return 3; }
    std::vector<std::uint8_t> buf(1u << 20);
    std::size_t hits = 0;
    std::vector<std::uintptr_t> contexts;
    for (const auto& m : maps) {
        if (m.perms[0] != 'r' || m.perms[1] != 'w') continue;
        for (std::uintptr_t cur = m.begin; cur < m.end;) {
            const auto want = static_cast<std::size_t>(
                std::min<std::uintptr_t>(buf.size(), m.end - cur));
            const ssize_t n = pread64(fd, buf.data(), want, static_cast<off64_t>(cur));
            if (n <= 0) { cur += want; continue; }
            for (std::size_t i = 0; i + 8 <= static_cast<std::size_t>(n); i += 8) {
                std::uintptr_t v{}; std::memcpy(&v, buf.data() + i, 8);
                if (v != target) continue;
                const auto obj = cur + i;
                const auto cb_head = Read<std::uintptr_t>(fd, obj + 0x58);
                const auto cb_count = Read<std::uint64_t>(fd, obj + 0x68);
                const auto worker = Read<std::uintptr_t>(fd, obj + 0x100);
                const auto backend = Read<std::uintptr_t>(fd, obj + 0x120);
                const auto backend_vtable = Read<std::uintptr_t>(fd, backend);
                const auto step_target = Read<std::uintptr_t>(fd, backend_vtable + 0x60);
                const auto inner_backend = Read<std::uintptr_t>(fd, backend + 0x120);
                const auto inner_vtable = Read<std::uintptr_t>(fd, inner_backend);
                const auto inner_step = Read<std::uintptr_t>(fd, inner_vtable + 0x60);
                const auto options = Read<std::uintptr_t>(fd, obj + 0x170);
                const auto interval = Read<float>(fd, obj + 0x178);
                const auto started = Read<std::uint8_t>(fd, obj + 0x1C8);
                const auto use_worker = Read<std::uint8_t>(fd, obj + 0x1C9);
                const auto token = Read<std::uint64_t>(fd, obj + 0x1D0);
                std::printf("CONTEXT object=0x%" PRIxPTR
                            " callback_head=0x%" PRIxPTR " callback_count=%" PRIu64
                            " worker=0x%" PRIxPTR " backend=0x%" PRIxPTR
                            " backend_vtable=0x%" PRIxPTR " step_target=0x%" PRIxPTR
                            " inner_backend=0x%" PRIxPTR " inner_vtable=0x%" PRIxPTR
                            " inner_step=0x%" PRIxPTR
                            " options=0x%" PRIxPTR " interval=%.9g"
                            " started=%u use_worker=%u token=%" PRIu64 "\n",
                            obj, cb_head, cb_count, worker, backend, backend_vtable,
                            step_target, inner_backend, inner_vtable, inner_step,
                            options, interval,
                            started, use_worker, token);
                if (options) {
                    std::uint8_t bytes[0x80]{};
                    if (ReadExact(fd, options, bytes, sizeof(bytes))) {
                        std::printf("OPTIONS");
                        for (std::size_t j = 0; j < sizeof(bytes); ++j) {
                            if ((j & 15U) == 0) std::printf("\n  +%02zx:", j);
                            std::printf(" %02x", bytes[j]);
                        }
                        std::printf("\n");
                    }
                }
                contexts.push_back(obj);
                ++hits;
            }
            cur += static_cast<std::size_t>(n);
        }
    }
    for (const auto context : contexts) {
        std::printf("REFERENCES context=0x%" PRIxPTR "\n", context);
        std::size_t refs = 0;
        for (const auto& m : maps) {
            if (m.perms[0] != 'r' || m.perms[1] != 'w') continue;
            for (std::uintptr_t cur = m.begin; cur < m.end;) {
                const auto want = static_cast<std::size_t>(
                    std::min<std::uintptr_t>(buf.size(), m.end - cur));
                const ssize_t n = pread64(fd, buf.data(), want,
                                          static_cast<off64_t>(cur));
                if (n <= 0) { cur += want; continue; }
                for (std::size_t i = 0; i + 8 <= static_cast<std::size_t>(n); i += 8) {
                    std::uintptr_t v{}; std::memcpy(&v, buf.data() + i, 8);
                    if (v == context) {
                        const auto ref = cur + i;
                        if (ref >= 0x1A98) {
                            const auto owner = ref - 0x1A98;
                            const auto owner_vtable = Read<std::uintptr_t>(fd, owner);
                            if (owner_vtable >= base && owner_vtable < base + 0xA5D8168) {
                                std::uint8_t scheduler[0x30]{};
                                ReadExact(fd, owner + 0x1AD0, scheduler,
                                          sizeof(scheduler));
                                std::printf("  SCHEDULER owner=0x%" PRIxPTR
                                            " vtable=lib+0x%" PRIxPTR
                                            " ref=0x%" PRIxPTR " bytes_1AD0=",
                                            owner, owner_vtable - base, ref);
                                for (std::uint8_t b : scheduler)
                                    std::printf("%02x", b);
                                std::printf("\n");
                            }
                        }
                        ++refs;
                    }
                }
                cur += static_cast<std::size_t>(n);
            }
        }
        std::printf("  reference_count=%zu\n", refs);
    }
    close(fd);
    std::printf("SUMMARY pid=%d base=0x%" PRIxPTR " target=0x%" PRIxPTR
                " contexts=%zu\n", pid, base, target, hits);
    return hits ? 0 : 4;
}
