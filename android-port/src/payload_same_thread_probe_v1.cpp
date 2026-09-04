#include <jni.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdint>

// Deliberately JNI-shaped because NativeBridge getTrampoline consumes a JNI
// shorty descriptor. The probe ignores both implicit JNI arguments, touches no
// game memory and returns only the Linux TID on which the guest code executed.
extern "C" __attribute__((visibility("default"))) jlong
a9tas_same_thread_probe_v1(JNIEnv*, jobject) {
    return static_cast<jlong>(syscall(__NR_gettid));
}

extern "C" __attribute__((visibility("default"))) std::uint32_t
a9tas_same_thread_probe_protocol_v1() {
    return 1;
}
