#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr std::size_t kOwnerSize = 0x2000;
constexpr std::size_t kInterfaceOffset = 0x30;
constexpr std::size_t kDirectModeOffset = 0x1378;

std::atomic<bool> g_run{true};
std::atomic<pid_t> g_writer_tid{0};

void* WriterMain(void* raw_owner) {
    pthread_setname_np(pthread_self(), "DTA0 Writer");
    g_writer_tid.store(static_cast<pid_t>(syscall(__NR_gettid)),
                       std::memory_order_release);
    auto* direct = reinterpret_cast<volatile std::uint8_t*>(
        static_cast<std::uint8_t*>(raw_owner) + kDirectModeOffset);
    const timespec half_frame{0, 8'000'000};
    while (g_run.load(std::memory_order_acquire)) {
        *direct = 1;
        nanosleep(&half_frame, nullptr);
        *direct = 0;
        nanosleep(&half_frame, nullptr);
    }
    *direct = 0;
    return nullptr;
}

void* FrameMain(void*) {
    pthread_setname_np(pthread_self(), "FrameThread 0");
    while (g_run.load(std::memory_order_acquire)) {
        asm volatile("pause" ::: "memory");
    }
    return nullptr;
}

}  // namespace

int main() {
    void* owner = mmap(nullptr, kOwnerSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (owner == MAP_FAILED) return 2;
    std::memset(owner, 0, kOwnerSize);
    std::memcpy(static_cast<std::uint8_t*>(owner) + kInterfaceOffset, &owner,
                sizeof(owner));

    pthread_t writer{};
    pthread_t frame{};
    if (pthread_create(&writer, nullptr, WriterMain, owner) != 0 ||
        pthread_create(&frame, nullptr, FrameMain, nullptr) != 0) {
        return 3;
    }
    while (g_writer_tid.load(std::memory_order_acquire) == 0) usleep(1000);

    std::printf("DTA0_FIXTURE_V1_READY pid=%d owner=%p writer_tid=%d "
                "writer_name=DTA0_Writer frame_name=FrameThread_0\n",
                getpid(), owner,
                static_cast<int>(g_writer_tid.load(std::memory_order_acquire)));
    std::fflush(stdout);

    while (g_run.load(std::memory_order_acquire)) pause();
    pthread_join(frame, nullptr);
    pthread_join(writer, nullptr);
    munmap(owner, kOwnerSize);
    return 0;
}
