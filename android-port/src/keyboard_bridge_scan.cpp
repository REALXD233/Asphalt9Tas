#include <algorithm>
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

struct Mapping {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    char perms[5]{};
    std::string path;
};

constexpr std::uintptr_t kImageSize = 0xA5D8168;
constexpr std::uintptr_t kParentVtableRva = 0x9EBCB50;
constexpr std::uintptr_t kGameplayGatewayRva = 0x385A0E8;
int gWatchMilliseconds = 0;
std::uintptr_t g_final_a = 0;
std::uintptr_t g_final_b = 0;

bool ReadExact(int fd, std::uintptr_t address, void* output, std::size_t size) {
    auto* cursor = static_cast<std::uint8_t*>(output);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = pread64(fd, cursor + done, size - done,
                                  static_cast<off64_t>(address + done));
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

std::vector<Mapping> ReadMaps(pid_t pid) {
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* file = std::fopen(path, "re");
    std::vector<Mapping> maps;
    if (!file) return maps;
    char line[2048]{};
    while (std::fgets(line, sizeof(line), file)) {
        unsigned long long begin = 0, end = 0, offset = 0;
        char perms[5]{}, name[1400]{};
        const int fields = std::sscanf(
            line, "%llx-%llx %4s %llx %*s %*s %1399[^\n]", &begin, &end,
            perms, &offset, name);
        if (fields < 4) continue;
        Mapping map{};
        map.begin = static_cast<std::uintptr_t>(begin);
        map.end = static_cast<std::uintptr_t>(end);
        std::memcpy(map.perms, perms, sizeof(map.perms));
        if (fields == 5) {
            const char* clean = name;
            while (*clean == ' ') ++clean;
            map.path = clean;
        }
        maps.push_back(std::move(map));
    }
    std::fclose(file);
    return maps;
}

const Mapping* FindMapping(const std::vector<Mapping>& maps,
                           std::uintptr_t address, std::size_t size) {
    if (!address || !size || address > UINTPTR_MAX - size) return nullptr;
    for (const auto& map : maps) {
        if (map.perms[0] == 'r' && address >= map.begin &&
            address + size <= map.end)
            return &map;
    }
    return nullptr;
}

void PrintModulePointer(const char* label, std::uintptr_t value,
                        std::uintptr_t base) {
    if (value >= base && value < base + kImageSize) {
        std::printf(" %s=0x%" PRIxPTR "(lib+0x%" PRIxPTR ")", label,
                    value, value - base);
    } else {
        std::printf(" %s=0x%" PRIxPTR, label, value);
    }
}

void PrintModuleRefs(const char* label, const std::uint8_t* bytes,
                     std::size_t size, std::uintptr_t base) {
    std::printf("  %s_module_refs", label);
    bool any = false;
    for (std::size_t offset = 0; offset + sizeof(std::uintptr_t) <= size;
         offset += sizeof(std::uintptr_t)) {
        std::uintptr_t value = 0;
        std::memcpy(&value, bytes + offset, sizeof(value));
        if (value >= base && value < base + kImageSize) {
            std::printf(" +0x%zx=lib+0x%" PRIxPTR, offset, value - base);
            any = true;
        }
    }
    if (!any) std::printf(" <none>");
    std::printf("\n");
}

void DescribeVirtualObject(int fd, const std::vector<Mapping>& maps,
                           const char* label, std::uintptr_t object,
                           const std::uintptr_t* slots,
                           std::size_t slot_count, std::uintptr_t base) {
    std::uintptr_t vtable = 0;
    if (!FindMapping(maps, object, sizeof(vtable)) ||
        !ReadExact(fd, object, &vtable, sizeof(vtable))) {
        std::printf("       CONTROL_DEP label=%s object=0x%" PRIxPTR
                    " <unreadable>\n",
                    label, object);
        return;
    }

    std::printf("       CONTROL_DEP label=%s object=0x%" PRIxPTR, label,
                object);
    PrintModulePointer("vtable", vtable, base);
    for (std::size_t index = 0; index < slot_count; ++index) {
        std::uintptr_t function = 0;
        const auto slot = slots[index];
        if (FindMapping(maps, vtable + slot, sizeof(function)))
            ReadExact(fd, vtable + slot, &function, sizeof(function));
        char slot_label[32]{};
        std::snprintf(slot_label, sizeof(slot_label), "vfunc_%02" PRIxPTR,
                      slot);
        PrintModulePointer(slot_label, function, base);
    }
    std::printf("\n");
}

void DescribeGameplayController(int fd, const std::vector<Mapping>& maps,
                                std::uintptr_t controller,
                                std::uintptr_t base) {
    std::uint8_t bytes[0x120]{};
    if (!FindMapping(maps, controller, sizeof(bytes)) ||
        !ReadExact(fd, controller, bytes, sizeof(bytes))) {
        std::printf("       GAMEPLAY_CONTROLLER object=0x%" PRIxPTR
                    " <unreadable>\n",
                    controller);
        return;
    }

    std::printf("       GAMEPLAY_CONTROLLER object=0x%" PRIxPTR "\n",
                controller);
    PrintModuleRefs("gameplay_controller", bytes, sizeof(bytes), base);
    std::printf("       GAMEPLAY_CONTROLLER_QWORDS");
    for (std::size_t offset = 0; offset < sizeof(bytes);
         offset += sizeof(std::uintptr_t)) {
        std::uintptr_t value = 0;
        std::memcpy(&value, bytes + offset, sizeof(value));
        std::printf(" +0x%zx=0x%" PRIxPTR, offset, value);
    }
    std::printf("\n");

    std::uintptr_t dependency = 0;
    constexpr std::uintptr_t kSinkSlots[] = {0x20, 0x28, 0x80, 0x90};
    constexpr std::uintptr_t kCommandSlots[] = {0x28, 0x90};
    constexpr std::uintptr_t kGenericSlots[] = {0x10, 0x18, 0x20, 0x28};
    constexpr std::uintptr_t kSourceSlots[] = {0x10, 0x18, 0x20};

    std::memcpy(&dependency, bytes + 0x10, sizeof(dependency));
    DescribeVirtualObject(fd, maps, "sink_plus_10", dependency, kSinkSlots,
                          sizeof(kSinkSlots) / sizeof(kSinkSlots[0]), base);
    std::memcpy(&dependency, bytes + 0x18, sizeof(dependency));
    DescribeVirtualObject(fd, maps, "command_plus_18", dependency,
                          kCommandSlots,
                          sizeof(kCommandSlots) / sizeof(kCommandSlots[0]),
                          base);
    std::memcpy(&dependency, bytes + 0x20, sizeof(dependency));
    DescribeVirtualObject(fd, maps, "dependency_plus_20", dependency,
                          kGenericSlots,
                          sizeof(kGenericSlots) / sizeof(kGenericSlots[0]),
                          base);
    std::memcpy(&dependency, bytes + 0x48, sizeof(dependency));
    DescribeVirtualObject(fd, maps, "input_source_plus_48", dependency,
                          kSourceSlots,
                          sizeof(kSourceSlots) / sizeof(kSourceSlots[0]),
                          base);
    std::memcpy(&dependency, bytes + 0x50, sizeof(dependency));
    DescribeVirtualObject(fd, maps, "queried_interface_plus_50", dependency,
                          kGenericSlots,
                          sizeof(kGenericSlots) / sizeof(kGenericSlots[0]),
                          base);

    auto print_float_field = [&](const char* label, std::uintptr_t address) {
        float value = 0.0F;
        if (FindMapping(maps, address, sizeof(value)) &&
            ReadExact(fd, address, &value, sizeof(value)))
            std::printf(" %s@0x%" PRIxPTR "=%.9g", label, address,
                        static_cast<double>(value));
        else
            std::printf(" %s@0x%" PRIxPTR "=<unreadable>", label, address);
    };

    // Runtime vtable metadata proves both source getters adjust back to the
    // same owner before reading owner+0x3C / owner+0x94. Capture the targets
    // and smoothed outputs without assigning semantic control names yet.
    std::uintptr_t source = 0, source_vtable = 0;
    std::uintptr_t source_value_a = 0, source_value_b = 0;
    std::memcpy(&source, bytes + 0x48, sizeof(source));
    if (FindMapping(maps, source, sizeof(source_vtable)) &&
        ReadExact(fd, source, &source_vtable, sizeof(source_vtable))) {
        std::int64_t adjustment_a = 0, adjustment_b = 0;
        if (source_vtable >= 0x40 &&
            ReadExact(fd, source_vtable - 0x38, &adjustment_a,
                      sizeof(adjustment_a)) &&
            ReadExact(fd, source_vtable - 0x40, &adjustment_b,
                      sizeof(adjustment_b))) {
            const auto owner_a = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(source) + adjustment_a);
            const auto owner_b = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(source) + adjustment_b);
            source_value_a = owner_a + 0x3C;
            source_value_b = owner_b + 0x94;
            std::printf("       INPUT_SOURCE_FIELDS owner_a=0x%" PRIxPTR
                        " owner_b=0x%" PRIxPTR,
                        owner_a, owner_b);
            print_float_field("target_30", owner_a + 0x30);
            print_float_field("target_34", owner_b + 0x34);
            print_float_field("value_A_3c", owner_a + 0x3C);
            print_float_field("value_B_94", owner_b + 0x94);
            std::printf("\n");
        }
    }

    // The two sink vfuncs use vtable metadata to adjust to a common owner,
    // then store the exact float bits at owner+0xE78 / owner+0xE7C.
    std::uintptr_t sink = 0, sink_vtable = 0;
    std::uintptr_t sink_value_a = 0, sink_value_b = 0;
    std::uintptr_t sink_value_x = 0;
    std::memcpy(&sink, bytes + 0x10, sizeof(sink));
    if (FindMapping(maps, sink, sizeof(sink_vtable)) &&
        ReadExact(fd, sink, &sink_vtable, sizeof(sink_vtable))) {
        std::int64_t adjustment_value_b = 0, adjustment_value_a = 0;
        if (sink_vtable >= 0x38 &&
            ReadExact(fd, sink_vtable - 0x30, &adjustment_value_b,
                      sizeof(adjustment_value_b)) &&
            ReadExact(fd, sink_vtable - 0x38, &adjustment_value_a,
                      sizeof(adjustment_value_a))) {
            const auto owner_b = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(sink) + adjustment_value_b);
            const auto owner_a = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(sink) + adjustment_value_a);
            sink_value_b = owner_b + 0xE78;
            sink_value_a = owner_a + 0xE7C;
            sink_value_x = owner_a + 0xE84;
            std::printf("       CONTROL_SINK_FIELDS owner_a=0x%" PRIxPTR
                        " owner_b=0x%" PRIxPTR,
                        owner_a, owner_b);
            print_float_field("value_B_e78", owner_b + 0xE78);
            print_float_field("value_A_e7c", owner_a + 0xE7C);
            print_float_field("value_X_e84", owner_a + 0xE84);
            std::printf("\n");
            std::printf("       EXTENDED_CONTROL_FIELDS owner=0x%" PRIxPTR,
                        owner_a);
            print_float_field("value_e60", owner_a + 0xE60);
            print_float_field("value_e68", owner_a + 0xE68);
            print_float_field("value_e6c", owner_a + 0xE6C);
            print_float_field("value_e74", owner_a + 0xE74);
            std::printf("\n");

            // Static evidence: two paths read +0xE78/+0xE7C and call
            // vtable+0x2A8 / vtable+0x2B0 on the object at owner+0x30.
            // Resolve those runtime function slots without naming semantics.
            std::uintptr_t downstream = 0, downstream_vtable = 0;
            if (FindMapping(maps, owner_a + 0x30, sizeof(downstream)) &&
                ReadExact(fd, owner_a + 0x30, &downstream,
                          sizeof(downstream)) &&
                FindMapping(maps, downstream, sizeof(downstream_vtable)) &&
                ReadExact(fd, downstream, &downstream_vtable,
                          sizeof(downstream_vtable))) {
                std::uintptr_t vfunc_2a8 = 0, vfunc_2b0 = 0;
                if (FindMapping(maps, downstream_vtable + 0x2A8,
                                sizeof(vfunc_2a8)))
                    ReadExact(fd, downstream_vtable + 0x2A8, &vfunc_2a8,
                              sizeof(vfunc_2a8));
                if (FindMapping(maps, downstream_vtable + 0x2B0,
                                sizeof(vfunc_2b0)))
                    ReadExact(fd, downstream_vtable + 0x2B0, &vfunc_2b0,
                              sizeof(vfunc_2b0));
                std::printf("       DOWNSTREAM_DISPATCH object=0x%" PRIxPTR,
                            downstream);
                PrintModulePointer("vtable", downstream_vtable, base);
                PrintModulePointer("vfunc_2a8", vfunc_2a8, base);
                PrintModulePointer("vfunc_2b0", vfunc_2b0, base);
                std::printf("\n");

                // sub_36AA578: wrappers first adjust the interface pointer
                // using vtable metadata at -0x2D8 (slot 0x2A8) / -0x2E0
                // (slot 0x2B0), then dereference owner+0xA0 to reach the
                // inner object whose vtable+0x2A8/+0x2B0 is the final consumer.
                std::int64_t adjustment_2a8 = 0, adjustment_2b0 = 0;
                std::uintptr_t owner_2a8 = 0, owner_2b0 = 0;
                if (downstream_vtable >= 0x2E0 &&
                    FindMapping(maps, downstream_vtable - 0x2D8,
                                sizeof(adjustment_2a8)) &&
                    ReadExact(fd, downstream_vtable - 0x2D8,
                              &adjustment_2a8, sizeof(adjustment_2a8)) &&
                    FindMapping(maps, downstream_vtable - 0x2E0,
                                sizeof(adjustment_2b0)) &&
                    ReadExact(fd, downstream_vtable - 0x2E0,
                              &adjustment_2b0, sizeof(adjustment_2b0))) {
                    owner_2a8 = static_cast<std::uintptr_t>(
                        static_cast<std::intptr_t>(downstream) +
                        adjustment_2a8);
                    owner_2b0 = static_cast<std::uintptr_t>(
                        static_cast<std::intptr_t>(downstream) +
                        adjustment_2b0);
                    std::printf("       DOWNSTREAM_OWNER owner_2a8=0x%"
                                PRIxPTR " adjustment_2a8=%" PRId64
                                " owner_2b0=0x%" PRIxPTR
                                " adjustment_2b0=%" PRId64 "\n",
                                owner_2a8, adjustment_2a8, owner_2b0,
                                adjustment_2b0);
                }
                std::uintptr_t inner_2a8_obj = 0, inner_2b8_obj = 0;
                std::uintptr_t inner_2a8_vtable = 0, inner_2b0_vtable = 0;
                std::uintptr_t inner_2a8_fn = 0, inner_2b0_fn = 0;
                if (owner_2a8 &&
                    FindMapping(maps, owner_2a8 + 0xA0,
                                sizeof(inner_2a8_obj)) &&
                    ReadExact(fd, owner_2a8 + 0xA0, &inner_2a8_obj,
                              sizeof(inner_2a8_obj)) &&
                    FindMapping(maps, inner_2a8_obj,
                                sizeof(inner_2a8_vtable)) &&
                    ReadExact(fd, inner_2a8_obj, &inner_2a8_vtable,
                              sizeof(inner_2a8_vtable))) {
                    if (FindMapping(maps, inner_2a8_vtable + 0x2A8,
                                    sizeof(inner_2a8_fn)))
                        ReadExact(fd, inner_2a8_vtable + 0x2A8,
                                  &inner_2a8_fn, sizeof(inner_2a8_fn));
                    std::printf("       DOWNSTREAM_INNER_2A8 object=0x%"
                                PRIxPTR,
                                inner_2a8_obj);
                    PrintModulePointer("vtable", inner_2a8_vtable, base);
                    PrintModulePointer("vfunc_2a8", inner_2a8_fn, base);
                    std::printf("\n");
                    std::int64_t inner_adj_2a8 = 0;
                    std::uintptr_t final_owner_2a8 = 0;
                    if (inner_2a8_vtable >= 0x2D8 &&
                        FindMapping(maps, inner_2a8_vtable - 0x2D8,
                                    sizeof(inner_adj_2a8)) &&
                        ReadExact(fd, inner_2a8_vtable - 0x2D8,
                                  &inner_adj_2a8, sizeof(inner_adj_2a8))) {
                        final_owner_2a8 = static_cast<std::uintptr_t>(
                            static_cast<std::intptr_t>(inner_2a8_obj) +
                            inner_adj_2a8);
                        std::printf("       DOWNSTREAM_FINAL_2A8 owner=0x%"
                                    PRIxPTR " adjustment=%" PRId64,
                                    final_owner_2a8, inner_adj_2a8);
                        print_float_field("value_C98", final_owner_2a8 + 0xC98);
                        std::printf("\n");
                    }
                }
                if (owner_2b0 &&
                    FindMapping(maps, owner_2b0 + 0xA0,
                                sizeof(inner_2b8_obj)) &&
                    ReadExact(fd, owner_2b0 + 0xA0, &inner_2b8_obj,
                              sizeof(inner_2b8_obj)) &&
                    FindMapping(maps, inner_2b8_obj,
                                sizeof(inner_2b0_vtable)) &&
                    ReadExact(fd, inner_2b8_obj, &inner_2b0_vtable,
                              sizeof(inner_2b0_vtable))) {
                    if (FindMapping(maps, inner_2b0_vtable + 0x2B0,
                                    sizeof(inner_2b0_fn)))
                        ReadExact(fd, inner_2b0_vtable + 0x2B0,
                                  &inner_2b0_fn, sizeof(inner_2b0_fn));
                    std::printf("       DOWNSTREAM_INNER_2B0 object=0x%"
                                PRIxPTR,
                                inner_2b8_obj);
                    PrintModulePointer("vtable", inner_2b0_vtable, base);
                    PrintModulePointer("vfunc_2b0", inner_2b0_fn, base);
                    std::printf("\n");
                    std::int64_t inner_adj_2b0 = 0;
                    std::uintptr_t final_owner_2b0 = 0;
                    if (inner_2b0_vtable >= 0x2E0 &&
                        FindMapping(maps, inner_2b0_vtable - 0x2E0,
                                    sizeof(inner_adj_2b0)) &&
                        ReadExact(fd, inner_2b0_vtable - 0x2E0,
                                  &inner_adj_2b0, sizeof(inner_adj_2b0))) {
                        final_owner_2b0 = static_cast<std::uintptr_t>(
                            static_cast<std::intptr_t>(inner_2b8_obj) +
                            inner_adj_2b0);
                        std::printf("       DOWNSTREAM_FINAL_2B0 owner=0x%"
                                    PRIxPTR " adjustment=%" PRId64,
                                    final_owner_2b0, inner_adj_2b0);
                        print_float_field("value_C9C", final_owner_2b0 + 0xC9C);
                        std::printf("\n");
                    }
                }
            }
        }
    }

    // The per-tick update appends its third float to the buffer rooted at
    // controller+0xD8. Preserve the raw header and a bounded sample without
    // assigning steering/brake/accelerator labels before dynamic proof.
    std::printf("       VALUE_C_BUFFER_HEADER");
    for (std::size_t offset = 0xD8; offset < 0x100;
         offset += sizeof(std::uintptr_t)) {
        std::uintptr_t value = 0;
        std::memcpy(&value, bytes + offset, sizeof(value));
        std::printf(" +0x%zx=0x%" PRIxPTR, offset, value);
    }
    std::printf("\n");
    std::uintptr_t sample = 0, sample_end = 0;
    std::memcpy(&sample, bytes + 0xD8, sizeof(sample));
    std::memcpy(&sample_end, bytes + 0xE0, sizeof(sample_end));
    if (sample_end >= sample && FindMapping(maps, sample, sizeof(float))) {
        const auto available = static_cast<std::size_t>(
            (sample_end - sample) / sizeof(float));
        const auto sample_count = std::min<std::size_t>(available, 16);
        std::printf("       VALUE_C_BUFFER_FLOATS address=0x%" PRIxPTR,
                    sample);
        for (std::size_t index = 0; index < sample_count; ++index) {
            float value = 0.0F;
            if (!FindMapping(maps, sample + index * sizeof(value),
                             sizeof(value)) ||
                !ReadExact(fd, sample + index * sizeof(value), &value,
                           sizeof(value)))
                break;
            std::printf(" [%zu]=%.9g", index, static_cast<double>(value));
        }
        std::printf("\n");
    }

    if (gWatchMilliseconds > 0 && source_value_a && source_value_b &&
        sink_value_a && sink_value_b && sink_value_x) {
        struct Sample {
            std::uint32_t source_a{};
            std::uint32_t source_b{};
            std::uint32_t sink_a{};
            std::uint32_t sink_b{};
            std::uint32_t sink_x{};
        } current{}, previous{};
        bool have_previous = false;
        timespec started{};
        clock_gettime(CLOCK_MONOTONIC, &started);
        std::printf("       VALUE_WATCH_BEGIN duration_ms=%d interval_ms=5\n",
                    gWatchMilliseconds);
        std::fflush(stdout);
        for (;;) {
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            const auto elapsed_ms =
                static_cast<std::int64_t>(now.tv_sec - started.tv_sec) * 1000 +
                (now.tv_nsec - started.tv_nsec) / 1000000;
            if (elapsed_ms > gWatchMilliseconds) break;
            if (!ReadExact(fd, source_value_a, &current.source_a,
                           sizeof(current.source_a)) ||
                !ReadExact(fd, source_value_b, &current.source_b,
                           sizeof(current.source_b)) ||
                !ReadExact(fd, sink_value_a, &current.sink_a,
                           sizeof(current.sink_a)) ||
                !ReadExact(fd, sink_value_b, &current.sink_b,
                           sizeof(current.sink_b)) ||
                !ReadExact(fd, sink_value_x, &current.sink_x,
                           sizeof(current.sink_x))) {
                std::printf("       VALUE_WATCH_READ_FAILED t_ms=%" PRId64
                            "\n",
                            elapsed_ms);
                std::fflush(stdout);
                break;
            }
            if (!have_previous ||
                std::memcmp(&current, &previous, sizeof(current)) != 0) {
                float source_a = 0.0F, source_b = 0.0F;
                float sink_a = 0.0F, sink_b = 0.0F, sink_x = 0.0F;
                std::memcpy(&source_a, &current.source_a, sizeof(source_a));
                std::memcpy(&source_b, &current.source_b, sizeof(source_b));
                std::memcpy(&sink_a, &current.sink_a, sizeof(sink_a));
                std::memcpy(&sink_b, &current.sink_b, sizeof(sink_b));
                std::memcpy(&sink_x, &current.sink_x, sizeof(sink_x));
                std::printf(
                    "       VALUE_WATCH t_ms=%" PRId64
                    " source_A=%.9g source_B=%.9g sink_A=%.9g sink_B=%.9g"
                    " sink_X_e84=%.9g\n",
                    elapsed_ms, static_cast<double>(source_a),
                    static_cast<double>(source_b),
                    static_cast<double>(sink_a),
                    static_cast<double>(sink_b),
                    static_cast<double>(sink_x));
                std::fflush(stdout);
                previous = current;
                have_previous = true;
            }
            usleep(5000);
        }
        std::printf("       VALUE_WATCH_END\n");
        std::fflush(stdout);
    }
}

void DescribeActionList(int fd, const std::vector<Mapping>& maps,
                        const char* label, std::uintptr_t container,
                        std::uintptr_t base, bool recurse = true) {
    std::uintptr_t head = 0;
    std::uint64_t count = 0;
    if (!FindMapping(maps, container, 0x20) ||
        !ReadExact(fd, container, &head, sizeof(head)) ||
        !ReadExact(fd, container + 0x10, &count, sizeof(count))) {
        std::printf("     ACTION_LIST label=%s container=0x%" PRIxPTR
                    " <unreadable>\n",
                    label, container);
        return;
    }

    std::printf("     ACTION_LIST label=%s container=0x%" PRIxPTR
                " head=0x%" PRIxPTR " count=%" PRIu64 "\n",
                label, container, head, count);
    if (count == 0 || count > 64 || !FindMapping(maps, head, 8)) return;

    std::uintptr_t node = head;
    for (std::uint64_t index = 0; index < count && index < 32; ++index) {
        if (node < 0x10 || !FindMapping(maps, node - 0x10, 0x100)) break;
        const auto object = node - 0x10;
        std::uint8_t object_bytes[0x100]{};
        if (!ReadExact(fd, object, object_bytes, sizeof(object_bytes))) break;

        std::uintptr_t vtable = 0, callback = 0;
        std::memcpy(&vtable, object_bytes, sizeof(vtable));
        if (FindMapping(maps, vtable + 0x40, sizeof(callback)))
            ReadExact(fd, vtable + 0x40, &callback, sizeof(callback));
        std::printf("      ACTION_SUB index=%" PRIu64
                    " node=0x%" PRIxPTR " object=0x%" PRIxPTR,
                    index, node, object);
        PrintModulePointer("vtable", vtable, base);
        PrintModulePointer("callback40", callback, base);
        std::printf("\n");
        PrintModuleRefs("action_object", object_bytes, sizeof(object_bytes),
                        base);

        std::uintptr_t holder = 0, target_fn = 0, target_raw = 0;
        std::int64_t adjustment = 0;
        std::memcpy(&holder, object_bytes + 0x50, sizeof(holder));
        std::memcpy(&target_fn, object_bytes + 0x58, sizeof(target_fn));
        std::memcpy(&adjustment, object_bytes + 0x60, sizeof(adjustment));
        if (FindMapping(maps, holder, sizeof(target_raw)))
            ReadExact(fd, holder, &target_raw, sizeof(target_raw));
        const auto target_this = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(target_raw) + (adjustment >> 1));
        std::printf("       ACTION_BIND holder=0x%" PRIxPTR
                    " target_raw=0x%" PRIxPTR " adjustment=%" PRId64
                    " target_this=0x%" PRIxPTR,
                    holder, target_raw, adjustment, target_this);
        PrintModulePointer("target_fn", target_fn, base);
        std::printf("\n");

        if (recurse && FindMapping(maps, target_this, sizeof(std::uintptr_t))) {
            std::uintptr_t target_vtable = 0;
            ReadExact(fd, target_this, &target_vtable, sizeof(target_vtable));

            auto describe_adjusted_list =
                [&](const char* nested_label, std::intptr_t metadata_offset,
                    std::uintptr_t list_offset) {
                    std::int64_t list_adjustment = 0;
                    const auto metadata = static_cast<std::uintptr_t>(
                        static_cast<std::intptr_t>(target_vtable) +
                        metadata_offset);
                    if (!FindMapping(maps, metadata, sizeof(list_adjustment)) ||
                        !ReadExact(fd, metadata, &list_adjustment,
                                   sizeof(list_adjustment)))
                        return;
                    const auto nested_container =
                        static_cast<std::uintptr_t>(
                            static_cast<std::intptr_t>(target_this) +
                            list_adjustment) +
                        list_offset;
                    std::printf(
                        "       SECOND_STAGE label=%s vtable=0x%" PRIxPTR
                        " adjustment=%" PRId64 " container=0x%" PRIxPTR
                        "\n",
                        nested_label, target_vtable, list_adjustment,
                        nested_container);
                    DescribeActionList(fd, maps, nested_label,
                                       nested_container, base, false);
                };

            if (target_fn == base + 0x386B748) {
                DescribeGameplayController(fd, maps, target_this, base);
                std::uint8_t flags[10]{};
                if (FindMapping(maps, target_this + 0x100, sizeof(flags)) &&
                    ReadExact(fd, target_this + 0x100, flags,
                              sizeof(flags))) {
                    std::printf("       ACTION07_STATE bytes_100_109=");
                    for (std::uint8_t value : flags)
                        std::printf("%02x", static_cast<unsigned>(value));
                    std::printf("\n");
                }
                std::uintptr_t fallback_object = 0, fallback_vtable = 0;
                std::uintptr_t fallback_fn = 0;
                if (ReadExact(fd, target_this + 0x10, &fallback_object,
                              sizeof(fallback_object)) &&
                    FindMapping(maps, fallback_object,
                                sizeof(fallback_vtable)) &&
                    ReadExact(fd, fallback_object, &fallback_vtable,
                              sizeof(fallback_vtable)) &&
                    FindMapping(maps, fallback_vtable + 0x90,
                                sizeof(fallback_fn)))
                    ReadExact(fd, fallback_vtable + 0x90, &fallback_fn,
                              sizeof(fallback_fn));
                std::printf("       ACTION07_FALLBACK object=0x%" PRIxPTR,
                            fallback_object);
                PrintModulePointer("vtable", fallback_vtable, base);
                PrintModulePointer("vfunc90", fallback_fn, base);
                std::printf("\n");
                // vfunc90 eventually reaches the no-argument command path at
                // lib+0x367B414.  That path either invokes interface vslot
                // +0x158 immediately or appends a ref-counted command pointer
                // to the vector rooted at the adjusted complete owner+0x1360.
                // Keep this inspection read-only: stale queue pointers must
                // never be copied back into a later race instance.
                std::uintptr_t queue_begin = 0, queue_end = 0;
                std::uintptr_t queue_capacity = 0, command_interface = 0;
                std::uintptr_t command_owner = 0;
                std::int64_t owner_adjustment = 0;
                std::uint8_t direct_mode = 0;
                bool owner_adjustment_ok =
                    fallback_object && fallback_vtable >= 0xA0 &&
                    FindMapping(maps, fallback_vtable - 0xA0,
                                sizeof(owner_adjustment)) &&
                    ReadExact(fd, fallback_vtable - 0xA0,
                              &owner_adjustment, sizeof(owner_adjustment));
                const auto signed_fallback =
                    static_cast<std::intptr_t>(fallback_object);
                if (owner_adjustment_ok) {
                    owner_adjustment_ok =
                        (owner_adjustment > 0 &&
                         signed_fallback <= INTPTR_MAX - owner_adjustment) ||
                        (owner_adjustment < 0 &&
                         signed_fallback >= INTPTR_MIN - owner_adjustment) ||
                        owner_adjustment == 0;
                }
                if (owner_adjustment_ok) {
                    command_owner = static_cast<std::uintptr_t>(
                        signed_fallback + owner_adjustment);
                }
                const bool have_nitro_queue =
                    command_owner != 0 &&
                    FindMapping(maps, command_owner + 0x30,
                                sizeof(command_interface)) &&
                    ReadExact(fd, command_owner + 0x30,
                              &command_interface,
                              sizeof(command_interface)) &&
                    FindMapping(maps, command_owner + 0x1360, 0x19) &&
                    ReadExact(fd, command_owner + 0x1360, &queue_begin,
                              sizeof(queue_begin)) &&
                    ReadExact(fd, command_owner + 0x1368, &queue_end,
                              sizeof(queue_end)) &&
                    ReadExact(fd, command_owner + 0x1370,
                              &queue_capacity, sizeof(queue_capacity)) &&
                    ReadExact(fd, command_owner + 0x1378, &direct_mode,
                              sizeof(direct_mode));
                if (have_nitro_queue) {
                    const bool ordered = queue_begin <= queue_end &&
                                         queue_end <= queue_capacity;
                    const std::uint64_t queue_count =
                        ordered ? (queue_end - queue_begin) /
                                      sizeof(std::uintptr_t)
                                : UINT64_MAX;
                    std::printf(
                        "       ACTION07_COMMAND_PATH owner_adjustment=%" PRId64
                        " owner=0x%" PRIxPTR " interface_vtable=0x%" PRIxPTR
                        " queue_begin=0x%" PRIxPTR
                        " queue_end=0x%" PRIxPTR
                        " queue_capacity=0x%" PRIxPTR
                        " queue_count=%" PRIu64 " direct_mode=%u\n",
                        owner_adjustment, command_owner, command_interface,
                        queue_begin, queue_end,
                        queue_capacity, queue_count,
                        static_cast<unsigned>(direct_mode));

                    // Mirror sub_368762C exactly to resolve the interface
                    // object and the no-argument command target at vslot
                    // +0x158.  Names remain neutral until the target's state
                    // effects are dynamically proven.
                    std::uintptr_t interface_head = 0;
                    std::int64_t interface_adjustment = 0;
                    std::uintptr_t dispatch_this = 0, dispatch_vtable = 0;
                    std::uintptr_t dispatch_vfunc_158 = 0;
                    bool dispatch_ok =
                        FindMapping(maps, command_interface,
                                    sizeof(interface_head)) &&
                        ReadExact(fd, command_interface, &interface_head,
                                  sizeof(interface_head)) &&
                        interface_head >= 0x230 &&
                        FindMapping(maps, interface_head - 0x230,
                                    sizeof(interface_adjustment)) &&
                        ReadExact(fd, interface_head - 0x230,
                                  &interface_adjustment,
                                  sizeof(interface_adjustment));
                    const auto signed_interface =
                        static_cast<std::intptr_t>(command_interface);
                    if (dispatch_ok) {
                        dispatch_ok =
                            (interface_adjustment > 0 &&
                             signed_interface <=
                                 INTPTR_MAX - interface_adjustment) ||
                            (interface_adjustment < 0 &&
                             signed_interface >=
                                 INTPTR_MIN - interface_adjustment) ||
                            interface_adjustment == 0;
                    }
                    if (dispatch_ok) {
                        dispatch_this = static_cast<std::uintptr_t>(
                            signed_interface + interface_adjustment);
                        dispatch_ok =
                            FindMapping(maps, dispatch_this,
                                        sizeof(dispatch_vtable)) &&
                            ReadExact(fd, dispatch_this, &dispatch_vtable,
                                      sizeof(dispatch_vtable)) &&
                            FindMapping(maps, dispatch_vtable + 0x158,
                                        sizeof(dispatch_vfunc_158)) &&
                            ReadExact(fd, dispatch_vtable + 0x158,
                                      &dispatch_vfunc_158,
                                      sizeof(dispatch_vfunc_158));
                    }
                    if (dispatch_ok) {
                        std::printf(
                            "       ACTION07_DISPATCH interface_head=0x%"
                            PRIxPTR " adjustment=%" PRId64
                            " dispatch_this=0x%" PRIxPTR,
                            interface_head, interface_adjustment,
                            dispatch_this);
                        PrintModulePointer("dispatch_vtable", dispatch_vtable,
                                           base);
                        PrintModulePointer("vfunc_158", dispatch_vfunc_158,
                                           base);
                        std::printf("\n");

                        // The observed vslot is a RigidBodyWrapper delegate
                        // thunk.  Resolve its runtime backend using
                        // RigidBodyWrapper_adjust_backend_delegate so the
                        // final implementation can be analyzed statically.
                        std::uintptr_t backend_interface = 0;
                        std::uintptr_t backend_head = 0;
                        std::int64_t backend_adjustment = 0;
                        std::uintptr_t backend_this = 0, backend_vtable = 0;
                        std::uintptr_t backend_vfunc_158 = 0;
                        bool backend_ok =
                            FindMapping(maps, dispatch_this + 0xA0,
                                        sizeof(backend_interface)) &&
                            ReadExact(fd, dispatch_this + 0xA0,
                                      &backend_interface,
                                      sizeof(backend_interface)) &&
                            FindMapping(maps, backend_interface,
                                        sizeof(backend_head)) &&
                            ReadExact(fd, backend_interface, &backend_head,
                                      sizeof(backend_head)) &&
                            backend_head >= 0x230 &&
                            FindMapping(maps, backend_head - 0x230,
                                        sizeof(backend_adjustment)) &&
                            ReadExact(fd, backend_head - 0x230,
                                      &backend_adjustment,
                                      sizeof(backend_adjustment));
                        const auto signed_backend_interface =
                            static_cast<std::intptr_t>(backend_interface);
                        if (backend_ok) {
                            backend_ok =
                                (backend_adjustment > 0 &&
                                 signed_backend_interface <=
                                     INTPTR_MAX - backend_adjustment) ||
                                (backend_adjustment < 0 &&
                                 signed_backend_interface >=
                                     INTPTR_MIN - backend_adjustment) ||
                                backend_adjustment == 0;
                        }
                        if (backend_ok) {
                            backend_this = static_cast<std::uintptr_t>(
                                signed_backend_interface +
                                backend_adjustment);
                            backend_ok =
                                FindMapping(maps, backend_this,
                                            sizeof(backend_vtable)) &&
                                ReadExact(fd, backend_this, &backend_vtable,
                                          sizeof(backend_vtable)) &&
                                FindMapping(maps, backend_vtable + 0x158,
                                            sizeof(backend_vfunc_158)) &&
                                ReadExact(fd, backend_vtable + 0x158,
                                          &backend_vfunc_158,
                                          sizeof(backend_vfunc_158));
                        }
                        if (backend_ok) {
                            std::printf(
                                "       ACTION07_BACKEND interface=0x%"
                                PRIxPTR " adjustment=%" PRId64
                                " backend_this=0x%" PRIxPTR,
                                backend_interface, backend_adjustment,
                                backend_this);
                            PrintModulePointer("backend_vtable",
                                               backend_vtable, base);
                            PrintModulePointer("vfunc_158",
                                               backend_vfunc_158, base);
                            std::printf("\n");

                            // backend vfunc+0x158 is another adjustment thunk
                            // that reaches sub_36A2970.  Resolve the complete
                            // vehicle-side owner and the service object used by
                            // that function's final vslot+0x108 dispatch.
                            std::int64_t vehicle_adjustment = 0;
                            std::uintptr_t vehicle_owner = 0;
                            std::uintptr_t service_object = 0;
                            std::uintptr_t service_vtable = 0;
                            std::uintptr_t service_vfunc_108 = 0;
                            std::uintptr_t service_vfunc_148 = 0;
                            bool service_ok =
                                backend_vtable >= 0x178 &&
                                FindMapping(maps, backend_vtable - 0x178,
                                            sizeof(vehicle_adjustment)) &&
                                ReadExact(fd, backend_vtable - 0x178,
                                          &vehicle_adjustment,
                                          sizeof(vehicle_adjustment));
                            const auto signed_backend_this =
                                static_cast<std::intptr_t>(backend_this);
                            if (service_ok) {
                                service_ok =
                                    (vehicle_adjustment > 0 &&
                                     signed_backend_this <=
                                         INTPTR_MAX - vehicle_adjustment) ||
                                    (vehicle_adjustment < 0 &&
                                     signed_backend_this >=
                                         INTPTR_MIN - vehicle_adjustment) ||
                                    vehicle_adjustment == 0;
                            }
                            if (service_ok) {
                                vehicle_owner =
                                    static_cast<std::uintptr_t>(
                                        signed_backend_this +
                                        vehicle_adjustment);
                                service_ok =
                                    FindMapping(maps, vehicle_owner + 0xCB8,
                                                sizeof(service_object)) &&
                                    ReadExact(fd, vehicle_owner + 0xCB8,
                                              &service_object,
                                              sizeof(service_object)) &&
                                    FindMapping(maps, service_object,
                                                sizeof(service_vtable)) &&
                                    ReadExact(fd, service_object,
                                              &service_vtable,
                                              sizeof(service_vtable)) &&
                                    FindMapping(maps,
                                                service_vtable + 0x148,
                                                sizeof(service_vfunc_148)) &&
                                    ReadExact(fd, service_vtable + 0x148,
                                              &service_vfunc_148,
                                              sizeof(service_vfunc_148)) &&
                                    FindMapping(maps,
                                                service_vtable + 0x108,
                                                sizeof(service_vfunc_108)) &&
                                    ReadExact(fd, service_vtable + 0x108,
                                              &service_vfunc_108,
                                              sizeof(service_vfunc_108));
                            }
                            if (service_ok) {
                                std::printf(
                                    "       ACTION07_SERVICE adjustment=%"
                                    PRId64 " vehicle_owner=0x%" PRIxPTR
                                    " service_object=0x%" PRIxPTR,
                                    vehicle_adjustment, vehicle_owner,
                                    service_object);
                                PrintModulePointer("service_vtable",
                                                   service_vtable, base);
                                PrintModulePointer("vfunc_148",
                                                   service_vfunc_148, base);
                                PrintModulePointer("vfunc_108",
                                                   service_vfunc_108, base);
                                std::printf("\n");

                                // vfunc+0x108 resolves through lib+0x3674E50
                                // to the state machine at lib+0x36D8524.  The
                                // wrapper adds 8 to service_object before the
                                // state-machine call.  Preserve raw fields;
                                // color/mode labels require controlled dynamic
                                // correlation and must not be guessed here.
                                const std::uintptr_t nitro_state =
                                    service_object + 8;
                                std::uint8_t optional_180 = 0;
                                std::uint32_t optional_value_184 = 0;
                                std::uint8_t active_188 = 0;
                                std::uint32_t mode_18c = 0;
                                std::uint8_t gate_1b8 = 0, gate_1b9 = 0;
                                std::uint8_t gate_1ba = 0, gate_1bb = 0;
                                std::uint8_t gate_1bc = 0;
                                const bool nitro_state_ok =
                                    FindMapping(maps, nitro_state + 0x180,
                                                0x3D) &&
                                    ReadExact(fd, nitro_state + 0x180,
                                              &optional_180,
                                              sizeof(optional_180)) &&
                                    ReadExact(fd, nitro_state + 0x184,
                                              &optional_value_184,
                                              sizeof(optional_value_184)) &&
                                    ReadExact(fd, nitro_state + 0x188,
                                              &active_188,
                                              sizeof(active_188)) &&
                                    ReadExact(fd, nitro_state + 0x18C,
                                              &mode_18c,
                                              sizeof(mode_18c)) &&
                                    ReadExact(fd, nitro_state + 0x1B8,
                                              &gate_1b8,
                                              sizeof(gate_1b8)) &&
                                    ReadExact(fd, nitro_state + 0x1B9,
                                              &gate_1b9,
                                              sizeof(gate_1b9)) &&
                                    ReadExact(fd, nitro_state + 0x1BA,
                                              &gate_1ba,
                                              sizeof(gate_1ba)) &&
                                    ReadExact(fd, nitro_state + 0x1BB,
                                              &gate_1bb,
                                              sizeof(gate_1bb)) &&
                                    ReadExact(fd, nitro_state + 0x1BC,
                                              &gate_1bc,
                                              sizeof(gate_1bc));
                                if (nitro_state_ok) {
                                    std::printf(
                                        "       ACTION07_NITRO_STATE object=0x%"
                                        PRIxPTR " optional_180=%u"
                                        " optional_value_184=%u active_188=%u"
                                        " mode_18c=%u gates_1b8_1bc="
                                        "%u%u%u%u%u\n",
                                        nitro_state,
                                        static_cast<unsigned>(optional_180),
                                        optional_value_184,
                                        static_cast<unsigned>(active_188),
                                        mode_18c,
                                        static_cast<unsigned>(gate_1b8),
                                        static_cast<unsigned>(gate_1b9),
                                        static_cast<unsigned>(gate_1ba),
                                        static_cast<unsigned>(gate_1bb),
                                        static_cast<unsigned>(gate_1bc));
                                } else {
                                    std::printf(
                                        "       ACTION07_NITRO_STATE"
                                        " <unreadable>\n");
                                }
                            } else {
                                std::printf(
                                    "       ACTION07_SERVICE <unreadable>\n");
                            }
                        } else {
                            std::printf(
                                "       ACTION07_BACKEND <unreadable>\n");
                        }
                    } else {
                        std::printf("       ACTION07_DISPATCH <unreadable>\n");
                    }
                } else {
                    std::printf("       ACTION07_COMMAND_PATH <unreadable>\n");
                }
                describe_adjusted_list("action07_second_stage", -0x20, 0x08);
            } else if (target_fn == base + 0x386B798) {
                std::uintptr_t predicate_fn = 0;
                if (FindMapping(maps, target_vtable + 0xD8,
                                sizeof(predicate_fn)))
                    ReadExact(fd, target_vtable + 0xD8, &predicate_fn,
                              sizeof(predicate_fn));
                std::uintptr_t command_object = 0, command_vtable = 0;
                std::uintptr_t command_fn = 0;
                ReadExact(fd, target_this + 0x18, &command_object,
                          sizeof(command_object));
                if (FindMapping(maps, command_object, sizeof(command_vtable)) &&
                    ReadExact(fd, command_object, &command_vtable,
                              sizeof(command_vtable)) &&
                    FindMapping(maps, command_vtable + 0x28,
                                sizeof(command_fn)))
                    ReadExact(fd, command_vtable + 0x28, &command_fn,
                              sizeof(command_fn));
                std::printf("       ACTION_MODE_COMMAND");
                PrintModulePointer("predicate_vfunc_d8", predicate_fn, base);
                std::printf(" object=0x%" PRIxPTR, command_object);
                PrintModulePointer("object_vtable", command_vtable, base);
                PrintModulePointer("command_vfunc28", command_fn, base);
                std::printf(" argument=4\n");
            } else if (target_fn == base + 0x386B7E4) {
                describe_adjusted_list("action23_second_stage", -0x18, 0x58);
            } else if (target_fn == base + 0x386B858) {
                describe_adjusted_list("actions18_1c_second_stage", -0x18,
                                       0xD0);
            } else if (target_fn == base + 0x386B884) {
                describe_adjusted_list("action25_second_stage", -0x18, 0xF8);
            } else if (target_fn == base + 0x386B86C) {
                describe_adjusted_list("action36_second_stage", -0x18, 0x120);
            }
        }

        std::memcpy(&node, object_bytes + 0x10, sizeof(node));
    }
}

void DescribeParent(int fd, const std::vector<Mapping>& maps,
                    std::uintptr_t parent, std::uintptr_t base) {
    std::uint8_t parent_bytes[0x80]{};
    if (!ReadExact(fd, parent, parent_bytes, sizeof(parent_bytes))) return;
    std::uintptr_t head = 0;
    std::uint64_t count = 0;
    std::memcpy(&head, parent_bytes + 0x18, sizeof(head));
    std::memcpy(&count, parent_bytes + 0x28, sizeof(count));
    const Mapping* parent_map = FindMapping(maps, parent, sizeof(parent_bytes));
    std::printf("PARENT object=0x%" PRIxPTR " map=%s head=0x%" PRIxPTR
                " count=%" PRIu64 "\n",
                parent,
                parent_map && !parent_map->path.empty()
                    ? parent_map->path.c_str()
                    : "[anonymous]",
                head, count);
    PrintModuleRefs("parent", parent_bytes, sizeof(parent_bytes), base);

    if (count == 0 || count > 64 || !FindMapping(maps, head, 8)) return;
    std::uintptr_t node = head;
    for (std::uint64_t index = 0; index < count && index < 16; ++index) {
        if (node < 0x10 || !FindMapping(maps, node - 0x10, 0x80)) break;
        const std::uintptr_t object = node - 0x10;
        std::uint8_t object_bytes[0x80]{};
        if (!ReadExact(fd, object, object_bytes, sizeof(object_bytes))) break;

        std::uintptr_t vtable = 0, owner = 0, callback = 0;
        std::uint64_t refcount = 0;
        std::uint8_t removed = object_bytes[0x28];
        std::memcpy(&vtable, object_bytes, sizeof(vtable));
        std::memcpy(&owner, object_bytes + 0x20, sizeof(owner));
        std::memcpy(&refcount, object_bytes + 0x30, sizeof(refcount));
        if (FindMapping(maps, vtable + 0x40, sizeof(callback)))
            ReadExact(fd, vtable + 0x40, &callback, sizeof(callback));

        std::printf(" SUB index=%" PRIu64 " node=0x%" PRIxPTR
                    " object=0x%" PRIxPTR " owner=0x%" PRIxPTR
                    " removed=%u refcount=%" PRIu64,
                    index, node, object, owner, static_cast<unsigned>(removed),
                    refcount);
        PrintModulePointer("vtable", vtable, base);
        PrintModulePointer("callback40", callback, base);
        std::printf("\n");
        PrintModuleRefs("object", object_bytes, sizeof(object_bytes), base);

        std::uintptr_t holder = 0, bound_target = 0;
        std::int64_t bound_adjustment = 0;
        std::memcpy(&holder, object_bytes + 0x50, sizeof(holder));
        std::memcpy(&bound_target, object_bytes + 0x58, sizeof(bound_target));
        std::memcpy(&bound_adjustment, object_bytes + 0x60,
                    sizeof(bound_adjustment));
        std::uintptr_t target_raw = 0;
        if (FindMapping(maps, holder, sizeof(target_raw)))
            ReadExact(fd, holder, &target_raw, sizeof(target_raw));
        const auto target_this = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(target_raw) +
            (bound_adjustment >> 1));
        std::printf("  binding holder=0x%" PRIxPTR
                    " target_raw=0x%" PRIxPTR
                    " adjustment=%" PRId64
                    " target_this=0x%" PRIxPTR,
                    holder, target_raw, bound_adjustment, target_this);
        PrintModulePointer("target_fn", bound_target, base);
        std::printf("\n");

        std::uint8_t target_bytes[0x100]{};
        if (FindMapping(maps, target_this, sizeof(target_bytes)) &&
            ReadExact(fd, target_this, target_bytes, sizeof(target_bytes))) {
            std::printf("  target_qwords");
            for (std::size_t target_offset = 0;
                 target_offset < sizeof(target_bytes);
                 target_offset += sizeof(std::uintptr_t)) {
                std::uintptr_t target_value = 0;
                std::memcpy(&target_value, target_bytes + target_offset,
                            sizeof(target_value));
                std::printf(" +0x%zx=0x%" PRIxPTR, target_offset,
                            target_value);
            }
            std::printf("\n");
        }

        // The observed bridge target at lib+0x5BAF9C8 applies vtable metadata
        // at -0x48 and then calls the same keyboard subscriber dispatcher.
        // Resolve that third-level list directly from the captured binding.
        if (bound_target == base + 0x5BAF9C8 &&
            FindMapping(maps, target_this, sizeof(std::uintptr_t))) {
            std::uintptr_t target_vtable = 0;
            ReadExact(fd, target_this, &target_vtable, sizeof(target_vtable));
            std::int64_t dispatch_adjustment = 0;
            if (target_vtable >= 0x48 &&
                FindMapping(maps, target_vtable - 0x48,
                            sizeof(dispatch_adjustment)) &&
                ReadExact(fd, target_vtable - 0x48, &dispatch_adjustment,
                          sizeof(dispatch_adjustment))) {
                const auto dispatch_base = static_cast<std::uintptr_t>(
                    static_cast<std::intptr_t>(target_this) +
                    dispatch_adjustment + 8);
                // KeyboardAction_DispatchSubscribers uses head at input+0,
                // count at input+0x10 and a traversal guard at input+0x18.
                const auto final_container = dispatch_base;
                std::uintptr_t final_head = 0;
                std::uint64_t final_count = 0;
                if (FindMapping(maps, final_container, 0x18) &&
                    ReadExact(fd, final_container, &final_head,
                              sizeof(final_head)) &&
                    ReadExact(fd, final_container + 0x10, &final_count,
                              sizeof(final_count))) {
                    std::printf("  FINAL_LIST target_vtable=0x%" PRIxPTR
                                " dispatch_adjustment=%" PRId64
                                " container=0x%" PRIxPTR
                                " head=0x%" PRIxPTR " count=%" PRIu64 "\n",
                                target_vtable, dispatch_adjustment,
                                final_container, final_head, final_count);
                    std::uintptr_t final_node = final_head;
                    for (std::uint64_t final_index = 0;
                         final_index < final_count && final_index < 32;
                         ++final_index) {
                        if (final_node < 0x10 ||
                            !FindMapping(maps, final_node - 0x10, 0x100))
                            break;
                        const auto final_object = final_node - 0x10;
                        std::uint8_t final_bytes[0x100]{};
                        if (!ReadExact(fd, final_object, final_bytes,
                                       sizeof(final_bytes)))
                            break;
                        std::uintptr_t final_vtable = 0, final_callback = 0;
                        std::memcpy(&final_vtable, final_bytes,
                                    sizeof(final_vtable));
                        if (FindMapping(maps, final_vtable + 0x40,
                                        sizeof(final_callback)))
                            ReadExact(fd, final_vtable + 0x40,
                                      &final_callback,
                                      sizeof(final_callback));
                        std::printf("   FINAL_SUB index=%" PRIu64
                                    " node=0x%" PRIxPTR
                                    " object=0x%" PRIxPTR,
                                    final_index, final_node, final_object);
                        PrintModulePointer("vtable", final_vtable, base);
                        PrintModulePointer("callback40", final_callback, base);
                        std::printf("\n");
                        PrintModuleRefs("final_object", final_bytes,
                                        sizeof(final_bytes), base);

                        std::uintptr_t final_holder = 0;
                        std::uintptr_t final_target_fn = 0;
                        std::int64_t final_adjustment = 0;
                        std::memcpy(&final_holder, final_bytes + 0x50,
                                    sizeof(final_holder));
                        std::memcpy(&final_target_fn, final_bytes + 0x58,
                                    sizeof(final_target_fn));
                        std::memcpy(&final_adjustment, final_bytes + 0x60,
                                    sizeof(final_adjustment));
                        std::uintptr_t final_target_raw = 0;
                        if (FindMapping(maps, final_holder,
                                        sizeof(final_target_raw)))
                            ReadExact(fd, final_holder, &final_target_raw,
                                      sizeof(final_target_raw));
                        const auto final_target_this =
                            static_cast<std::uintptr_t>(
                                static_cast<std::intptr_t>(final_target_raw) +
                                (final_adjustment >> 1));
                        std::printf("    FINAL_BIND holder=0x%" PRIxPTR
                                    " target_raw=0x%" PRIxPTR
                                    " adjustment=%" PRId64
                                    " target_this=0x%" PRIxPTR,
                                    final_holder, final_target_raw,
                                    final_adjustment, final_target_this);
                        PrintModulePointer("target_fn", final_target_fn, base);
                        std::printf("\n");

                        if (final_target_fn == base + 0x5BC8384) {
                            std::uintptr_t token_holder = 0, token = 0;
                            std::memcpy(&token_holder, final_bytes + 0x80,
                                        sizeof(token_holder));
                            if (FindMapping(maps, token_holder, sizeof(token)))
                                ReadExact(fd, token_holder, &token,
                                          sizeof(token));
                            std::printf("    GATEWAY_TOKEN holder=0x%" PRIxPTR
                                        " value=0x%" PRIxPTR "\n",
                                        token_holder, token);
                        }

                        if (final_target_fn == base + kGameplayGatewayRva &&
                            FindMapping(maps, final_target_this,
                                        sizeof(std::uintptr_t))) {
                            std::uintptr_t gateway_vtable = 0;
                            ReadExact(fd, final_target_this, &gateway_vtable,
                                      sizeof(gateway_vtable));
                            std::int64_t gateway_adjustment = 0;
                            if (gateway_vtable >= 0x18 &&
                                FindMapping(maps, gateway_vtable - 0x18,
                                            sizeof(gateway_adjustment)) &&
                                ReadExact(fd, gateway_vtable - 0x18,
                                          &gateway_adjustment,
                                          sizeof(gateway_adjustment))) {
                                const auto gateway_base =
                                    static_cast<std::uintptr_t>(
                                        static_cast<std::intptr_t>(
                                            final_target_this) +
                                        gateway_adjustment);
                                std::printf(
                                    "    GAMEPLAY_GATEWAY vtable=0x%" PRIxPTR
                                    " adjustment=%" PRId64
                                    " base=0x%" PRIxPTR "\n",
                                    gateway_vtable, gateway_adjustment,
                                    gateway_base);
                                DescribeActionList(fd, maps, "action_07", 
                                                   gateway_base + 0x08, base);
                                DescribeActionList(
                                    fd, maps, "action_31_or_21_by_mode",
                                    gateway_base + 0x30, base);
                                DescribeActionList(fd, maps, "action_23",
                                                   gateway_base + 0x58, base);
                                DescribeActionList(
                                    fd, maps, "actions_18_through_1c_scalar",
                                    gateway_base + 0x120, base);
                                DescribeActionList(fd, maps, "action_25",
                                                   gateway_base + 0x148, base);
                                DescribeActionList(fd, maps, "action_36_bool",
                                                   gateway_base + 0x170, base);
                            }
                        }

                        if (final_target_fn == base + 0x5BF5624 &&
                            FindMapping(maps, final_target_this,
                                        sizeof(std::uintptr_t))) {
                            std::uintptr_t nested_vtable = 0;
                            ReadExact(fd, final_target_this, &nested_vtable,
                                      sizeof(nested_vtable));
                            std::int64_t nested_adjustment = 0;
                            if (nested_vtable >= 0x48 &&
                                FindMapping(maps, nested_vtable - 0x48,
                                            sizeof(nested_adjustment)) &&
                                ReadExact(fd, nested_vtable - 0x48,
                                          &nested_adjustment,
                                          sizeof(nested_adjustment))) {
                                const auto nested_container =
                                    static_cast<std::uintptr_t>(
                                        static_cast<std::intptr_t>(
                                            final_target_this) +
                                        nested_adjustment + 8);
                                std::uintptr_t nested_head = 0;
                                std::uint64_t nested_count = 0;
                                if (FindMapping(maps, nested_container, 0x20) &&
                                    ReadExact(fd, nested_container,
                                              &nested_head,
                                              sizeof(nested_head)) &&
                                    ReadExact(fd, nested_container + 0x10,
                                              &nested_count,
                                              sizeof(nested_count))) {
                                    std::printf("    NESTED_LIST vtable=0x%" PRIxPTR
                                                " adjustment=%" PRId64
                                                " container=0x%" PRIxPTR
                                                " head=0x%" PRIxPTR
                                                " count=%" PRIu64 "\n",
                                                nested_vtable,
                                                nested_adjustment,
                                                nested_container, nested_head,
                                                nested_count);
                                    std::uintptr_t nested_node = nested_head;
                                    for (std::uint64_t nested_index = 0;
                                         nested_index < nested_count &&
                                         nested_index < 32;
                                         ++nested_index) {
                                        if (nested_node < 0x10 ||
                                            !FindMapping(maps,
                                                         nested_node - 0x10,
                                                         0x100))
                                            break;
                                        const auto nested_object =
                                            nested_node - 0x10;
                                        std::uint8_t nested_bytes[0x100]{};
                                        if (!ReadExact(fd, nested_object,
                                                       nested_bytes,
                                                       sizeof(nested_bytes)))
                                            break;
                                        std::uintptr_t nested_object_vtable = 0;
                                        std::uintptr_t nested_callback = 0;
                                        std::memcpy(&nested_object_vtable,
                                                    nested_bytes,
                                                    sizeof(nested_object_vtable));
                                        if (FindMapping(
                                                maps,
                                                nested_object_vtable + 0x40,
                                                sizeof(nested_callback)))
                                            ReadExact(fd,
                                                      nested_object_vtable +
                                                          0x40,
                                                      &nested_callback,
                                                      sizeof(nested_callback));
                                        std::printf(
                                            "     NESTED_SUB index=%" PRIu64
                                            " node=0x%" PRIxPTR
                                            " object=0x%" PRIxPTR,
                                            nested_index, nested_node,
                                            nested_object);
                                        PrintModulePointer(
                                            "vtable", nested_object_vtable,
                                            base);
                                        PrintModulePointer(
                                            "callback40", nested_callback,
                                            base);
                                        std::printf("\n");
                                        PrintModuleRefs(
                                            "nested_object", nested_bytes,
                                            sizeof(nested_bytes), base);
                                        std::memcpy(&nested_node,
                                                    nested_bytes + 0x10,
                                                    sizeof(nested_node));
                                    }
                                }
                            }
                        }
                        std::memcpy(&final_node, final_bytes + 0x10,
                                    sizeof(final_node));
                    }
                }
            }
        }

        std::uint8_t vtable_bytes[0x80]{};
        if (FindMapping(maps, vtable, sizeof(vtable_bytes)) &&
            ReadExact(fd, vtable, vtable_bytes, sizeof(vtable_bytes))) {
            PrintModuleRefs("vtable", vtable_bytes, sizeof(vtable_bytes), base);
        }

        std::uintptr_t next = 0;
        std::memcpy(&next, object_bytes + 0x10, sizeof(next));
        node = next;
    }
}

void PrintFloatIfReadable(int fd, const std::vector<Mapping>& maps,
                          const char* label, std::uintptr_t address) {
    float value = 0.0F;
    if (FindMapping(maps, address, sizeof(value)) &&
        ReadExact(fd, address, &value, sizeof(value)))
        std::printf(" %s=%.9g", label, static_cast<double>(value));
    else
        std::printf(" %s=<unreadable>", label);
}

// Mode-agnostic helper: describe an object whose first qword is the
// VehicleControlSink vtable (0x7EEA080). Does not depend on the keyboard
// parent/gateway chain.
void DescribeSinkHit(int fd, const std::vector<Mapping>& maps,
                     std::uintptr_t sink, std::uintptr_t base) {
    std::uintptr_t sink_vtable = 0;
    if (!FindMapping(maps, sink, sizeof(sink_vtable)) ||
        !ReadExact(fd, sink, &sink_vtable, sizeof(sink_vtable)))
        return;
    std::int64_t adj_b = 0, adj_a = 0;
    if (sink_vtable < 0x38 ||
        !FindMapping(maps, sink_vtable - 0x30, sizeof(adj_b)) ||
        !ReadExact(fd, sink_vtable - 0x30, &adj_b, sizeof(adj_b)) ||
        !FindMapping(maps, sink_vtable - 0x38, sizeof(adj_a)) ||
        !ReadExact(fd, sink_vtable - 0x38, &adj_a, sizeof(adj_a)))
        return;
    const auto owner_b = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(sink) + adj_b);
    const auto owner_a = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(sink) + adj_a);
    std::printf("     MODE_AGNOSTIC_SINK object=0x%" PRIxPTR
                " owner_a=0x%" PRIxPTR " owner_b=0x%" PRIxPTR,
                sink, owner_a, owner_b);
    PrintFloatIfReadable(fd, maps, "value_B_e78", owner_b + 0xE78);
    PrintFloatIfReadable(fd, maps, "value_A_e7c", owner_a + 0xE7C);
    PrintFloatIfReadable(fd, maps, "value_X_e84", owner_a + 0xE84);
    std::printf("\n");

    std::uintptr_t downstream = 0, downstream_vtable = 0;
    if (!FindMapping(maps, owner_a + 0x30, sizeof(downstream)) ||
        !ReadExact(fd, owner_a + 0x30, &downstream, sizeof(downstream)) ||
        !FindMapping(maps, downstream, sizeof(downstream_vtable)) ||
        !ReadExact(fd, downstream, &downstream_vtable,
                   sizeof(downstream_vtable)))
        return;
    std::uintptr_t vf_2a8 = 0, vf_2b0 = 0;
    if (FindMapping(maps, downstream_vtable + 0x2A8, sizeof(vf_2a8)))
        ReadExact(fd, downstream_vtable + 0x2A8, &vf_2a8, sizeof(vf_2a8));
    if (FindMapping(maps, downstream_vtable + 0x2B0, sizeof(vf_2b0)))
        ReadExact(fd, downstream_vtable + 0x2B0, &vf_2b0, sizeof(vf_2b0));
    std::printf("     MODE_AGNOSTIC_DOWNSTREAM object=0x%" PRIxPTR,
                downstream);
    PrintModulePointer("vtable", downstream_vtable, base);
    PrintModulePointer("vfunc_2a8", vf_2a8, base);
    PrintModulePointer("vfunc_2b0", vf_2b0, base);
    std::printf("\n");

    std::int64_t adj_d2a8 = 0, adj_d2b0 = 0;
    if (downstream_vtable < 0x2E0 ||
        !FindMapping(maps, downstream_vtable - 0x2D8, sizeof(adj_d2a8)) ||
        !ReadExact(fd, downstream_vtable - 0x2D8, &adj_d2a8,
                   sizeof(adj_d2a8)) ||
        !FindMapping(maps, downstream_vtable - 0x2E0, sizeof(adj_d2b0)) ||
        !ReadExact(fd, downstream_vtable - 0x2E0, &adj_d2b0,
                   sizeof(adj_d2b0)))
        return;
    const auto down_owner_a = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(downstream) + adj_d2a8);
    const auto down_owner_b = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(downstream) + adj_d2b0);
    std::uintptr_t inner_a = 0, inner_b = 0;
    std::uintptr_t inner_vtable_a = 0, inner_vtable_b = 0;
    if (FindMapping(maps, down_owner_a + 0xA0, sizeof(inner_a)))
        ReadExact(fd, down_owner_a + 0xA0, &inner_a, sizeof(inner_a));
    if (FindMapping(maps, down_owner_b + 0xA0, sizeof(inner_b)))
        ReadExact(fd, down_owner_b + 0xA0, &inner_b, sizeof(inner_b));
    if (FindMapping(maps, inner_a, sizeof(inner_vtable_a)))
        ReadExact(fd, inner_a, &inner_vtable_a, sizeof(inner_vtable_a));
    if (FindMapping(maps, inner_b, sizeof(inner_vtable_b)))
        ReadExact(fd, inner_b, &inner_vtable_b, sizeof(inner_vtable_b));
    std::printf("     MODE_AGNOSTIC_INNER inner_a=0x%" PRIxPTR
                " inner_b=0x%" PRIxPTR,
                inner_a, inner_b);
    PrintModulePointer("inner_vtable_a", inner_vtable_a, base);
    PrintModulePointer("inner_vtable_b", inner_vtable_b, base);
    std::printf("\n");
}

// Mode-agnostic helper: describe an object whose first qword is the inner
// downstream vtable (0x7EED9E0) and print the final owner C98/C9C floats.
void DescribeInnerHit(int fd, const std::vector<Mapping>& maps,
                      std::uintptr_t inner, std::uintptr_t base) {
    std::uintptr_t inner_vtable = 0;
    if (!FindMapping(maps, inner, sizeof(inner_vtable)) ||
        !ReadExact(fd, inner, &inner_vtable, sizeof(inner_vtable)))
        return;
    std::int64_t adj_2a8 = 0, adj_2b0 = 0;
    if (inner_vtable < 0x2E0 ||
        !FindMapping(maps, inner_vtable - 0x2D8, sizeof(adj_2a8)) ||
        !ReadExact(fd, inner_vtable - 0x2D8, &adj_2a8, sizeof(adj_2a8)) ||
        !FindMapping(maps, inner_vtable - 0x2E0, sizeof(adj_2b0)) ||
        !ReadExact(fd, inner_vtable - 0x2E0, &adj_2b0, sizeof(adj_2b0)))
        return;
    const auto final_a = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(inner) + adj_2a8);
    const auto final_b = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(inner) + adj_2b0);
    g_final_a = final_a;
    g_final_b = final_b;
    std::printf("     MODE_AGNOSTIC_FINAL inner=0x%" PRIxPTR
                " final_a=0x%" PRIxPTR " final_b=0x%" PRIxPTR,
                inner, final_a, final_b);
    PrintModulePointer("inner_vtable", inner_vtable, base);
    PrintFloatIfReadable(fd, maps, "value_C98", final_a + 0xC98);
    PrintFloatIfReadable(fd, maps, "value_C9C", final_b + 0xC9C);
    std::printf("\n");
}

// After the scan has located the mode-agnostic final owners, watch their
// C98/C9C float fields read-only at 5 ms intervals. Prints changes plus a
// heartbeat every 500 ms.
void FinalWatch(int fd, const std::vector<Mapping>& maps) {
    (void)maps;
    if (gWatchMilliseconds <= 0 || g_final_a == 0 || g_final_b == 0) return;
    std::uint32_t c98_bits = 0, c9c_bits = 0;
    std::uint32_t previous_c98 = 0, previous_c9c = 0;
    bool have_previous = false;
    timespec started{};
    clock_gettime(CLOCK_MONOTONIC, &started);
    std::printf("       FINAL_WATCH_BEGIN duration_ms=%d interval_ms=5 "
                "final_a=0x%" PRIxPTR " final_b=0x%" PRIxPTR "\n",
                gWatchMilliseconds, g_final_a, g_final_b);
    std::fflush(stdout);
    std::int64_t last_heartbeat = -1;
    for (;;) {
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        const auto elapsed_ms =
            static_cast<std::int64_t>(now.tv_sec - started.tv_sec) * 1000 +
            (now.tv_nsec - started.tv_nsec) / 1000000;
        if (elapsed_ms > gWatchMilliseconds) break;
        if (!ReadExact(fd, g_final_a + 0xC98, &c98_bits, sizeof(c98_bits)) ||
            !ReadExact(fd, g_final_b + 0xC9C, &c9c_bits, sizeof(c9c_bits))) {
            std::printf("       FINAL_WATCH_READ_FAILED t_ms=%" PRId64 "\n",
                        elapsed_ms);
            std::fflush(stdout);
            break;
        }
        const std::int64_t heartbeat = elapsed_ms / 500;
        if (!have_previous || c98_bits != previous_c98 ||
            c9c_bits != previous_c9c || heartbeat != last_heartbeat) {
            float c98 = 0.0F, c9c = 0.0F;
            std::memcpy(&c98, &c98_bits, sizeof(c98));
            std::memcpy(&c9c, &c9c_bits, sizeof(c9c));
            std::printf("       FINAL_WATCH t_ms=%" PRId64
                        " C98=%.9g C9C=%.9g\n",
                        elapsed_ms, static_cast<double>(c98),
                        static_cast<double>(c9c));
            std::fflush(stdout);
            previous_c98 = c98_bits;
            previous_c9c = c9c_bits;
            last_heartbeat = heartbeat;
            have_previous = true;
        }
        usleep(5000);
    }
    std::printf("       FINAL_WATCH_END\n");
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX [WATCH_MILLISECONDS]\n",
                     argv[0]);
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(std::strtol(argv[1], nullptr, 10));
    const auto base = static_cast<std::uintptr_t>(
        std::strtoull(argv[2], nullptr, 16));
    if (argc == 4) {
        gWatchMilliseconds = static_cast<int>(
            std::clamp(std::strtol(argv[3], nullptr, 10), 0L, 60000L));
    }
    if (pid <= 0 || !base) return 2;
    const auto maps = ReadMaps(pid);
    if (maps.empty()) return 3;

    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid);
    const int fd = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        std::fprintf(stderr, "open %s failed: %s\n", mem_path,
                     std::strerror(errno));
        return 4;
    }

    const std::uintptr_t target = base + kParentVtableRva;
    const std::uintptr_t sink_target = base + 0x7EEA080;
    const std::uintptr_t inner_target = base + 0x7EED9E0;
    constexpr std::size_t kChunk = 1u << 20;
    std::vector<std::uint8_t> buffer(kChunk);
    std::size_t hits = 0;
    std::size_t sink_hits = 0, inner_hits = 0;
    std::vector<std::uintptr_t> sink_seen, inner_seen;
    std::uint64_t scanned_bytes = 0;
    for (const auto& map : maps) {
        if (map.perms[0] != 'r' || map.perms[1] != 'w') continue;
        if (map.path == "[vvar]" || map.path == "[vdso]") continue;
        for (std::uintptr_t cursor = map.begin; cursor < map.end;) {
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uintptr_t>(kChunk, map.end - cursor));
            const ssize_t got_raw = pread64(fd, buffer.data(), want,
                                            static_cast<off64_t>(cursor));
            if (got_raw <= 0) {
                cursor += want;
                continue;
            }
            const auto got = static_cast<std::size_t>(got_raw);
            scanned_bytes += got;
            for (std::size_t offset = 0;
                 offset + sizeof(std::uintptr_t) <= got;
                 offset += alignof(std::uintptr_t)) {
                std::uintptr_t value = 0;
                std::memcpy(&value, buffer.data() + offset, sizeof(value));
                if (value == target) {
                    DescribeParent(fd, maps, cursor + offset, base);
                    ++hits;
                } else if (value == sink_target) {
                    const auto object = cursor + offset;
                    if (std::find(sink_seen.begin(), sink_seen.end(), object) ==
                        sink_seen.end()) {
                        if (sink_seen.size() < 8) {
                            DescribeSinkHit(fd, maps, object, base);
                            ++sink_hits;
                        }
                        sink_seen.push_back(object);
                    }
                } else if (value == inner_target) {
                    const auto object = cursor + offset;
                    if (std::find(inner_seen.begin(), inner_seen.end(),
                                  object) == inner_seen.end()) {
                        if (inner_seen.size() < 8) {
                            DescribeInnerHit(fd, maps, object, base);
                            ++inner_hits;
                        }
                        inner_seen.push_back(object);
                    }
                }
            }
            cursor += got;
        }
    }
    FinalWatch(fd, maps);
    close(fd);
    std::printf("SUMMARY pid=%d base=0x%" PRIxPTR
                " target_vtable=0x%" PRIxPTR
                " scanned_bytes=%" PRIu64 " parents=%zu"
                " sink_hits=%zu inner_hits=%zu\n",
                pid, base, target, scanned_bytes, hits, sink_hits, inner_hits);
    return hits == 0 ? 5 : 0;
}
