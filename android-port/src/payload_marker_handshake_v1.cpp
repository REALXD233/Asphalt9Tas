#include <android/log.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace {
constexpr const char* kTag = "A9TAS_MARKER_V1";
constexpr const char* kMarkerPath =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/"
    "a9tas-marker-handshake-v1.status";
constexpr const char* kProtocol = "marker-only-v1";
std::atomic<int> g_status{0};

pid_t CurrentTid() {
    return static_cast<pid_t>(syscall(SYS_gettid));
}

void WriteMarker() {
    const pid_t pid = getpid();
    const pid_t tid = CurrentTid();
    char record[256]{};
    const int length = std::snprintf(
        record, sizeof(record),
        "protocol=%s\npid=%d\ntid=%d\nabi=arm64-v8a\nhooks=0\nthreads=0\n",
        kProtocol, static_cast<int>(pid), static_cast<int>(tid));
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(record)) {
        g_status.store(-1, std::memory_order_release);
        return;
    }

    const int fd = open(kMarkerPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                        0600);
    if (fd < 0) {
        const int saved_errno = errno;
        g_status.store(-saved_errno, std::memory_order_release);
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "constructor marker open failed errno=%d", saved_errno);
        return;
    }

    std::size_t written = 0;
    while (written < static_cast<std::size_t>(length)) {
        const ssize_t result =
            write(fd, record + written,
                  static_cast<std::size_t>(length) - written);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) break;
        written += static_cast<std::size_t>(result);
    }
    const int close_result = close(fd);
    if (written != static_cast<std::size_t>(length) || close_result != 0) {
        const int saved_errno = errno != 0 ? errno : EIO;
        g_status.store(-saved_errno, std::memory_order_release);
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "constructor marker write failed errno=%d bytes=%zu/%d",
                            saved_errno, written, length);
        return;
    }

    g_status.store(1, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "constructor marker written pid=%d tid=%d hooks=0 threads=0",
                        static_cast<int>(pid), static_cast<int>(tid));
}

__attribute__((constructor)) void OnLoad() {
    WriteMarker();
}
}  // namespace

extern "C" __attribute__((visibility("default"))) int
a9tas_marker_handshake_status() {
    return g_status.load(std::memory_order_acquire);
}

