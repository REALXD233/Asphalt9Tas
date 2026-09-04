#pragma once

// Included inside the lifecycle controller's private namespace after the FC-2
// process helpers.  This is the read-only identity subset previously proved by
// the FC3 action-affinity resolver.  It deliberately resolves the scheduler
// owner independently from the car-physics callback object.  The recovered
// complete-object address is then expected to equal that callback object; the
// independent walk is identity proof, not proof of a second allocation.

constexpr std::uintptr_t kNalActionVectorOffset = 0x1360;
constexpr std::uintptr_t kNalDirectModeOffset = 0x1378;
constexpr std::uintptr_t kNalActionFallbackVtableRva = 0x7EEA080;
constexpr std::uintptr_t kNalActionFallbackVfunc90Rva = 0x367B548;
constexpr std::uintptr_t kNalActionOwnerAdjustMetadataDelta = 0xA0;
constexpr std::uintptr_t kNalActionDispatchAdjustMetadataDelta = 0x230;
constexpr std::uintptr_t kNalActionDispatchVtableRva = 0x7EEFE68;
constexpr std::uintptr_t kNalActionDispatchVfunc158Rva = 0x36A9CAC;
constexpr std::uintptr_t kNalNitroServiceOffset = 0xCB8;
constexpr std::uintptr_t kNalNitroServiceVtableRva = 0x7EE8A90;
constexpr std::uintptr_t kNalNitroActivateSlot = 0x108;
constexpr std::uintptr_t kNalNitroActivateRva = 0x3674E50;
constexpr std::uintptr_t kNalNitroStateOffset = 8;
constexpr std::size_t kNalMaximumQueueCount = 4096;

struct NalActionQueueIdentity {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    std::uintptr_t capacity{};
    std::uint64_t count{};
    std::uint8_t direct_mode{0xff};
};

bool NalCheckedAdd(std::uintptr_t base, std::uintptr_t offset,
                   std::uintptr_t* output) {
    if (!output || base > UINTPTR_MAX - offset) return false;
    *output = base + offset;
    return true;
}

bool NalReadableWritable(const std::vector<Mapping>& maps,
                         std::uintptr_t address, std::size_t size) {
    const Mapping* mapping = FindMapping(maps, address, size);
    return mapping && mapping->perms[0] == 'r' && mapping->perms[1] == 'w';
}

bool ReadNalActionQueue(int mem, std::uintptr_t owner,
                        NalActionQueueIdentity* output) {
    if (!output || owner == 0 || (owner & 7u) != 0) return false;
    NalActionQueueIdentity queue{};
    if (!ReadExact(mem, owner + kNalActionVectorOffset, &queue.begin,
                   sizeof(queue.begin)) ||
        !ReadExact(mem, owner + kNalActionVectorOffset + 8u, &queue.end,
                   sizeof(queue.end)) ||
        !ReadExact(mem, owner + kNalActionVectorOffset + 16u, &queue.capacity,
                   sizeof(queue.capacity)) ||
        !ReadExact(mem, owner + kNalDirectModeOffset, &queue.direct_mode,
                   sizeof(queue.direct_mode)) ||
        queue.direct_mode > 1 || queue.begin > queue.end ||
        queue.end > queue.capacity || ((queue.end - queue.begin) & 7u) != 0 ||
        ((queue.capacity - queue.begin) & 7u) != 0)
        return false;
    queue.count = (queue.end - queue.begin) / 8u;
    if (queue.count > kNalMaximumQueueCount) return false;
    *output = queue;
    return true;
}

bool ValidateNalActionOwner(int mem, const std::vector<Mapping>& maps,
                            std::uintptr_t base, std::uintptr_t owner,
                            NalActionQueueIdentity* queue_out) {
    NalActionQueueIdentity queue{};
    std::uintptr_t command_interface = 0;
    std::uintptr_t interface_head = 0;
    std::int64_t interface_adjustment = 0;
    if (!queue_out || owner == 0 || (owner & 7u) != 0 ||
        !NalReadableWritable(maps, owner + 0x30, sizeof(command_interface)) ||
        !ReadExact(mem, owner + 0x30, &command_interface,
                   sizeof(command_interface)) ||
        !ReadNalActionQueue(mem, owner, &queue) ||
        !FindMapping(maps, command_interface, sizeof(interface_head)) ||
        !ReadExact(mem, command_interface, &interface_head,
                   sizeof(interface_head)) ||
        interface_head < kNalActionDispatchAdjustMetadataDelta ||
        !FindMapping(maps,
                     interface_head - kNalActionDispatchAdjustMetadataDelta,
                     sizeof(interface_adjustment)) ||
        !ReadExact(mem,
                   interface_head - kNalActionDispatchAdjustMetadataDelta,
                   &interface_adjustment, sizeof(interface_adjustment)))
        return false;
    const auto signed_interface = static_cast<std::intptr_t>(command_interface);
    if ((interface_adjustment > 0 &&
         signed_interface > INTPTR_MAX - interface_adjustment) ||
        (interface_adjustment < 0 &&
         signed_interface < INTPTR_MIN - interface_adjustment))
        return false;
    const auto signed_dispatch = signed_interface + interface_adjustment;
    if (signed_dispatch <= 0) return false;
    const auto dispatch_this = static_cast<std::uintptr_t>(signed_dispatch);
    std::uintptr_t dispatch_vtable = 0, dispatch_vfunc = 0;
    if (!FindMapping(maps, dispatch_this, sizeof(dispatch_vtable)) ||
        !ReadExact(mem, dispatch_this, &dispatch_vtable,
                   sizeof(dispatch_vtable)) ||
        dispatch_vtable != base + kNalActionDispatchVtableRva ||
        !FindMapping(maps, dispatch_vtable + 0x158, sizeof(dispatch_vfunc)) ||
        !ReadExact(mem, dispatch_vtable + 0x158, &dispatch_vfunc,
                   sizeof(dispatch_vfunc)) ||
        dispatch_vfunc != base + kNalActionDispatchVfunc158Rva)
        return false;
    *queue_out = queue;
    return true;
}

bool ResolveUniqueNalActionOwner(int mem, const std::vector<Mapping>& maps,
                                 std::uintptr_t base,
                                 std::uintptr_t* owner_out,
                                 NalActionQueueIdentity* queue_out) {
    if (!owner_out || !queue_out) return false;
    const std::uintptr_t fallback_vtable =
        base + kNalActionFallbackVtableRva;
    std::uintptr_t fallback_vfunc = 0;
    std::int64_t owner_adjustment = 0;
    if (!FindMapping(maps, fallback_vtable + 0x90,
                     sizeof(fallback_vfunc)) ||
        !ReadExact(mem, fallback_vtable + 0x90, &fallback_vfunc,
                   sizeof(fallback_vfunc)) ||
        fallback_vfunc != base + kNalActionFallbackVfunc90Rva ||
        fallback_vtable < kNalActionOwnerAdjustMetadataDelta ||
        !FindMapping(maps,
                     fallback_vtable - kNalActionOwnerAdjustMetadataDelta,
                     sizeof(owner_adjustment)) ||
        !ReadExact(mem,
                   fallback_vtable - kNalActionOwnerAdjustMetadataDelta,
                   &owner_adjustment, sizeof(owner_adjustment)))
        return false;

    std::vector<std::uintptr_t> candidates;
    std::vector<std::uint8_t> buffer(1u << 20);
    for (const auto& mapping : maps) {
        if (mapping.perms[0] != 'r' || mapping.perms[1] != 'w' ||
            mapping.path == "[vvar]" || mapping.path == "[vdso]")
            continue;
        for (std::uintptr_t cursor = mapping.begin; cursor < mapping.end;) {
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uintptr_t>(buffer.size(), mapping.end - cursor));
            const ssize_t got =
                pread(mem, buffer.data(), want, static_cast<off_t>(cursor));
            if (got <= 0) {
                cursor += want;
                continue;
            }
            for (std::size_t offset = 0;
                 offset + sizeof(std::uintptr_t) <=
                     static_cast<std::size_t>(got);
                 offset += alignof(std::uintptr_t)) {
                std::uintptr_t value = 0;
                std::memcpy(&value, buffer.data() + offset, sizeof(value));
                if (value != fallback_vtable) continue;
                const auto signed_fallback = static_cast<std::intptr_t>(
                    cursor + static_cast<std::uintptr_t>(offset));
                if ((owner_adjustment > 0 &&
                     signed_fallback > INTPTR_MAX - owner_adjustment) ||
                    (owner_adjustment < 0 &&
                     signed_fallback < INTPTR_MIN - owner_adjustment))
                    continue;
                const auto signed_owner = signed_fallback + owner_adjustment;
                if (signed_owner <= 0) continue;
                NalActionQueueIdentity queue{};
                const auto owner = static_cast<std::uintptr_t>(signed_owner);
                if (ValidateNalActionOwner(mem, maps, base, owner, &queue))
                    candidates.push_back(owner);
            }
            cursor += static_cast<std::uintptr_t>(got);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
    if (candidates.size() != 1) return false;
    NalActionQueueIdentity queue{};
    if (!ValidateNalActionOwner(mem, maps, base, candidates.front(), &queue))
        return false;
    *owner_out = candidates.front();
    *queue_out = queue;
    return true;
}

bool ResolveNalNitroState(int mem, const std::vector<Mapping>& maps,
                          std::uintptr_t base,
                          std::uintptr_t physics_owner,
                          std::uintptr_t* state_out) {
    if (!state_out) return false;
    std::uintptr_t service = 0, vtable = 0, activate = 0;
    if (!ReadExact(mem, physics_owner + kNalNitroServiceOffset, &service,
                   sizeof(service)) ||
        service == 0 || (service & 7u) != 0 ||
        !NalReadableWritable(maps, service, sizeof(vtable)) ||
        !ReadExact(mem, service, &vtable, sizeof(vtable)) ||
        vtable != base + kNalNitroServiceVtableRva ||
        !FindMapping(maps, vtable + kNalNitroActivateSlot,
                     sizeof(activate)) ||
        !ReadExact(mem, vtable + kNalNitroActivateSlot, &activate,
                   sizeof(activate)) ||
        activate != base + kNalNitroActivateRva)
        return false;
    const std::uintptr_t state = service + kNalNitroStateOffset;
    if (!NalReadableWritable(maps, state + 0x188, 1) ||
        !NalReadableWritable(maps, state + 0x18C, 4))
        return false;
    *state_out = state;
    return true;
}
