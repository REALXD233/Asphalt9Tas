#define A9TAS_HABI1_CONTROLLER_CORE_ONLY 1
#include "habi1_one_shot_controller.cpp"

#include <cctype>
#include <dlfcn.h>
#include <sys/mman.h>

#ifndef A9TAS_HABI1_LIBC_PATH
#error "A9TAS_HABI1_LIBC_PATH must be fixed by the build"
#endif
#ifndef A9TAS_HABI1_LIBC_SHA256
#error "A9TAS_HABI1_LIBC_SHA256 must be fixed by the build"
#endif
#ifndef A9TAS_HABI1_LIBC_BUILD_ID
#error "A9TAS_HABI1_LIBC_BUILD_ID must be fixed by the build"
#endif
#ifndef A9TAS_HABI1_LIBC_TRAP_RVA
#error "A9TAS_HABI1_LIBC_TRAP_RVA must be fixed by the build"
#endif

namespace {

constexpr const char* kCarrierLibcPath =
    A9TAS_HABI1_C_STRING(A9TAS_HABI1_LIBC_PATH);
constexpr const char* kCarrierLibcSha256 =
    A9TAS_HABI1_C_STRING(A9TAS_HABI1_LIBC_SHA256);
constexpr const char* kCarrierLibcBuildId =
    A9TAS_HABI1_C_STRING(A9TAS_HABI1_LIBC_BUILD_ID);
constexpr std::uintptr_t kCarrierLibcTrapRva = A9TAS_HABI1_LIBC_TRAP_RVA;
constexpr const char* kRequiredBridgePath = "/system/lib64/libnb.so";
constexpr const char* kForbiddenGameNeedle = "libAsphalt9.so";
constexpr const char* kDefaultGameProcessName =
    "com.aligames.kuang.kybc.aligames";

bool ValidGameProcessName(const char* value) {
    if (!value) return false;
    const std::size_t length = std::strlen(value);
    if (length == 0 || length >= 192) return false;
    for (std::size_t index = 0; index < length; ++index) {
        const unsigned char c = static_cast<unsigned char>(value[index]);
        if (!(std::isalnum(c) || c == '.' || c == '_' || c == ':' ||
              c == '-')) return false;
    }
    return true;
}

int CarrierFail(const char* stage, int code, bool killed = false) {
    std::fprintf(stderr,
                 "HABI1_EARLY_CARRIER passed=0 stage=%s code=%d errno=%d "
                 "process_killed=%d\n",
                 stage, code, errno, killed ? 1 : 0);
    return code;
}

bool HasPath(const std::vector<Mapping>& maps, const char* path) {
    return std::any_of(maps.begin(), maps.end(), [path](const Mapping& map) {
        return map.path == path && map.readable && map.private_mapping &&
               map.path.find("(deleted)") == std::string::npos;
    });
}

bool HasNeedle(const std::vector<Mapping>& maps, const char* needle) {
    return std::any_of(maps.begin(), maps.end(), [needle](const Mapping& map) {
        return map.path.find(needle) != std::string::npos;
    });
}

bool AllowedResolverPath(const std::string& path) {
    return path == "/system/lib64/libc.so" ||
           path == "/system/lib64/libdl.so" ||
           path == "/system/bin/linker64";
}

bool ResolveRemoteFixedSymbol(const std::vector<Mapping>& remote_maps,
                              const char* symbol, std::uintptr_t* output,
                              std::string* module_path) {
    if (!symbol || !output || !module_path) return false;
    void* local_symbol = dlsym(RTLD_DEFAULT, symbol);
    if (!local_symbol) return false;
    const std::uintptr_t local_address =
        reinterpret_cast<std::uintptr_t>(local_symbol);
    const std::vector<Mapping> local_maps = ReadMaps(getpid());
    const Mapping* local_map = FindMapping(local_maps, local_address);
    if (!local_map || !local_map->readable || !local_map->executable ||
        local_map->writable || !local_map->private_mapping ||
        !AllowedResolverPath(local_map->path)) return false;

    struct stat file{};
    if (lstat(local_map->path.c_str(), &file) != 0 ||
        !S_ISREG(file.st_mode) || S_ISLNK(file.st_mode) ||
        !MappingMatchesFile(*local_map, local_map->path.c_str(), file))
        return false;
    const std::uintptr_t file_offset =
        local_map->offset + (local_address - local_map->start);
    std::vector<std::uintptr_t> matches;
    for (const Mapping& map : remote_maps) {
        if (!MappingMatchesFile(map, local_map->path.c_str(), file) ||
            !map.readable || !map.executable || map.writable ||
            file_offset < map.offset ||
            file_offset >= map.offset + (map.end - map.start)) continue;
        matches.push_back(map.start + (file_offset - map.offset));
    }
    if (matches.size() != 1) return false;
    *output = matches.front();
    *module_path = local_map->path;
    return true;
}

pid_t FindExactGameProcess(const char* process_name) {
    if (!ValidGameProcessName(process_name)) return 0;
    DIR* directory = opendir("/proc");
    if (!directory) return 0;
    std::vector<pid_t> matches;
    while (dirent* entry = readdir(directory)) {
        char* end = nullptr;
        const long raw = std::strtol(entry->d_name,&end,10);
        if (raw <= 0 || end == entry->d_name || *end != '\0') continue;
        const std::string path = std::string("/proc/") + entry->d_name +
                                 "/cmdline";
        const int fd = open(path.c_str(),O_RDONLY|O_CLOEXEC);
        if (fd < 0) continue;
        std::array<char,256> command{};
        const ssize_t count = read(fd,command.data(),command.size());
        const bool closed = close(fd) == 0;
        const std::size_t expected = std::strlen(process_name);
        const std::size_t first_field = count > 0
            ? strnlen(command.data(),static_cast<std::size_t>(count)) : 0;
        if (closed && first_field == expected &&
            std::memcmp(command.data(),process_name,expected) == 0)
            matches.push_back(static_cast<pid_t>(raw));
    }
    closedir(directory);
    return matches.size() == 1 ? matches.front() : 0;
}

bool WaitForFixedPreloadWindow(const char* process_name, pid_t* pid,
                               std::uint64_t* start_ticks,
                               std::vector<Mapping>* maps) {
    if (!ValidGameProcessName(process_name) || !pid || !start_ticks || !maps)
        return false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(30);
    pid_t last_candidate = 0;
    std::uint64_t last_ticks = 0;
    bool last_bridge = false, last_libc = false, last_game = false;
    bool last_bootstrap = false, last_signal = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t candidate = FindExactGameProcess(process_name);
        if (candidate > 0) {
            const std::uint64_t ticks = ProcessStartTicks(candidate);
            std::vector<Mapping> candidate_maps = ReadMaps(candidate);
            last_candidate = candidate;
            last_ticks = ticks;
            last_bridge = HasPath(candidate_maps,kRequiredBridgePath);
            last_libc = HasPath(candidate_maps,kCarrierLibcPath);
            last_game = HasNeedle(candidate_maps,kForbiddenGameNeedle);
            last_bootstrap = HasPath(candidate_maps,kBootstrapPath);
            last_signal = last_bridge && last_libc && !last_game &&
                          !last_bootstrap && UniqueSignalCatcher(candidate) > 0;
            if (ticks != 0 && HasNeedle(candidate_maps,kForbiddenGameNeedle)) {
                errno = ETIME;
                std::fprintf(stderr,
                    "HABI1_EARLY_CARRIER_OBSERVE pid=%d start_ticks=%llu "
                    "bridge=%d libc=%d game=%d bootstrap=%d signal=%d\n",
                    candidate,static_cast<unsigned long long>(ticks),
                    last_bridge?1:0,last_libc?1:0,last_game?1:0,
                    last_bootstrap?1:0,last_signal?1:0);
                return false;
            }
            if (ticks != 0 && HasPath(candidate_maps,kRequiredBridgePath) &&
                HasPath(candidate_maps,kCarrierLibcPath) &&
                !HasPath(candidate_maps,kBootstrapPath) &&
                TracerPid(candidate) == 0 && last_signal) {
                *pid = candidate;
                *start_ticks = ticks;
                *maps = std::move(candidate_maps);
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    errno = ETIMEDOUT;
    std::fprintf(stderr,
        "HABI1_EARLY_CARRIER_OBSERVE pid=%d start_ticks=%llu bridge=%d "
        "libc=%d game=%d bootstrap=%d signal=%d\n",
        last_candidate,static_cast<unsigned long long>(last_ticks),
        last_bridge?1:0,last_libc?1:0,last_game?1:0,last_bootstrap?1:0,
        last_signal?1:0);
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 1 || argc > 2) {
        std::fprintf(stderr, "usage: %s [exact-process-name]\n", argv[0]);
        return CarrierFail("usage", 2);
    }
    const char* process_name = argc == 2 ? argv[1] : kDefaultGameProcessName;
    if (!ValidGameProcessName(process_name))
        return CarrierFail("process_name", 3);

    FileImage bootstrap_file{}, payload_file{}, libc_file{};
    if (!ReadPinnedRegularFile(kBootstrapPath, kBootstrapSha256,
                               &bootstrap_file) ||
        !ReadPinnedRegularFile(kPayloadPath, kPayloadSha256, &payload_file) ||
        !ReadPinnedRegularFile(kCarrierLibcPath, kCarrierLibcSha256, &libc_file))
        return CarrierFail("artifact_identity", 4);
    ElfImage bootstrap_elf{}, payload_elf{}, libc_elf{};
    if (!bootstrap_elf.Parse(bootstrap_file, EM_X86_64) ||
        bootstrap_elf.build_id() != kBootstrapBuildId ||
        !payload_elf.Parse(payload_file, EM_AARCH64) ||
        payload_elf.build_id() != kPayloadBuildId ||
        !libc_elf.Parse(libc_file, EM_X86_64) ||
        libc_elf.build_id() != kCarrierLibcBuildId ||
        kCarrierLibcTrapRva >= libc_file.bytes.size() ||
        libc_file.bytes[kCarrierLibcTrapRva] != 0xcc)
        return CarrierFail("elf_identity", 5);

    Elf64_Sym locator_symbol{}, stage_symbol{};
    if (!bootstrap_elf.ResolveUnique(kLocatorSymbol, true, &locator_symbol) ||
        ELF64_ST_TYPE(locator_symbol.st_info) != STT_OBJECT ||
        locator_symbol.st_size != sizeof(Locator) ||
        !bootstrap_elf.ResolveUnique(kStageSymbol, false, &stage_symbol))
        return CarrierFail("bootstrap_symbols", 6);

    pid_t pid = 0;
    std::uint64_t expected_ticks = 0;
    std::vector<Mapping> maps;
    if (!WaitForFixedPreloadWindow(process_name,&pid,&expected_ticks,&maps) ||
        !IsAlive(pid,expected_ticks) || TracerPid(pid) != 0)
        return CarrierFail("preload_window", 7);
    std::uintptr_t libc_base = 0;
    if (!UniqueOffsetZeroBase(maps, kCarrierLibcPath, libc_file.status,
                              &libc_base))
        return CarrierFail("libc_mapping", 8);
    const std::uintptr_t trap = libc_base + kCarrierLibcTrapRva;
    const Mapping* trap_map = FindMapping(maps, trap);
    if (!trap_map || !MappingMatchesFile(*trap_map, kCarrierLibcPath,
                                         libc_file.status) ||
        !trap_map->readable || !trap_map->executable || trap_map->writable)
        return CarrierFail("fixed_trap", 9);

    std::uintptr_t remote_gettid=0, remote_mmap=0, remote_munmap=0;
    std::uintptr_t remote_dlopen=0;
    std::string gettid_module, mmap_module, munmap_module, dlopen_module;
    if (!ResolveRemoteFixedSymbol(maps,"gettid",&remote_gettid,&gettid_module) ||
        !ResolveRemoteFixedSymbol(maps,"mmap",&remote_mmap,&mmap_module) ||
        !ResolveRemoteFixedSymbol(maps,"munmap",&remote_munmap,&munmap_module) ||
        !ResolveRemoteFixedSymbol(maps,"dlopen",&remote_dlopen,&dlopen_module))
        return CarrierFail("fixed_symbol_resolution", 10);

    const pid_t tid = UniqueSignalCatcher(pid);
    if (tid <= 0 || !IsAlive(pid, expected_ticks) || TracerPid(pid) != 0)
        return CarrierFail("signal_catcher", 11);
    if (ptrace(PTRACE_ATTACH, tid, nullptr, nullptr) == -1)
        return CarrierFail("attach", 12);
    int attach_status = 0;
    if (waitpid(tid,&attach_status,__WALL) != tid ||
        !WIFSTOPPED(attach_status) || WSTOPSIG(attach_status) != SIGSTOP) {
        const bool killed = KillUncertainProcess(pid);
        return CarrierFail("attach_wait",13,killed);
    }

    bool uncertain = false;
    user_regs_struct original{};
    if (!GetRegs(tid,&original) || ProcessStartTicks(pid) != expected_ticks ||
        ThreadName(pid,tid) != "Signal Catcher" || ThreadGroupId(tid) != pid)
        uncertain = true;

    long bias = 0;
    bool calibrated = false;
    CallReport report{};
    const std::uint64_t zero_args[6]{};
    if (!uncertain) {
        constexpr long candidates[] = {0,2,-2,4};
        for (const long candidate : candidates) {
            const bool called = RemoteCallOnce(tid,maps,original,candidate,
                                               remote_gettid,trap,zero_args,&report);
            if (called && report.result == static_cast<std::uint64_t>(tid)) {
                bias = candidate;
                calibrated = true;
                break;
            }
            if (!report.detach_safe || report.stop_signal != SIGTRAP) {
                uncertain = true;
                break;
            }
        }
    }
    if (!calibrated) uncertain = true;

    std::uint64_t scratch = 0;
    if (!uncertain) {
        const std::uint64_t mmap_args[6] = {
            0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,
            static_cast<std::uint64_t>(-1),0};
        if (!RemoteCallOnce(tid,maps,original,bias,remote_mmap,trap,mmap_args,
                            &report) || !report.detach_safe || report.result == 0 ||
            report.result == UINT64_MAX) uncertain = true;
        else scratch = report.result;
    }
    if (!uncertain) {
        const std::size_t path_size = std::strlen(kBootstrapPath) + 1;
        std::array<char, 256> readback{};
        if (path_size > readback.size() ||
            !WriteRemote(tid,scratch,kBootstrapPath,path_size) ||
            !ReadRemote(tid,scratch,readback.data(),path_size) ||
            std::memcmp(readback.data(),kBootstrapPath,path_size) != 0)
            uncertain = true;
    }

    std::uint64_t handle = 0;
    if (!uncertain) {
        const std::uint64_t dlopen_args[6] = {
            scratch,RTLD_NOW|RTLD_LOCAL,0,0,0,0};
        if (!RemoteCallOnce(tid,maps,original,bias,remote_dlopen,trap,
                            dlopen_args,&report) || !report.detach_safe ||
            report.result == 0) uncertain = true;
        else handle = report.result;
    }

    if (!uncertain && scratch != 0) {
        const std::uint64_t munmap_args[6] = {scratch,4096,0,0,0,0};
        if (!RemoteCallOnce(tid,maps,original,bias,remote_munmap,trap,
                            munmap_args,&report) || !report.detach_safe ||
            report.result != 0) uncertain = true;
        scratch = 0;
    }

    std::vector<Mapping> after_maps;
    std::uintptr_t bootstrap_base = 0;
    int stage = 0;
    Locator locator{};
    if (!uncertain) {
        after_maps = ReadMaps(pid);
        if (!UniqueOffsetZeroBase(after_maps,kBootstrapPath,
                                  bootstrap_file.status,&bootstrap_base) ||
            !ReadProcessValue(pid,bootstrap_base+stage_symbol.st_value,&stage) ||
            (stage != 2 && stage != 5) ||
            !ReadStableLocator(pid,bootstrap_base+locator_symbol.st_value,&locator) ||
            locator.magic != kLocatorMagic || locator.version != 2 ||
            locator.size != sizeof(Locator) ||
            !ExactString(locator.payload_path,sizeof(locator.payload_path),kPayloadPath) ||
            !ExactString(locator.payload_sha256,sizeof(locator.payload_sha256),
                         kPayloadSha256) ||
            !ExactString(locator.payload_build_id,sizeof(locator.payload_build_id),
                         kPayloadBuildId))
            uncertain = true;
        if (!uncertain &&
            !ExactString(locator.payload_source_sha256,
                         sizeof(locator.payload_source_sha256),
                         kPayloadSourceSha256))
            uncertain = true;
    }

    if (uncertain) {
        const bool killed = KillUncertainProcess(pid);
        return CarrierFail("remote_load",14,killed);
    }
    if (ptrace(PTRACE_DETACH,tid,nullptr,nullptr) == -1) {
        const bool killed = KillUncertainProcess(pid);
        return CarrierFail("detach",15,killed);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!IsAlive(pid,expected_ticks) || TracerPid(pid) != 0) {
        const bool killed = KillUncertainProcess(pid);
        return CarrierFail("post_detach_identity",16,killed);
    }
    std::printf(
        "HABI1_EARLY_CARRIER passed=1 pid=%d tid=%d start_ticks=%llu "
        "bias=%ld fixed_bootstrap=1 handle=%p stage=%d rollback=1 detach=1 "
        "libc_sha256=%s trap_rva=0x%llx resolver_gettid=%s "
        "resolver_mmap=%s resolver_munmap=%s resolver_dlopen=%s\n",
        pid,tid,static_cast<unsigned long long>(expected_ticks),bias,
        reinterpret_cast<void*>(handle),stage,
        kCarrierLibcSha256,
        static_cast<unsigned long long>(kCarrierLibcTrapRva),
        gettid_module.c_str(),mmap_module.c_str(),munmap_module.c_str(),
        dlopen_module.c_str());
    return 0;
}
