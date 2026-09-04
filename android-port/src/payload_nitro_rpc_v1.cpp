// ARM64 guest payload for tick-synchronous calls to the game's real nitro
// activation entry. This payload installs no hooks and patches no game code.
// A dedicated guest thread accepts a narrowly-scoped request over a local Unix
// socket, validates the exact game build/object/vtable/function chain, and then
// calls the existing NitroService vslot +0x108 at most twice.
//
// Live execution is deliberately gated: nonzero activation requests must claim
// that the external HWBP controller currently holds the game tick writer
// stopped. The caller must also provide the current race vehicle owner.

#include "nitro_rpc_protocol_v1.h"

#include <android/log.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

#ifndef A9TAS_GAME_ACTION_RPC_V1
#define A9TAS_GAME_ACTION_RPC_V1 0
#endif

#ifndef A9TAS_GAME_ACTION_RPC_ENABLE_EXECUTE
#define A9TAS_GAME_ACTION_RPC_ENABLE_EXECUTE 0
#endif

#ifndef A9TAS_NITRO_RPC_ENABLE_SERVER_THREAD
#define A9TAS_NITRO_RPC_ENABLE_SERVER_THREAD 1
#endif

#ifndef A9TAS_NITRO_RPC_ENABLE_STATUS_WRITES
#define A9TAS_NITRO_RPC_ENABLE_STATUS_WRITES 1
#endif

using a9tas::nitro_rpc_v1::Request;
using a9tas::nitro_rpc_v1::Response;
using a9tas::nitro_rpc_v1::Result;
using a9tas::nitro_rpc_v1::StateSnapshot;

#if A9TAS_GAME_ACTION_RPC_V1
constexpr const char* kTag = "A9TAS_GAME_ACTION_RPC_V1";
constexpr const char* kProtocolName = "game-action-rpc-v1";
constexpr const char* kSocketPath =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/"
    "a9tas-game-action-rpc-v1.sock";
constexpr const char* kStatusPath =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/"
    "a9tas-game-action-rpc-v1.status";
#else
constexpr const char* kTag = "A9TAS_NITRO_RPC_V1";
constexpr const char* kProtocolName = "nitro-rpc-v1";
constexpr const char* kSocketPath =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/"
    "a9tas-nitro-rpc-v1.sock";
constexpr const char* kStatusPath =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/"
    "a9tas-nitro-rpc-v1.status";
#endif
constexpr std::uint8_t kExpectedBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};
constexpr std::uintptr_t kServiceOffset = 0xCB8;
constexpr std::uintptr_t kServiceVtableRva = 0x7EE8A90;
constexpr std::uintptr_t kServiceActivateSlot = 0x108;
constexpr std::uintptr_t kServiceActivateRva = 0x3674E50;
#if A9TAS_GAME_ACTION_RPC_V1
// Exact no-argument action path used by the game for command vslot +0x158.
// When direct_mode is zero it constructs the game's own ref-counted completion
// and task objects, submits the task through the owner scheduler, and retains
// the completion token in owner+0x1360.  Do not replace this with raw queue or
// Nitro-state writes.
constexpr std::uintptr_t kGameActionDispatchRva = 0x367B414;
constexpr std::uintptr_t kCommandQueueOffset = 0x1360;
constexpr std::uintptr_t kDirectModeOffset = 0x1378;
constexpr std::size_t kMaxObservedCommandTokens = 4096;
#endif
// The live A9 CN process currently has about 7,000 mappings. The vehicle heap
// appears early, while libAsphalt9's vtable segment appears after line 4,000;
// a 1,024-entry cache therefore produced a safe false negative. Keep a fixed
// no-allocation table, but size it above the measured process and reject
// overflow instead of silently accepting an incomplete map view.
constexpr std::size_t kMaxRanges = 16384;

struct ReadableRange {
    std::uintptr_t begin;
    std::uintptr_t end;
};

struct GameMapping {
    std::uintptr_t map_base{};
    std::uintptr_t phdr_base{};
    char path[1024]{};
};

struct BaseDiagnostics {
    std::uintptr_t map_base{};
    std::uintptr_t phdr_base{};
    std::uint32_t flags{};
    char path[256]{};
};

ReadableRange g_ranges[kMaxRanges]{};
std::size_t g_range_count = 0;
std::atomic<std::uint64_t> g_last_sequence{0};
std::atomic<int> g_status{0};
std::atomic<std::uintptr_t> g_phdr_base{0};
std::atomic<std::uintptr_t> g_verified_base{0};

#if A9TAS_NITRO_RPC_ENABLE_SERVER_THREAD
bool WriteAll(int fd, const void* data, std::size_t size, int flags = 0) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = send(fd, bytes + done, size - done, flags);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool ReadAll(int fd, void* data, std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = recv(fd, bytes + done, size - done, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

void WriteStatus(const char* phase, int detail) {
    char text[512]{};
    const int length = std::snprintf(
        text, sizeof(text),
        "protocol=%s\npid=%d\nabi=arm64-v8a\n"
        "hooks=0\ncode_patches=0\nthreads=1\nphase=%s\ndetail=%d\n"
        "socket=%s\n",
        kProtocolName, static_cast<int>(getpid()), phase, detail,
        kSocketPath);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(text)) return;
    const int fd = open(kStatusPath,
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return;
    std::size_t done = 0;
    while (done < static_cast<std::size_t>(length)) {
        const ssize_t n = write(fd, text + done,
                                static_cast<std::size_t>(length) - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += static_cast<std::size_t>(n);
    }
    close(fd);
}
#endif

void WriteResolveFailureStatus(const BaseDiagnostics& diagnostics) {
#if A9TAS_NITRO_RPC_ENABLE_STATUS_WRITES
    char text[1024]{};
    const int length = std::snprintf(
        text, sizeof(text),
        "protocol=%s\npid=%d\nabi=arm64-v8a\n"
        "hooks=0\ncode_patches=0\nthreads=1\nphase=resolve_failed\n"
        "detail=%u\nmap_base=0x%" PRIxPTR "\nphdr_base=0x%" PRIxPTR
        "\npath=%s\n",
        kProtocolName, static_cast<int>(getpid()), diagnostics.flags,
        diagnostics.map_base, diagnostics.phdr_base, diagnostics.path);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(text)) return;
    const int fd = open(kStatusPath,
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return;
    std::size_t done = 0;
    while (done < static_cast<std::size_t>(length)) {
        const ssize_t n = write(fd, text + done,
                                static_cast<std::size_t>(length) - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += static_cast<std::size_t>(n);
    }
    close(fd);
#else
    (void)diagnostics;
#endif
}

bool RefreshReadableRanges() {
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (!maps) return false;
    char line[2048]{};
    std::size_t count = 0;
    bool overflow = false;
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long begin = 0, end = 0;
        char permissions[5]{};
        if (std::sscanf(line, "%llx-%llx %4s", &begin, &end,
                        permissions) == 3 &&
            permissions[0] == 'r' && begin < end) {
            if (count >= kMaxRanges) {
                overflow = true;
                break;
            }
            g_ranges[count++] = {static_cast<std::uintptr_t>(begin),
                                 static_cast<std::uintptr_t>(end)};
        }
    }
    std::fclose(maps);
    g_range_count = count;
    return count != 0 && !overflow;
}

bool IsReadable(std::uintptr_t address, std::size_t size) {
    if (!address || !size || address > UINTPTR_MAX - size) return false;
    const std::uintptr_t end = address + size;
    for (std::size_t i = 0; i < g_range_count; ++i) {
        if (address >= g_ranges[i].begin && end <= g_ranges[i].end) return true;
    }
    return false;
}

int FindGuestGameModule(dl_phdr_info* info, size_t, void*) {
    if (info && info->dlpi_name &&
        std::strstr(info->dlpi_name, "libAsphalt9.so")) {
        g_phdr_base.store(static_cast<std::uintptr_t>(info->dlpi_addr),
                          std::memory_order_release);
        return 1;
    }
    return 0;
}

bool FindGameMapping(GameMapping* output) {
    output->map_base = 0;
    output->phdr_base = 0;
    output->path[0] = '\0';
    dl_iterate_phdr(FindGuestGameModule, nullptr);
    output->phdr_base = g_phdr_base.load(std::memory_order_acquire);

    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (!maps) return false;
    char line[2048]{};
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long start = 0, end = 0, offset = 0;
        char permissions[5]{}, path[1024]{};
        const int fields = std::sscanf(
            line, "%llx-%llx %4s %llx %*s %*s %1023[^\n]", &start,
            &end, permissions, &offset, path);
        if (fields != 5 || offset != 0 ||
            !std::strstr(path, "libAsphalt9.so"))
            continue;
        char* clean = path;
        while (*clean == ' ') ++clean;
        output->map_base = static_cast<std::uintptr_t>(start);
        std::snprintf(output->path, sizeof(output->path), "%s", clean);
        break;
    }
    std::fclose(maps);
    return output->map_base != 0 || output->phdr_base != 0;
}

bool ReadBuildId(const char* path, std::uint8_t output[20]) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    Elf64_Ehdr header{};
    if (std::fread(&header, sizeof(header), 1, file) != 1 ||
        std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_machine != EM_AARCH64) {
        std::fclose(file);
        return false;
    }
    bool found = false;
    for (std::uint16_t i = 0; i < header.e_phnum && !found; ++i) {
        Elf64_Phdr phdr{};
        const long offset = static_cast<long>(header.e_phoff) +
                            static_cast<long>(i) * sizeof(phdr);
        if (std::fseek(file, offset, SEEK_SET) != 0 ||
            std::fread(&phdr, sizeof(phdr), 1, file) != 1)
            break;
        if (phdr.p_type != PT_NOTE || phdr.p_filesz > 1024 * 1024) continue;
        std::uint64_t cursor = phdr.p_offset;
        const std::uint64_t end = phdr.p_offset + phdr.p_filesz;
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
                std::fseek(file, static_cast<long>(cursor), SEEK_SET);
                std::fread(name, 1, note.n_namesz, file);
            }
            cursor += name_size;
            if (note.n_type == NT_GNU_BUILD_ID && note.n_descsz == 20 &&
                std::strcmp(name, "GNU") == 0) {
                std::fseek(file, static_cast<long>(cursor), SEEK_SET);
                found = std::fread(output, 20, 1, file) == 1;
                break;
            }
            cursor += desc_size;
        }
    }
    std::fclose(file);
    return found;
}

bool ResolveVerifiedBase(std::uintptr_t* base,
                         BaseDiagnostics* diagnostics) {
    std::memset(diagnostics, 0, sizeof(*diagnostics));
    const std::uintptr_t cached =
        g_verified_base.load(std::memory_order_acquire);
    if (cached != 0 && RefreshReadableRanges()) {
        *base = cached;
        return true;
    }
    GameMapping mapping{};
    if (!RefreshReadableRanges()) return false;
    diagnostics->flags |= 1u << 0;
    if (!FindGameMapping(&mapping)) return false;
    diagnostics->flags |= 1u << 1;
    diagnostics->map_base = mapping.map_base;
    diagnostics->phdr_base = mapping.phdr_base;
    std::snprintf(diagnostics->path, sizeof(diagnostics->path), "%s",
                  mapping.path);
    if (mapping.path[0]) {
        diagnostics->flags |= 1u << 2;
        std::uint8_t build_id[20]{};
        if (!ReadBuildId(mapping.path, build_id)) return false;
        diagnostics->flags |= 1u << 3;
        if (std::memcmp(build_id, kExpectedBuildId, sizeof(build_id)) != 0)
            return false;
        diagnostics->flags |= 1u << 4;
    }
    // Houdini does not expose executable guest bytes consistently to memcpy
    // performed by guest code, even though the same addresses are executable
    // and externally readable through /proc/<pid>/mem. Base verification here
    // therefore uses the exact ELF Build ID. The request path then requires
    // the live service vtable and its +0x108 function pointer to equal this
    // build's fixed RVAs. The external HWBP controller independently checks
    // four code signatures before sending any state-changing request.
    if (mapping.phdr_base != 0) {
        *base = mapping.phdr_base;
        g_verified_base.store(*base, std::memory_order_release);
        return true;
    }
    if (mapping.map_base != 0) {
        *base = mapping.map_base;
        g_verified_base.store(*base, std::memory_order_release);
        return true;
    }
    return false;
}

bool Snapshot(std::uintptr_t state, StateSnapshot* output) {
    if (!IsReadable(state + 0x180, 0x40)) return false;
    std::memset(output, 0, sizeof(*output));
    output->optional_180 =
        *reinterpret_cast<const std::uint8_t*>(state + 0x180);
    output->optional_value_184 =
        *reinterpret_cast<const std::uint32_t*>(state + 0x184);
    output->active_188 =
        *reinterpret_cast<const std::uint8_t*>(state + 0x188);
    output->mode_18c =
        *reinterpret_cast<const std::uint32_t*>(state + 0x18C);
    std::memcpy(output->gates_1b8_1bc,
                reinterpret_cast<const void*>(state + 0x1B8), 5);
    return true;
}

void InitializeResponse(const Request& request, Response* response) {
    std::memset(response, 0, sizeof(*response));
    std::memcpy(response->magic, a9tas::nitro_rpc_v1::kResponseMagic,
                sizeof(response->magic));
    response->version = a9tas::nitro_rpc_v1::kVersion;
    response->size = sizeof(*response);
    response->sequence = request.sequence;
    response->result = static_cast<std::int32_t>(Result::kInternalError);
    response->vehicle_owner = request.vehicle_owner;
}

Result HandleRequest(const Request& request, Response* response) {
    const bool basic_ok =
        std::memcmp(request.magic, a9tas::nitro_rpc_v1::kRequestMagic,
                    sizeof(request.magic)) == 0 &&
        request.version == a9tas::nitro_rpc_v1::kVersion &&
        request.size == sizeof(request) && request.sequence != 0;
    const bool observe = request.activations == 0 &&
                         request.flags ==
                             a9tas::nitro_rpc_v1::kFlagObserveOnly;
    const bool execute =
        request.activations >= 1 &&
        request.activations <= a9tas::nitro_rpc_v1::kMaxActivations &&
        request.flags ==
            a9tas::nitro_rpc_v1::kFlagExternalTickStopped;
    if (!basic_ok || (!observe && !execute)) return Result::kBadRequest;
#if A9TAS_GAME_ACTION_RPC_V1 && !A9TAS_GAME_ACTION_RPC_ENABLE_EXECUTE
    // The isolated action-route artifact is observation-only until natural
    // producer affinity and scheduler ordering have separate live evidence.
    if (execute) return Result::kBadRequest;
#endif

    const std::uint64_t last = g_last_sequence.load(std::memory_order_acquire);
    if (request.sequence <= last) return Result::kReplaySequence;

    std::uintptr_t base = 0;
    BaseDiagnostics diagnostics{};
    if (!ResolveVerifiedBase(&base, &diagnostics)) {
        WriteResolveFailureStatus(diagnostics);
        __android_log_print(
            ANDROID_LOG_ERROR, kTag,
            "base verification failed flags=0x%x map=0x%" PRIxPTR
            " phdr=0x%" PRIxPTR " path=%s",
            diagnostics.flags, diagnostics.map_base, diagnostics.phdr_base,
            diagnostics.path);
        return Result::kUnsupportedBuild;
    }
    response->guest_base = base;
    const auto owner = static_cast<std::uintptr_t>(request.vehicle_owner);
    if ((owner & 7u) != 0 || !IsReadable(owner + kServiceOffset,
                                         sizeof(std::uintptr_t)))
        return Result::kUnreadableVehicle;
    const std::uintptr_t service =
        *reinterpret_cast<const std::uintptr_t*>(owner + kServiceOffset);
    response->service_object = service;
    if (!IsReadable(service, sizeof(std::uintptr_t)) || (service & 7u) != 0)
        return Result::kInvalidService;
    const std::uintptr_t vtable =
        *reinterpret_cast<const std::uintptr_t*>(service);
    if (vtable != base + kServiceVtableRva ||
        !IsReadable(vtable + kServiceActivateSlot, sizeof(std::uintptr_t)))
        return Result::kInvalidService;
    const std::uintptr_t service_dispatch =
        *reinterpret_cast<const std::uintptr_t*>(vtable +
                                                kServiceActivateSlot);
    if (service_dispatch != base + kServiceActivateRva)
        return Result::kInvalidDispatch;

#if A9TAS_GAME_ACTION_RPC_V1
    if (!IsReadable(owner + kCommandQueueOffset, 3 * sizeof(std::uintptr_t)) ||
        !IsReadable(owner + kDirectModeOffset, sizeof(std::uint8_t)))
        return Result::kUnreadableVehicle;
    const auto* queue = reinterpret_cast<const std::uintptr_t*>(
        owner + kCommandQueueOffset);
    const std::uintptr_t queue_begin = queue[0];
    const std::uintptr_t queue_end = queue[1];
    const std::uintptr_t queue_capacity = queue[2];
    const bool queue_shape_ok =
        queue_begin <= queue_end && queue_end <= queue_capacity &&
        ((queue_end - queue_begin) % sizeof(std::uintptr_t)) == 0 &&
        ((queue_capacity - queue_begin) % sizeof(std::uintptr_t)) == 0 &&
        (queue_end - queue_begin) / sizeof(std::uintptr_t) <=
            kMaxObservedCommandTokens;
    if (!queue_shape_ok) return Result::kUnreadableVehicle;
    response->dispatch_function = base + kGameActionDispatchRva;
#else
    response->dispatch_function = service_dispatch;
#endif

    const std::uintptr_t state = service + 8;
    if (!Snapshot(state, &response->before)) return Result::kUnreadableState;
    if (execute && response->before.gates_1b8_1bc[1] == 0)
        return Result::kActivationGateClosed;

    // Consume the sequence before the first state-changing call. If the call
    // completes but the post-call snapshot or socket reply fails, retrying the
    // same sequence must never activate nitro a second time.
    g_last_sequence.store(request.sequence, std::memory_order_release);
    if (execute) {
#if A9TAS_GAME_ACTION_RPC_V1 && A9TAS_GAME_ACTION_RPC_ENABLE_EXECUTE
        // This is intentionally unreachable in the default isolated build.
        // If a later reviewed build enables it, require the asynchronous
        // game-owned scheduling branch.  Entering the direct branch on this
        // RPC worker would reproduce the already-rejected wrong affinity.
        const auto direct_mode = *reinterpret_cast<const std::uint8_t*>(
            owner + kDirectModeOffset);
        if (direct_mode != 0) return Result::kActivationGateClosed;
        using DispatchActionFn = void (*)(void*);
        auto dispatch_action = reinterpret_cast<DispatchActionFn>(
            response->dispatch_function);
        for (std::uint32_t i = 0; i < request.activations; ++i) {
            dispatch_action(reinterpret_cast<void*>(owner));
            response->calls_completed = i + 1;
        }
#elif A9TAS_GAME_ACTION_RPC_V1
        // Defense in depth: no action-call instruction is compiled into the
        // observation-only artifact even if the earlier request gate regresses.
        return Result::kBadRequest;
#else
        using ActivateFn = void (*)(void*);
        auto activate = reinterpret_cast<ActivateFn>(service_dispatch);
        for (std::uint32_t i = 0; i < request.activations; ++i) {
            activate(reinterpret_cast<void*>(service));
            response->calls_completed = i + 1;
        }
#endif
    }
    if (!RefreshReadableRanges() || !Snapshot(state, &response->after))
        return Result::kUnreadableState;
    return Result::kOk;
}

#if A9TAS_NITRO_RPC_ENABLE_SERVER_THREAD
void ServeClient(int client) {
    while (true) {
        Request request{};
        if (!ReadAll(client, &request, sizeof(request))) break;
        Response response{};
        InitializeResponse(request, &response);
        const Result result = HandleRequest(request, &response);
        response.result = static_cast<std::int32_t>(result);
        if (!WriteAll(client, &response, sizeof(response), MSG_NOSIGNAL)) break;
    }
}

void* ServerThread(void*) {
    unlink(kSocketPath);
    const int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server < 0) {
        g_status.store(-errno, std::memory_order_release);
        WriteStatus("socket_failed", errno);
        return nullptr;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (std::strlen(kSocketPath) >= sizeof(address.sun_path)) {
        g_status.store(-ENAMETOOLONG, std::memory_order_release);
        WriteStatus("path_too_long", ENAMETOOLONG);
        close(server);
        return nullptr;
    }
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s",
                  kSocketPath);
    if (bind(server, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0 ||
        chmod(kSocketPath, 0600) != 0 || listen(server, 1) != 0) {
        const int saved = errno;
        g_status.store(-saved, std::memory_order_release);
        WriteStatus("bind_or_listen_failed", saved);
        close(server);
        unlink(kSocketPath);
        return nullptr;
    }
    g_status.store(1, std::memory_order_release);
    WriteStatus("ready", 0);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "ready hooks=0 code_patches=0 socket=%s", kSocketPath);
    while (true) {
        const int client = accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR) continue;
            const int saved = errno;
            g_status.store(-saved, std::memory_order_release);
            WriteStatus("accept_failed", saved);
            break;
        }
        ServeClient(client);
        close(client);
    }
    close(server);
    unlink(kSocketPath);
    return nullptr;
}

__attribute__((constructor)) void OnLoad() {
    WriteStatus("starting", 0);
    pthread_t thread{};
    const int result = pthread_create(&thread, nullptr, ServerThread, nullptr);
    if (result != 0) {
        g_status.store(-result, std::memory_order_release);
        WriteStatus("thread_failed", result);
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "server thread create failed error=%d", result);
        return;
    }
    pthread_detach(thread);
}
#endif

}  // namespace

extern "C" __attribute__((visibility("default"))) int
a9tas_nitro_rpc_v1_status() {
    return g_status.load(std::memory_order_acquire);
}
