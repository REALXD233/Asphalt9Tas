#define RC_SELFTEST_CONTROLLER
#include "injector.cpp"

#include <elf.h>
#include <fcntl.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <string>
#include <vector>

namespace {

bool ResolveElfSymbolValue(const char* path, const char* wanted,
                           std::uint64_t* value) {
    if (path == nullptr || wanted == nullptr || value == nullptr) return false;
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    Elf64_Ehdr ehdr{};
    bool ok = std::fread(&ehdr, sizeof(ehdr), 1, file) == 1 &&
              std::memcmp(ehdr.e_ident, ELFMAG, SELFMAG) == 0 &&
              ehdr.e_ident[EI_CLASS] == ELFCLASS64 &&
              ehdr.e_machine == EM_X86_64 && ehdr.e_type == ET_DYN &&
              ehdr.e_shentsize == sizeof(Elf64_Shdr);
    std::vector<Elf64_Shdr> sections;
    if (ok) {
        sections.resize(ehdr.e_shnum);
        ok = std::fseek(file, static_cast<long>(ehdr.e_shoff), SEEK_SET) == 0 &&
             std::fread(sections.data(), sizeof(Elf64_Shdr), sections.size(), file) ==
                 sections.size();
    }
    if (ok) {
        for (const Elf64_Shdr& symbols : sections) {
            if (symbols.sh_type != SHT_DYNSYM && symbols.sh_type != SHT_SYMTAB)
                continue;
            if (symbols.sh_link >= sections.size() || symbols.sh_entsize == 0)
                continue;
            const Elf64_Shdr& strings = sections[symbols.sh_link];
            std::vector<char> string_data(strings.sh_size);
            if (std::fseek(file, static_cast<long>(strings.sh_offset), SEEK_SET) != 0 ||
                (strings.sh_size != 0 &&
                 std::fread(string_data.data(), 1, string_data.size(), file) !=
                     string_data.size())) {
                continue;
            }
            const std::size_t count = symbols.sh_size / symbols.sh_entsize;
            if (std::fseek(file, static_cast<long>(symbols.sh_offset), SEEK_SET) != 0)
                continue;
            for (std::size_t i = 0; i < count; ++i) {
                Elf64_Sym symbol{};
                if (std::fread(&symbol, sizeof(symbol), 1, file) != 1) break;
                if (symbol.st_name >= string_data.size()) continue;
                if (std::strcmp(string_data.data() + symbol.st_name, wanted) == 0 &&
                    symbol.st_shndx != SHN_UNDEF) {
                    *value = symbol.st_value;
                    std::fclose(file);
                    return true;
                }
            }
        }
    }
    std::fclose(file);
    return false;
}

bool FindOffsetZeroModuleBase(pid_t pid, const std::string& exact_path,
                              std::uintptr_t* base) {
    if (base == nullptr) return false;
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    std::vector<std::uintptr_t> candidates;
    while (std::getline(maps, line)) {
        unsigned long long start = 0, end = 0, offset = 0;
        char perms[5]{};
        char path[1024]{};
        const int fields = std::sscanf(
            line.c_str(), "%llx-%llx %4s %llx %*s %*s %1023[^\n]", &start,
            &end, perms, &offset, path);
        if (fields != 5 || offset != 0) continue;
        std::string mapped = path;
        mapped.erase(mapped.begin(),
                     std::find_if(mapped.begin(), mapped.end(), [](unsigned char c) {
                         return c != ' ' && c != '\t';
                     }));
        if (mapped == exact_path) {
            candidates.push_back(static_cast<std::uintptr_t>(start));
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
    if (candidates.size() != 1) return false;
    *base = candidates.front();
    return true;
}

bool HasExactMappedPath(pid_t pid, const std::string& exact_path) {
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(maps, line)) {
        const std::size_t slash = line.find('/');
        if (slash != std::string::npos && line.substr(slash) == exact_path) {
            return true;
        }
    }
    return false;
}

pid_t FindUniqueThreadByName(pid_t pid, const char* wanted) {
    DIR* dir = opendir(("/proc/" + std::to_string(pid) + "/task").c_str());
    if (dir == nullptr) return 0;
    std::vector<pid_t> matches;
    while (dirent* entry = readdir(dir)) {
        char* end = nullptr;
        long parsed = std::strtol(entry->d_name, &end, 10);
        if (parsed <= 0 || end == entry->d_name || *end != '\0') continue;
        const pid_t tid = static_cast<pid_t>(parsed);
        if (ThreadName(pid, tid) == wanted) matches.push_back(tid);
    }
    closedir(dir);
    return matches.size() == 1 ? matches.front() : 0;
}

int ReadTracerPid(pid_t pid) {
    std::ifstream status("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(status, line)) {
        int tracer = -1;
        if (std::sscanf(line.c_str(), "TracerPid:%d", &tracer) == 1) return tracer;
    }
    return -1;
}

}  // namespace

#ifndef A9TAS_SAME_THREAD_PROBE_CONTROLLER_LIBRARY
int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr,
                     "usage: %s PID /absolute/bootstrap.so "
                     "/absolute/arm64_probe.so\n",
                     argv[0]);
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(std::strtol(argv[1], nullptr, 10));
    const std::string bootstrap_path = argv[2];
    const std::string payload_path = argv[3];
    if (pid <= 0 || bootstrap_path.empty() || payload_path.empty() ||
        bootstrap_path.front() != '/' || payload_path.front() != '/') {
        return 2;
    }
    if (!IsPidTrulyAlive(pid) || ReadTracerPid(pid) != 0) {
        std::fprintf(stderr, "precondition failed: pid dead or already traced\n");
        return 3;
    }

    std::uintptr_t bootstrap_base = 0;
    if (!FindOffsetZeroModuleBase(pid, bootstrap_path, &bootstrap_base) ||
        !HasExactMappedPath(pid, payload_path)) {
        std::fprintf(stderr, "required isolated modules not uniquely mapped\n");
        return 4;
    }
    std::uint64_t status_offset = 0;
    std::uint64_t getter_offset = 0;
    if (!ResolveElfSymbolValue(argv[2],
                               "a9tas_bootstrap_same_thread_probe_status",
                               &status_offset) ||
        !ResolveElfSymbolValue(
            argv[2], "a9tas_bootstrap_same_thread_probe_trampoline",
            &getter_offset)) {
        std::fprintf(stderr, "bootstrap export resolution failed\n");
        return 5;
    }
    const std::uintptr_t status_fn = bootstrap_base + status_offset;
    const std::uintptr_t getter_fn = bootstrap_base + getter_offset;
    Mapping status_mapping{};
    Mapping getter_mapping{};
    if (!FindMapping(pid, status_fn, &status_mapping) ||
        !status_mapping.readable || !status_mapping.executable ||
        !FindMapping(pid, getter_fn, &getter_mapping) ||
        !getter_mapping.readable || !getter_mapping.executable) {
        std::fprintf(stderr, "bootstrap getters are not executable\n");
        return 6;
    }

    const pid_t tid = FindUniqueThreadByName(pid, "Signal Catcher");
    const std::uintptr_t trap = FindInt3Stub(pid);
    const std::uintptr_t remote_gettid = RemoteSymbolByName(pid, "gettid");
    if (tid <= 0 || trap == 0 || remote_gettid == 0) {
        std::fprintf(stderr,
                     "controller prerequisites unavailable tid=%d trap=%p gettid=%p\n",
                     static_cast<int>(tid), reinterpret_cast<void*>(trap),
                     reinterpret_cast<void*>(remote_gettid));
        return 7;
    }

    bool attached = false;
    bool passed = false;
    std::uint64_t probe_status = 0;
    std::uint64_t trampoline = 0;
    std::uint64_t returned_tid = 0;
    RemoteCallReport status_report{};
    RemoteCallReport getter_report{};
    RemoteCallReport probe_report{};
    const std::uint64_t zero_args[6] = {0, 0, 0, 0, 0, 0};

    if (ptrace(PTRACE_ATTACH, tid, nullptr, nullptr) == -1) {
        std::fprintf(stderr, "attach failed tid=%d errno=%d\n", tid, errno);
        return 8;
    }
    attached = true;
    int wait_status = 0;
    if (waitpid(tid, &wait_status, __WALL) != tid ||
        !WIFSTOPPED(wait_status)) {
        std::fprintf(stderr, "attach wait failed tid=%d status=0x%x\n", tid,
                     wait_status);
        goto cleanup;
    }

    {
        RemoteCallSession session{};
        if (!RemoteCallSessionInit(tid, trap, &session) ||
            !CalibrateRipBias(&session, remote_gettid)) {
            std::fprintf(stderr, "safe remote-call calibration failed\n");
            goto cleanup;
        }
        if (!RemoteCallSessionCall(&session, status_fn, zero_args,
                                   &probe_status, &status_report) ||
            probe_status != 1) {
            std::fprintf(stderr, "probe status failed value=%llu result=%s\n",
                         static_cast<unsigned long long>(probe_status),
                         RemoteCallResultName(status_report.result));
            goto cleanup;
        }
        if (!RemoteCallSessionCall(&session, getter_fn, zero_args, &trampoline,
                                   &getter_report) ||
            trampoline == 0) {
            std::fprintf(stderr,
                         "trampoline getter failed value=%p result=%s\n",
                         reinterpret_cast<void*>(trampoline),
                         RemoteCallResultName(getter_report.result));
            goto cleanup;
        }
        Mapping trampoline_mapping{};
        if (!FindMapping(pid, static_cast<std::uintptr_t>(trampoline),
                         &trampoline_mapping) ||
            !trampoline_mapping.readable || !trampoline_mapping.executable) {
            std::fprintf(stderr, "trampoline is not in an r-x mapping: %p\n",
                         reinterpret_cast<void*>(trampoline));
            goto cleanup;
        }
        if (!RemoteCallSessionCall(&session,
                                   static_cast<std::uintptr_t>(trampoline),
                                   zero_args, &returned_tid, &probe_report) ||
            returned_tid != static_cast<std::uint64_t>(tid)) {
            std::fprintf(stderr,
                         "guest gettid probe failed returned=%llu expected=%d "
                         "result=%s signal=%d\n",
                         static_cast<unsigned long long>(returned_tid), tid,
                         RemoteCallResultName(probe_report.result),
                         probe_report.stop_signal);
            goto cleanup;
        }
        passed = true;
    }

cleanup:
    if (attached && ptrace(PTRACE_DETACH, tid, nullptr, nullptr) == -1) {
        std::fprintf(stderr, "detach failed tid=%d errno=%d\n", tid, errno);
        passed = false;
    }
    usleep(10000);
    const bool alive = IsPidTrulyAlive(pid);
    const int tracer = ReadTracerPid(pid);
    if (!alive || tracer != 0) passed = false;
    std::printf(
        "SAME_THREAD_PROBE_V1_RESULT passed=%d pid=%d tid=%d status=%llu "
        "trampoline=%p returned_tid=%llu alive=%d tracer_pid=%d\n",
        passed ? 1 : 0, pid, tid,
        static_cast<unsigned long long>(probe_status),
        reinterpret_cast<void*>(trampoline),
        static_cast<unsigned long long>(returned_tid), alive ? 1 : 0, tracer);
    return passed ? 0 : 1;
}
#endif
