#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

#if !defined(A9TAS_G4_NATIVE_ARM64_CONTROLLER)

#ifndef A9TAS_HABI1_BOOTSTRAP_PATH
#error "A9TAS_HABI1_BOOTSTRAP_PATH must be fixed by the build"
#endif
#ifndef A9TAS_HABI1_BOOTSTRAP_SHA256
#error "A9TAS_HABI1_BOOTSTRAP_SHA256 must be fixed by the build"
#endif
#ifndef A9TAS_HABI1_BOOTSTRAP_BUILD_ID
#error "A9TAS_HABI1_BOOTSTRAP_BUILD_ID must be fixed by the build"
#endif
#ifndef A9TAS_HABI1_PAYLOAD_PATH
#error "A9TAS_HABI1_PAYLOAD_PATH must be fixed by the build"
#endif
#ifndef A9TAS_HABI1_PAYLOAD_SHA256
#error "A9TAS_HABI1_PAYLOAD_SHA256 must be fixed by the build"
#endif
#ifndef A9TAS_HABI1_PAYLOAD_BUILD_ID
#error "A9TAS_HABI1_PAYLOAD_BUILD_ID must be fixed by the build"
#endif
#ifndef A9TAS_HABI1_PAYLOAD_SOURCE_SHA256
#error "A9TAS_HABI1_PAYLOAD_SOURCE_SHA256 must be fixed by the build"
#endif
#ifndef A9TAS_HABI1_PAYLOAD_HEADER_SHA256
#error "A9TAS_HABI1_PAYLOAD_HEADER_SHA256 must be fixed by the build"
#endif

#define A9TAS_HABI1_C_STRING_INNER(value) #value
#define A9TAS_HABI1_C_STRING(value) A9TAS_HABI1_C_STRING_INNER(value)

constexpr const char* kBootstrapPath =
    A9TAS_HABI1_C_STRING(A9TAS_HABI1_BOOTSTRAP_PATH);
constexpr const char* kBootstrapSha256 =
    A9TAS_HABI1_C_STRING(A9TAS_HABI1_BOOTSTRAP_SHA256);
constexpr const char* kBootstrapBuildId =
    A9TAS_HABI1_C_STRING(A9TAS_HABI1_BOOTSTRAP_BUILD_ID);
constexpr const char* kPayloadPath =
    A9TAS_HABI1_C_STRING(A9TAS_HABI1_PAYLOAD_PATH);
constexpr const char* kPayloadSha256 =
    A9TAS_HABI1_C_STRING(A9TAS_HABI1_PAYLOAD_SHA256);
constexpr const char* kPayloadBuildId =
    A9TAS_HABI1_C_STRING(A9TAS_HABI1_PAYLOAD_BUILD_ID);
constexpr const char* kPayloadSourceSha256 =
    A9TAS_HABI1_C_STRING(A9TAS_HABI1_PAYLOAD_SOURCE_SHA256);
constexpr const char* kPayloadHeaderSha256 =
    A9TAS_HABI1_C_STRING(A9TAS_HABI1_PAYLOAD_HEADER_SHA256);
constexpr const char* kReceiptPath =
    "/data/user/0/com.aligames.kuang.kybc.aligames/cache/"
    "a9tas-hook-abi-selftest-habi1.status";

#else

// The native G4 controller does not execute the embedded HABI1 artifact
// resolver. Keep its dead compatibility surface syntactically complete
// without embedding a fake NativeBridge/bootstrap identity.
constexpr const char* kBootstrapPath = "";
constexpr const char* kBootstrapSha256 = "";
constexpr const char* kBootstrapBuildId = "";
constexpr const char* kPayloadPath = "";
constexpr const char* kPayloadSha256 = "";
constexpr const char* kPayloadBuildId = "";
constexpr const char* kPayloadSourceSha256 = "";

#endif  // !defined(A9TAS_G4_NATIVE_ARM64_CONTROLLER)
constexpr const char* kLocatorSymbol =
    "a9tas_bootstrap_hook_abi_selftest_habi1_locator";
constexpr const char* kStageSymbol = "_ZN12_GLOBAL__N_17g_stageE";
constexpr const char* kProbeStatusSymbol =
    "_ZN12_GLOBAL__N_126g_same_thread_probe_statusE";
constexpr const char* kTrampolineSymbol =
    "_ZN12_GLOBAL__N_130g_same_thread_probe_trampolineE";
constexpr const char* kTrapSymbol =
    "a9tas_bootstrap_hook_abi_selftest_habi1_return_trap";
constexpr const char* kCalibrateSymbol =
    "a9tas_bootstrap_hook_abi_selftest_habi1_calibrate_tid";
constexpr std::uint64_t kLocatorMagic = 0x48414249314c4f43ULL;
constexpr std::uint32_t kRunPassTag = 0x48414201;
constexpr auto kCallTimeout = std::chrono::seconds(5);

struct Sha256 {
    std::uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    std::uint64_t total{};
    std::uint8_t buffer[64]{};
    std::size_t buffered{};

    static std::uint32_t Rotate(std::uint32_t value, unsigned bits) {
        return (value >> bits) | (value << (32 - bits));
    }

    void Transform(const std::uint8_t block[64]) {
        static constexpr std::uint32_t constants[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
        };
        std::uint32_t words[64]{};
        for (std::size_t i = 0; i < 16; ++i) {
            words[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                       (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                       (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                       static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = Rotate(words[i - 15], 7) ^
                                     Rotate(words[i - 15], 18) ^
                                     (words[i - 15] >> 3);
            const std::uint32_t s1 = Rotate(words[i - 2], 17) ^
                                     Rotate(words[i - 2], 19) ^
                                     (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        std::uint32_t a=state[0],b=state[1],c=state[2],d=state[3];
        std::uint32_t e=state[4],f=state[5],g=state[6],h=state[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = Rotate(e, 6) ^ Rotate(e, 11) ^ Rotate(e, 25);
            const std::uint32_t choice = (e & f) ^ (~e & g);
            const std::uint32_t t1 = h + s1 + choice + constants[i] + words[i];
            const std::uint32_t s0 = Rotate(a, 2) ^ Rotate(a, 13) ^ Rotate(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + majority;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
        state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
    }

    void Update(const void* raw, std::size_t size) {
        const auto* data = static_cast<const std::uint8_t*>(raw);
        total += size;
        while (size != 0) {
            const std::size_t take = std::min(size, sizeof(buffer) - buffered);
            std::memcpy(buffer + buffered, data, take);
            buffered += take; data += take; size -= take;
            if (buffered == sizeof(buffer)) { Transform(buffer); buffered = 0; }
        }
    }

    std::array<std::uint8_t, 32> Finish() {
        const std::uint64_t bits = total * 8;
        const std::uint8_t marker = 0x80;
        Update(&marker, 1);
        const std::uint8_t zero = 0;
        while (buffered != 56) Update(&zero, 1);
        std::uint8_t length[8]{};
        for (int i = 0; i < 8; ++i)
            length[7 - i] = static_cast<std::uint8_t>(bits >> (i * 8));
        Update(length, sizeof(length));
        std::array<std::uint8_t, 32> digest{};
        for (std::size_t i = 0; i < 8; ++i) {
            digest[i*4] = static_cast<std::uint8_t>(state[i] >> 24);
            digest[i*4+1] = static_cast<std::uint8_t>(state[i] >> 16);
            digest[i*4+2] = static_cast<std::uint8_t>(state[i] >> 8);
            digest[i*4+3] = static_cast<std::uint8_t>(state[i]);
        }
        return digest;
    }
};

std::string Hex(const std::array<std::uint8_t, 32>& digest) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string output(64, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        output[i * 2] = kHex[digest[i] >> 4];
        output[i * 2 + 1] = kHex[digest[i] & 0xf];
    }
    return output;
}

std::string Sha256Bytes(const void* data, std::size_t size) {
    Sha256 hash;
    hash.Update(data, size);
    return Hex(hash.Finish());
}

struct FileImage {
    std::vector<std::uint8_t> bytes;
    struct stat status{};
    std::string sha256;
};

bool ReadPinnedRegularFile(const char* path, const char* expected_sha,
                           FileImage* output) {
    if (!path || !expected_sha || !output) return false;
    struct stat before{};
    if (lstat(path, &before) != 0 || !S_ISREG(before.st_mode) ||
        S_ISLNK(before.st_mode) || before.st_size <= 0) return false;
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat opened{};
    bool ok = fstat(fd, &opened) == 0 && S_ISREG(opened.st_mode) &&
              opened.st_dev == before.st_dev && opened.st_ino == before.st_ino &&
              opened.st_size == before.st_size;
    if (ok) {
        output->bytes.resize(static_cast<std::size_t>(opened.st_size));
        std::size_t done = 0;
        while (done < output->bytes.size()) {
            const ssize_t count = read(fd, output->bytes.data() + done,
                                       output->bytes.size() - done);
            if (count <= 0) { ok = false; break; }
            done += static_cast<std::size_t>(count);
        }
    }
    struct stat after{};
    ok = ok && fstat(fd, &after) == 0 && after.st_dev == opened.st_dev &&
         after.st_ino == opened.st_ino && after.st_size == opened.st_size;
    ok = close(fd) == 0 && ok;
    if (!ok) return false;
    output->status = after;
    output->sha256 = Sha256Bytes(output->bytes.data(), output->bytes.size());
    return output->sha256 == expected_sha;
}

class ElfImage {
public:
    bool Parse(const FileImage& file, std::uint16_t machine) {
        data_ = &file.bytes;
        if (data_->size() < sizeof(Elf64_Ehdr)) return false;
        std::memcpy(&header_, data_->data(), sizeof(header_));
        if (std::memcmp(header_.e_ident, ELFMAG, SELFMAG) != 0 ||
            header_.e_ident[EI_CLASS] != ELFCLASS64 ||
            header_.e_ident[EI_DATA] != ELFDATA2LSB ||
            header_.e_type != ET_DYN || header_.e_machine != machine ||
            header_.e_phentsize != sizeof(Elf64_Phdr) ||
            header_.e_shentsize != sizeof(Elf64_Shdr) ||
            header_.e_phnum == 0 || header_.e_shnum == 0) return false;
        if (!Range(header_.e_phoff,
                   static_cast<std::uint64_t>(header_.e_phnum) * sizeof(Elf64_Phdr)) ||
            !Range(header_.e_shoff,
                   static_cast<std::uint64_t>(header_.e_shnum) * sizeof(Elf64_Shdr)))
            return false;
        programs_.resize(header_.e_phnum);
        std::memcpy(programs_.data(), data_->data() + header_.e_phoff,
                    programs_.size() * sizeof(Elf64_Phdr));
        sections_.resize(header_.e_shnum);
        std::memcpy(sections_.data(), data_->data() + header_.e_shoff,
                    sections_.size() * sizeof(Elf64_Shdr));
        for (const Elf64_Phdr& ph : programs_) {
            if (!Range(ph.p_offset, ph.p_filesz)) return false;
            if (ph.p_type == PT_LOAD &&
                (ph.p_flags & (PF_W | PF_X)) == (PF_W | PF_X)) return false;
        }
        return ReadBuildId();
    }

    bool ResolveUnique(const char* wanted, bool dynamic_only, Elf64_Sym* out) const {
        if (!wanted || !out) return false;
        int matches = 0;
        for (const Elf64_Shdr& symbols : sections_) {
            if (symbols.sh_type != SHT_DYNSYM &&
                (dynamic_only || symbols.sh_type != SHT_SYMTAB)) continue;
            if (symbols.sh_link >= sections_.size() ||
                symbols.sh_entsize != sizeof(Elf64_Sym) ||
                symbols.sh_size % sizeof(Elf64_Sym) != 0 ||
                !Range(symbols.sh_offset, symbols.sh_size)) return false;
            const Elf64_Shdr& strings = sections_[symbols.sh_link];
            if (!Range(strings.sh_offset, strings.sh_size)) return false;
            const char* names = reinterpret_cast<const char*>(
                data_->data() + strings.sh_offset);
            for (std::uint64_t offset = 0; offset < symbols.sh_size;
                 offset += sizeof(Elf64_Sym)) {
                Elf64_Sym symbol{};
                std::memcpy(&symbol, data_->data() + symbols.sh_offset + offset,
                            sizeof(symbol));
                if (symbol.st_name >= strings.sh_size) return false;
                const void* end = std::memchr(names + symbol.st_name, '\0',
                                              strings.sh_size - symbol.st_name);
                if (!end) return false;
                if (std::strcmp(names + symbol.st_name, wanted) == 0 &&
                    symbol.st_shndx != SHN_UNDEF) {
                    *out = symbol;
                    ++matches;
                }
            }
        }
        // A symbol may appear once in dynsym and once in symtab.  Callers that
        // request all tables therefore accept two identical records.
        return dynamic_only ? matches == 1 : (matches == 1 || matches == 2);
    }

    bool VirtualBytes(std::uint64_t address, std::size_t size,
                      const std::uint8_t** output) const {
        if (!output || address > UINT64_MAX - size) return false;
        for (const Elf64_Phdr& ph : programs_) {
            if (ph.p_type != PT_LOAD || address < ph.p_vaddr ||
                address + size > ph.p_vaddr + ph.p_filesz) continue;
            const std::uint64_t offset = ph.p_offset + address - ph.p_vaddr;
            if (!Range(offset, size)) return false;
            *output = data_->data() + offset;
            return true;
        }
        return false;
    }

    bool AddressInRelro(std::uint64_t address, std::uint64_t size) const {
        for (const Elf64_Phdr& ph : programs_) {
            if (ph.p_type == PT_GNU_RELRO && address >= ph.p_vaddr &&
                address + size <= ph.p_vaddr + ph.p_memsz) return true;
        }
        return false;
    }

    const std::string& build_id() const { return build_id_; }

private:
    bool Range(std::uint64_t offset, std::uint64_t size) const {
        return offset <= data_->size() && size <= data_->size() - offset;
    }

    bool ReadBuildId() {
        for (const Elf64_Phdr& ph : programs_) {
            if (ph.p_type != PT_NOTE) continue;
            std::uint64_t cursor = ph.p_offset;
            const std::uint64_t end = cursor + ph.p_filesz;
            while (cursor + sizeof(Elf64_Nhdr) <= end) {
                Elf64_Nhdr note{};
                std::memcpy(&note, data_->data() + cursor, sizeof(note));
                cursor += sizeof(note);
                const std::uint64_t name_size = (note.n_namesz + 3U) & ~3ULL;
                const std::uint64_t desc_size = (note.n_descsz + 3U) & ~3ULL;
                if (cursor + name_size + desc_size > end) return false;
                const std::uint8_t* name = data_->data() + cursor;
                const std::uint8_t* desc = name + name_size;
                if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 &&
                    std::memcmp(name, "GNU", 4) == 0 &&
                    note.n_descsz > 0 && note.n_descsz <= 32) {
                    std::array<std::uint8_t, 32> padded{};
                    std::memcpy(padded.data(), desc, note.n_descsz);
                    constexpr char hex[] = "0123456789abcdef";
                    build_id_.resize(note.n_descsz * 2);
                    for (std::size_t i = 0; i < note.n_descsz; ++i) {
                        build_id_[i*2] = hex[desc[i] >> 4];
                        build_id_[i*2+1] = hex[desc[i] & 0xf];
                    }
                    return true;
                }
                cursor += name_size + desc_size;
            }
        }
        return false;
    }

    const std::vector<std::uint8_t>* data_{};
    Elf64_Ehdr header_{};
    std::vector<Elf64_Phdr> programs_;
    std::vector<Elf64_Shdr> sections_;
    std::string build_id_;
};

struct Mapping {
    std::uintptr_t start{};
    std::uintptr_t end{};
    std::uintptr_t offset{};
    unsigned dev_major{};
    unsigned dev_minor{};
    std::uint64_t inode{};
    bool readable{};
    bool writable{};
    bool executable{};
    bool private_mapping{};
    std::string path;
};

std::string Trim(std::string value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.erase(value.begin());
    return value;
}

std::vector<Mapping> ReadMaps(pid_t pid) {
    std::ifstream input("/proc/" + std::to_string(pid) + "/maps");
    std::vector<Mapping> output;
    std::string line;
    while (std::getline(input, line)) {
        unsigned long long start=0,end=0,offset=0,inode=0;
        unsigned device_major=0,device_minor=0;
        char perms[5]{};
        char path[2048]{};
        const int fields = std::sscanf(
            line.c_str(), "%llx-%llx %4s %llx %x:%x %llu %2047[^\n]",
            &start,&end,perms,&offset,&device_major,&device_minor,&inode,path);
        if (fields < 7 || start >= end) continue;
        Mapping map{};
        map.start=start; map.end=end; map.offset=offset;
        map.dev_major=device_major; map.dev_minor=device_minor; map.inode=inode;
        map.readable=perms[0]=='r'; map.writable=perms[1]=='w';
        map.executable=perms[2]=='x'; map.private_mapping=perms[3]=='p';
        map.path=fields==8 ? Trim(path) : "";
        output.push_back(std::move(map));
    }
    return output;
}

const Mapping* FindMapping(const std::vector<Mapping>& maps,
                           std::uintptr_t address, std::size_t size=1) {
    if (size == 0 || address > UINTPTR_MAX - size) return nullptr;
    for (const Mapping& map : maps)
        if (address >= map.start && address + size <= map.end) return &map;
    return nullptr;
}

bool MappingMatchesFile(const Mapping& map, const char* path,
                        const struct stat& file) {
    return map.path == path && map.path.find("(deleted)") == std::string::npos &&
           map.private_mapping && map.inode == static_cast<std::uint64_t>(file.st_ino) &&
           map.dev_major == static_cast<unsigned>(major(file.st_dev)) &&
           map.dev_minor == static_cast<unsigned>(minor(file.st_dev));
}

bool UniqueOffsetZeroBase(const std::vector<Mapping>& maps, const char* path,
                          const struct stat& file, std::uintptr_t* base) {
    std::set<std::uintptr_t> candidates;
    for (const Mapping& map : maps)
        if (map.offset == 0 && MappingMatchesFile(map, path, file))
            candidates.insert(map.start);
    if (candidates.size() != 1 || !base) return false;
    *base = *candidates.begin();
    return true;
}

[[maybe_unused]] bool HasPinnedMapping(const std::vector<Mapping>& maps, const char* path,
                      const struct stat& file) {
    return std::any_of(maps.begin(), maps.end(), [&](const Mapping& map) {
        return MappingMatchesFile(map, path, file) && map.readable;
    });
}

bool ReadProcessMemory(pid_t pid, std::uintptr_t address, void* output,
                       std::size_t size) {
    const std::string path = "/proc/" + std::to_string(pid) + "/mem";
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    std::size_t done = 0;
    auto* bytes = static_cast<std::uint8_t*>(output);
    while (done < size) {
        const ssize_t count = pread(fd, bytes + done, size - done,
                                    static_cast<off_t>(address + done));
        if (count <= 0) { close(fd); return false; }
        done += static_cast<std::size_t>(count);
    }
    return close(fd) == 0;
}

template <typename T>
bool ReadProcessValue(pid_t pid, std::uintptr_t address, T* value) {
    return value && ReadProcessMemory(pid, address, value, sizeof(T));
}

std::uint64_t ProcessStartTicks(pid_t pid) {
    std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
    std::string content((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
    const std::size_t close = content.rfind(')');
    if (close == std::string::npos || close + 2 >= content.size()) return 0;
    std::istringstream fields(content.substr(close + 2));
    std::string field;
    for (int index = 3; index <= 22; ++index) {
        if (!(fields >> field)) return 0;
        if (index == 22) {
            char* end = nullptr;
            errno = 0;
            const unsigned long long value = std::strtoull(field.c_str(), &end, 10);
            return errno == 0 && end && *end == '\0' ? value : 0;
        }
    }
    return 0;
}

int TracerPid(pid_t pid) {
    std::ifstream input("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(input, line)) {
        int value = -1;
        if (std::sscanf(line.c_str(), "TracerPid:%d", &value) == 1) return value;
    }
    return -1;
}

pid_t ThreadGroupId(pid_t tid) {
    std::ifstream input("/proc/" + std::to_string(tid) + "/status");
    std::string line;
    while (std::getline(input, line)) {
        int value = 0;
        if (std::sscanf(line.c_str(), "Tgid:%d", &value) == 1) return value;
    }
    return 0;
}

std::string ThreadName(pid_t pid, pid_t tid) {
    std::ifstream input("/proc/" + std::to_string(pid) + "/task/" +
                        std::to_string(tid) + "/comm");
    std::string name;
    std::getline(input, name);
    if (!name.empty() && name.back() == '\r') name.pop_back();
    return name;
}

pid_t UniqueSignalCatcher(pid_t pid) {
    const std::string task_path = "/proc/" + std::to_string(pid) + "/task";
    DIR* directory = opendir(task_path.c_str());
    if (!directory) return 0;
    std::vector<pid_t> matches;
    while (dirent* entry = readdir(directory)) {
        char* end = nullptr;
        const long raw = std::strtol(entry->d_name, &end, 10);
        if (raw <= 0 || end == entry->d_name || *end != '\0') continue;
        const pid_t tid = static_cast<pid_t>(raw);
        if (ThreadName(pid, tid) == "Signal Catcher" &&
            ThreadGroupId(tid) == pid) matches.push_back(tid);
    }
    closedir(directory);
    if (matches.size() != 1) return 0;
    const pid_t tid = matches.front();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return ThreadName(pid, tid) == "Signal Catcher" && ThreadGroupId(tid) == pid
        ? tid : 0;
}

struct alignas(8) Locator {
    std::uint64_t magic;
    std::uint32_t version;
    std::uint32_t size;
    char payload_path[192];
    char payload_sha256[65];
    char payload_build_id[41];
    char payload_source_sha256[65];
    char run_symbol[64];
    char shorty[8];
    std::uintptr_t stage_address;
    std::uintptr_t probe_status_address;
    std::uintptr_t trampoline_address;
    std::uint32_t stage_size;
    std::uint32_t probe_status_size;
    std::uint32_t trampoline_size;
    std::uint32_t reserved;
    std::uintptr_t return_trap_address;
    std::uintptr_t calibrate_tid_address;
};
static_assert(sizeof(Locator) == 512);

bool ExactString(const char* field, std::size_t capacity, const char* expected) {
    const std::size_t length = std::strlen(expected);
    if (length >= capacity || std::memcmp(field, expected, length + 1) != 0)
        return false;
    for (std::size_t i = length + 1; i < capacity; ++i)
        if (field[i] != '\0') return false;
    return true;
}

bool ReadStableLocator(pid_t pid, std::uintptr_t address, Locator* output) {
    Locator first{}, second{};
    if (!ReadProcessValue(pid, address, &first)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!ReadProcessValue(pid, address, &second) ||
        std::memcmp(&first, &second, sizeof(first)) != 0) return false;
    *output = first;
    return true;
}

#if defined(__x86_64__)

bool PeekWord(pid_t tid, std::uintptr_t address, long* output) {
    errno = 0;
    const long value = ptrace(PTRACE_PEEKDATA, tid, address, nullptr);
    if (value == -1 && errno != 0) return false;
    *output = value;
    return true;
}

bool WriteRemote(pid_t tid, std::uintptr_t address, const void* input,
                 std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(input);
    for (std::size_t done = 0; done < size; done += sizeof(long)) {
        const std::size_t chunk = std::min(sizeof(long), size - done);
        long word = 0;
        if (chunk != sizeof(long) && !PeekWord(tid, address + done, &word))
            return false;
        std::memcpy(&word, bytes + done, chunk);
        if (ptrace(PTRACE_POKEDATA, tid, address + done,
                   reinterpret_cast<void*>(word)) == -1) return false;
    }
    return true;
}

bool ReadRemote(pid_t tid, std::uintptr_t address, void* output,
                std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(output);
    for (std::size_t done = 0; done < size; done += sizeof(long)) {
        long word = 0;
        if (!PeekWord(tid, address + done, &word)) return false;
        const std::size_t chunk = std::min(sizeof(long), size - done);
        std::memcpy(bytes + done, &word, chunk);
    }
    return true;
}

#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif

bool GetRegs(pid_t tid, user_regs_struct* regs) {
    iovec vector{regs, sizeof(*regs)};
    return regs && ptrace(PTRACE_GETREGSET, tid,
                          reinterpret_cast<void*>(NT_PRSTATUS), &vector) != -1 &&
           vector.iov_len == sizeof(*regs);
}

bool SetRegsExact(pid_t tid, const user_regs_struct& regs) {
    user_regs_struct copy = regs;
    iovec vector{&copy, sizeof(copy)};
    user_regs_struct observed{};
    if (ptrace(PTRACE_SETREGSET, tid, reinterpret_cast<void*>(NT_PRSTATUS),
               &vector) != -1 && GetRegs(tid, &observed) &&
        std::memcmp(&observed, &regs, sizeof(regs)) == 0) return true;
    const auto* words = reinterpret_cast<const unsigned long*>(&regs);
    constexpr std::size_t count = sizeof(regs) / sizeof(unsigned long);
    for (int attempt = 0; attempt < 3; ++attempt) {
        bool written = true;
        for (std::size_t index = 0; index < count; ++index) {
            if (ptrace(PTRACE_POKEUSER, tid, index * sizeof(unsigned long),
                       reinterpret_cast<void*>(words[index])) == -1) {
                written = false;
                break;
            }
        }
        if (!written) return false;
        if (GetRegs(tid, &observed) &&
            std::memcmp(&observed, &regs, sizeof(regs)) == 0) return true;
    }
    return false;
}

bool SelectCallStack(const std::vector<Mapping>& maps, std::uintptr_t rsp,
                     std::uintptr_t* output) {
    const Mapping* stack = FindMapping(maps, rsp - 1);
    if (!stack || !stack->readable || !stack->writable ||
        !stack->private_mapping) return false;
    constexpr std::uintptr_t reserve = 0x8000;
    constexpr std::uintptr_t margin = 0x1000;
    if (rsp < stack->start + reserve + margin) return false;
    const std::uintptr_t candidate = ((rsp - reserve) & ~std::uintptr_t{0xf}) - 8;
    if (candidate < stack->start + margin || candidate + 8 > stack->end)
        return false;
    *output = candidate;
    return true;
}

struct CallReport {
    bool stopped{};
    bool rollback_attempted{};
    bool rollback_succeeded{};
    bool detach_safe{};
    int stop_signal{};
    int wait_status{};
    int error{};
    std::uint64_t result{};
    std::uint64_t rip{};
};

bool WaitBounded(pid_t tid, int* status, bool* stopped) {
    const auto deadline = std::chrono::steady_clock::now() + kCallTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        int observed = 0;
        const pid_t waited = waitpid(tid, &observed, WNOHANG | __WALL);
        if (waited == tid) {
            *status = observed;
            *stopped = WIFSTOPPED(observed);
            return true;
        }
        if (waited == -1 && errno != EINTR) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    (void)kill(tid, SIGSTOP);
    const auto stop_deadline = std::chrono::steady_clock::now() +
                               std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < stop_deadline) {
        int observed = 0;
        const pid_t waited = waitpid(tid, &observed, WNOHANG | __WALL);
        if (waited == tid) {
            *status = observed;
            *stopped = WIFSTOPPED(observed);
            errno = ETIMEDOUT;
            return false;
        }
        if (waited == -1 && errno != EINTR) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    errno = ETIMEDOUT;
    return false;
}

bool RemoteCallOnce(pid_t tid, const std::vector<Mapping>& maps,
                    const user_regs_struct& original, long bias,
                    std::uintptr_t function, std::uintptr_t trap,
                    const std::uint64_t args[6], CallReport* report) {
    if (!report) return false;
    *report = {};
    const Mapping* function_map = FindMapping(maps, function);
    const Mapping* trap_map = FindMapping(maps, trap, 2);
    if (!function_map || !function_map->readable || !function_map->executable ||
        function_map->writable || !trap_map || !trap_map->readable ||
        !trap_map->executable || trap_map->writable) return false;
    long trap_word = 0;
    if (!PeekWord(tid, trap, &trap_word) || (trap_word & 0xff) != 0xcc)
        return false;
    std::uintptr_t call_stack = 0;
    if (!SelectCallStack(maps, original.rsp, &call_stack)) return false;
    std::uint64_t saved_stack = 0;
    if (!ReadRemote(tid, call_stack, &saved_stack, sizeof(saved_stack)))
        return false;
    const std::uint64_t return_address = trap;
    if (!WriteRemote(tid, call_stack, &return_address, sizeof(return_address)))
        return false;
    std::uint64_t stack_readback = 0;
    if (!ReadRemote(tid, call_stack, &stack_readback, sizeof(stack_readback)) ||
        stack_readback != return_address) {
        (void)WriteRemote(tid, call_stack, &saved_stack, sizeof(saved_stack));
        return false;
    }
    user_regs_struct call = original;
    call.rip = function + static_cast<std::uint64_t>(bias);
    call.rsp = call_stack;
    call.rdi=args[0]; call.rsi=args[1]; call.rdx=args[2];
    call.rcx=args[3]; call.r8=args[4]; call.r9=args[5];
    if (!SetRegsExact(tid, call)) {
        (void)WriteRemote(tid, call_stack, &saved_stack, sizeof(saved_stack));
        return false;
    }
    bool tracee_stopped = true;
    bool ok = ptrace(PTRACE_CONT, tid, nullptr, nullptr) != -1;
    if (ok) tracee_stopped = false;
    int status = 0;
    if (ok) ok = WaitBounded(tid, &status, &tracee_stopped);
    report->stopped = tracee_stopped;
    report->wait_status = status;
    if (tracee_stopped && WIFSTOPPED(status)) report->stop_signal = WSTOPSIG(status);
    user_regs_struct returned{};
    siginfo_t info{};
    const bool returned_regs = tracee_stopped && GetRegs(tid, &returned);
    const bool siginfo_ok = tracee_stopped &&
        ptrace(PTRACE_GETSIGINFO, tid, nullptr, &info) != -1;
    if (returned_regs) report->rip = returned.rip;
    const bool expected_trap = ok && returned_regs && siginfo_ok &&
        report->stop_signal == SIGTRAP && info.si_signo == SIGTRAP &&
        (info.si_code == SI_KERNEL || info.si_code == TRAP_BRKPT) &&
        (returned.rip == trap || returned.rip == trap + 1);
    if (expected_trap) report->result = returned.rax;

    if (!tracee_stopped) {
        report->error = errno;
        return false;
    }
    report->rollback_attempted = true;
    user_regs_struct rollback = original;
    rollback.rip = original.rip + static_cast<std::uint64_t>(bias);
    bool restored = SetRegsExact(tid, rollback);
    restored = WriteRemote(tid, call_stack, &saved_stack, sizeof(saved_stack)) && restored;
    std::uint64_t restored_stack = 0;
    restored = ReadRemote(tid, call_stack, &restored_stack,
                          sizeof(restored_stack)) &&
               restored_stack == saved_stack && restored;
    report->rollback_succeeded = restored;
    report->detach_safe = restored;
    report->error = expected_trap && restored ? 0 : errno;
    return expected_trap && restored;
}

#endif  // defined(__x86_64__)

bool KillUncertainProcess(pid_t pid) {
    if (pid <= 0) return false;
    (void)kill(pid, SIGKILL);
    for (int i = 0; i < 100; ++i) {
        std::ifstream state_file("/proc/" + std::to_string(pid) + "/stat");
        std::string state_text((std::istreambuf_iterator<char>(state_file)),
                               std::istreambuf_iterator<char>());
        const std::size_t close = state_text.rfind(')');
        if (state_text.empty() || close == std::string::npos ||
            close + 2 >= state_text.size() || state_text[close + 2] == 'Z' ||
            state_text[close + 2] == 'X') return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return ProcessStartTicks(pid) == 0;
}

[[maybe_unused]] bool ParseUnsigned(const std::string& text,
                                  std::uint64_t* output) {
    if (!output || text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0') return false;
    *output = value;
    return true;
}

#if !defined(A9TAS_G4_NATIVE_ARM64_CONTROLLER)
[[maybe_unused]] bool ParseReceipt(const FileImage& receipt, pid_t pid, pid_t tid,
                  std::uint64_t nonce) {
    const std::string text(reinterpret_cast<const char*>(receipt.bytes.data()),
                           receipt.bytes.size());
    if (text.empty() || text.back() != '\n' || text.find('\0') != std::string::npos)
        return false;
    std::map<std::string, std::string> fields;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty()) return false;
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos || equals == 0 ||
            !fields.emplace(line.substr(0, equals), line.substr(equals + 1)).second)
            return false;
    }
    const std::map<std::string, std::string> exact = {
        {"protocol","hook-abi-selftest-habi1"},{"revision","2"},
        {"abi","arm64-v8a"},{"gate","HABI-1"},{"scope","self-only"},
        {"run_mode","explicit-export-only"},{"selftest_constructor_trigger","0"},
        {"threads_created","0"},{"receipt_commit","atomic-rename-file-fsync-dir-fsync"},
        {"run_calls","1"},{"source_sha256",kPayloadSourceSha256},
        {"payload_build_id",kPayloadBuildId},{"expected_payload_path",kPayloadPath},
        {"game_module_names_queried","0"},{"game_addresses_dereferenced","0"},
        {"game_addresses_modified","0"},{"absolute_jump_scratch","x17-ip1"},
        {"target_page_w_xor_x","1"},{"failures","0"},
        {"first_failure_stage","none"},{"first_failure_code","0"},
        {"first_errno","0"},{"trigger_claimed","1"},{"stale_receipt_cleared","1"},
        {"build_identity","1"},{"self_payload_scope","1"},
        {"target_elf_exec_load","1"},{"target_initial_guest_code_view","1"},
        {"target_initial_mapping_private","1"},{"target_page_isolated","1"},
        {"exact_16b_prologue","1"},{"trampoline_prologue_pic","1"},
        {"trampoline_rw_no_exec_readback","1"},{"trampoline_bytes_readback","1"},
        {"trampoline_rx_readback","1"},{"target_rw_no_exec_readback","1"},
        {"install_bytes_readback","1"},{"install_protection_readback","1"},
        {"patch_published","1"},{"install_succeeded","1"},
        {"baseline_original_executed","1"},{"passthrough","1"},{"freeze","1"},
        {"single_step","1"},{"stress_100000","1"},{"return_abi","1"},
        {"filter_stack_aligned","1"},{"target_stack_exact","1"},
        {"required_gpr_preserved","1"},{"simd_q0_q31_preserved","1"},
        {"nzcv_preserved","1"},{"fpcr_preserved","1"},{"fpsr_preserved","1"},
        {"prologue_once_per_call","1"},{"abi_all_100004_calls","1"},
        {"calls","100004"},{"hooked_target_calls","100004"},
        {"restore_attempted","1"},{"restore_bytes_readback","1"},
        {"restore_rw_no_exec_readback","1"},{"restore_protection_readback","1"},
        {"trampoline_unmapped_readback","1"},{"restored_code_executed","1"},
        {"cleanup_complete","1"},{"result","PASS"},
    };
    for (const auto& [key, value] : exact) {
        const auto found = fields.find(key);
        if (found == fields.end() || found->second != value) return false;
    }
    std::uint64_t parsed = 0;
    for (const auto& item : std::array<std::pair<const char*,std::uint64_t>,3>{
             std::pair{"pid",static_cast<std::uint64_t>(pid)},
             std::pair{"tid",static_cast<std::uint64_t>(tid)},
             std::pair{"trigger_nonce",nonce}}) {
        const auto found = fields.find(item.first);
        if (found == fields.end() || !ParseUnsigned(found->second, &parsed) ||
            parsed != item.second) return false;
    }
    for (const char* key : {"checks","mapping_metadata_queries"}) {
        const auto found = fields.find(key);
        if (found == fields.end() || !ParseUnsigned(found->second, &parsed) || parsed == 0)
            return false;
    }
    const auto protection = fields.find("target_initial_host_protection");
    const auto exec_visible = fields.find("target_initial_host_exec_visible");
    if (protection == fields.end() || exec_visible == fields.end() ||
        !ParseUnsigned(protection->second, &parsed) || (parsed != 1 && parsed != 5) ||
        (exec_visible->second != "0" && exec_visible->second != "1") ||
        ((parsed == 5) != (exec_visible->second == "1"))) return false;
    const auto header = fields.find("physics_token_gate_sha256");
    if (header == fields.end() || header->second != kPayloadHeaderSha256)
        return false;
    static const std::set<std::string> variable = {
        "pid","tid","trigger_nonce","checks","mapping_metadata_queries",
        "target_initial_host_protection","target_initial_host_exec_visible",
        "physics_token_gate_sha256",
    };
    if (fields.size() != exact.size() + variable.size()) return false;
    return true;
}
#endif

bool IsAlive(pid_t pid, std::uint64_t start_ticks) {
    return start_ticks != 0 && ProcessStartTicks(pid) == start_ticks;
}

[[maybe_unused]] int Fail(const char* stage, int code) {
    std::fprintf(stderr, "HABI1_ONE_SHOT passed=0 stage=%s code=%d errno=%d\n",
                 stage, code, errno);
    return code;
}

}  // namespace

#ifndef A9TAS_HABI1_CONTROLLER_CORE_ONLY
int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s PID START_TICKS NONCE\n", argv[0]);
        return Fail("usage", 2);
    }
    char* pid_end = nullptr;
    char* ticks_end = nullptr;
    char* nonce_end = nullptr;
    errno = 0;
    const long parsed_pid = std::strtol(argv[1], &pid_end, 10);
    const unsigned long long expected_ticks = std::strtoull(argv[2], &ticks_end, 10);
    const unsigned long long nonce = std::strtoull(argv[3], &nonce_end, 10);
    if (errno != 0 || parsed_pid <= 0 || !pid_end || *pid_end != '\0' ||
        expected_ticks == 0 || !ticks_end || *ticks_end != '\0' ||
        nonce == 0 || !nonce_end || *nonce_end != '\0') return Fail("arguments", 2);
    const pid_t pid = static_cast<pid_t>(parsed_pid);
    if (!IsAlive(pid, expected_ticks) || TracerPid(pid) != 0)
        return Fail("process_identity", 3);

    FileImage payload_file{}, bootstrap_file{};
    if (!ReadPinnedRegularFile(kPayloadPath, kPayloadSha256, &payload_file) ||
        !ReadPinnedRegularFile(kBootstrapPath, kBootstrapSha256, &bootstrap_file))
        return Fail("artifact_identity", 4);
    ElfImage payload_elf{}, bootstrap_elf{};
    if (!payload_elf.Parse(payload_file, EM_AARCH64) ||
        payload_elf.build_id() != kPayloadBuildId ||
        !bootstrap_elf.Parse(bootstrap_file, EM_X86_64) ||
        bootstrap_elf.build_id() != kBootstrapBuildId)
        return Fail("elf_identity", 5);

    Elf64_Sym locator_symbol{}, stage_symbol{}, probe_symbol{}, trampoline_symbol{};
    Elf64_Sym trap_symbol{}, calibrate_symbol{};
    if (!bootstrap_elf.ResolveUnique(kLocatorSymbol, true, &locator_symbol) ||
        ELF64_ST_TYPE(locator_symbol.st_info) != STT_OBJECT ||
        ELF64_ST_BIND(locator_symbol.st_info) != STB_GLOBAL ||
        locator_symbol.st_size != sizeof(Locator) ||
        !bootstrap_elf.AddressInRelro(locator_symbol.st_value, locator_symbol.st_size) ||
        !bootstrap_elf.ResolveUnique(kStageSymbol, false, &stage_symbol) ||
        !bootstrap_elf.ResolveUnique(kProbeStatusSymbol, false, &probe_symbol) ||
        !bootstrap_elf.ResolveUnique(kTrampolineSymbol, false, &trampoline_symbol) ||
        !bootstrap_elf.ResolveUnique(kTrapSymbol, true, &trap_symbol) ||
        !bootstrap_elf.ResolveUnique(kCalibrateSymbol, true, &calibrate_symbol))
        return Fail("bootstrap_symbols", 6);

    const std::vector<Mapping> maps = ReadMaps(pid);
    std::uintptr_t base = 0;
    if (!UniqueOffsetZeroBase(maps, kBootstrapPath, bootstrap_file.status, &base) ||
        !HasPinnedMapping(maps, kPayloadPath, payload_file.status))
        return Fail("runtime_mappings", 7);
    const std::uintptr_t locator_address = base + locator_symbol.st_value;
    const Mapping* locator_mapping = FindMapping(maps, locator_address, sizeof(Locator));
    if (!locator_mapping || !MappingMatchesFile(*locator_mapping, kBootstrapPath,
                                                bootstrap_file.status) ||
        !locator_mapping->readable || locator_mapping->writable)
        return Fail("locator_mapping", 8);
    Locator locator{};
    if (!ReadStableLocator(pid, locator_address, &locator) ||
        locator.magic != kLocatorMagic || locator.version != 2 ||
        locator.size != sizeof(Locator) ||
        !ExactString(locator.payload_path,sizeof(locator.payload_path),kPayloadPath) ||
        !ExactString(locator.payload_sha256,sizeof(locator.payload_sha256),kPayloadSha256) ||
        !ExactString(locator.payload_build_id,sizeof(locator.payload_build_id),kPayloadBuildId) ||
        !ExactString(locator.payload_source_sha256,sizeof(locator.payload_source_sha256),kPayloadSourceSha256) ||
        !ExactString(locator.run_symbol,sizeof(locator.run_symbol),
                     "a9tas_hook_abi_selftest_habi1_run") ||
        !ExactString(locator.shorty,sizeof(locator.shorty),"JJ") ||
        locator.stage_size != 4 || locator.probe_status_size != 4 ||
        locator.trampoline_size != 8 || locator.reserved != 0 ||
        locator.stage_address != base + stage_symbol.st_value ||
        locator.probe_status_address != base + probe_symbol.st_value ||
        locator.trampoline_address != base + trampoline_symbol.st_value ||
        locator.return_trap_address != base + trap_symbol.st_value ||
        locator.calibrate_tid_address != base + calibrate_symbol.st_value)
        return Fail("locator_identity", 9);

    const Mapping* trap_map = FindMapping(maps, locator.return_trap_address, 2);
    const Mapping* calibrate_map = FindMapping(maps, locator.calibrate_tid_address, 1);
    const Mapping* stage_map = FindMapping(maps, locator.stage_address, 4);
    if (!trap_map || !calibrate_map || !stage_map ||
        !MappingMatchesFile(*trap_map,kBootstrapPath,bootstrap_file.status) ||
        !MappingMatchesFile(*calibrate_map,kBootstrapPath,bootstrap_file.status) ||
        !trap_map->readable || !trap_map->executable || trap_map->writable ||
        !calibrate_map->readable || !calibrate_map->executable || calibrate_map->writable ||
        !stage_map->readable || !stage_map->writable || !stage_map->private_mapping)
        return Fail("locator_targets", 10);
    int stage = 0, probe_status = 0;
    std::uintptr_t guest_trampoline = 0;
    if (!ReadProcessValue(pid,locator.stage_address,&stage) || stage != 5 ||
        !ReadProcessValue(pid,locator.probe_status_address,&probe_status) ||
        probe_status != 1 ||
        !ReadProcessValue(pid,locator.trampoline_address,&guest_trampoline) ||
        guest_trampoline == 0)
        return Fail("bootstrap_state", 11);
    const Mapping* guest_map = FindMapping(maps, guest_trampoline);
    if (!guest_map || !guest_map->readable || !guest_map->executable ||
        guest_map->writable || !guest_map->private_mapping)
        return Fail("guest_trampoline_mapping", 12);

    const pid_t tid = UniqueSignalCatcher(pid);
    if (tid <= 0 || !IsAlive(pid, expected_ticks) || TracerPid(pid) != 0)
        return Fail("signal_catcher", 13);
    // The payload itself removes a stale receipt before running, but the host
    // gate also refuses to start if one is already visible.
    struct stat stale{};
    if (lstat(kReceiptPath, &stale) == 0 || errno != ENOENT)
        return Fail("stale_receipt", 14);

    if (ptrace(PTRACE_ATTACH, tid, nullptr, nullptr) == -1)
        return Fail("attach", 15);
    int attach_status = 0;
    if (waitpid(tid, &attach_status, __WALL) != tid ||
        !WIFSTOPPED(attach_status) || WSTOPSIG(attach_status) != SIGSTOP) {
        (void)KillUncertainProcess(pid);
        return Fail("attach_wait", 16);
    }
    bool uncertain = false;
    user_regs_struct original{};
    if (!GetRegs(tid, &original) || ProcessStartTicks(pid) != expected_ticks ||
        ThreadName(pid,tid) != "Signal Catcher" || ThreadGroupId(tid) != pid) {
        uncertain = true;
    }

    long calibrated_bias = 0;
    bool calibrated = false;
    CallReport calibration_report{};
    const std::uint64_t zero_args[6]{};
    if (!uncertain) {
        constexpr long candidates[] = {0,2,-2,4};
        for (long candidate : candidates) {
            const bool called = RemoteCallOnce(
                tid,maps,original,candidate,locator.calibrate_tid_address,
                locator.return_trap_address,zero_args,&calibration_report);
            if (called && calibration_report.result == static_cast<std::uint64_t>(tid)) {
                calibrated_bias = candidate;
                calibrated = true;
                break;
            }
            if (!calibration_report.detach_safe ||
                calibration_report.stop_signal != SIGTRAP) {
                uncertain = true;
                break;
            }
        }
    }
    if (!calibrated) uncertain = true;

    CallReport guest_report{};
    std::uint64_t guest_return = 0;
    bool guest_called = false;
    std::uint32_t returned_tid = 0;
    std::uint32_t tag = 0;
    if (!uncertain) {
        const std::uint64_t guest_args[6] = {0,0,nonce,0,0,0};
        guest_called = RemoteCallOnce(
            tid,maps,original,calibrated_bias,guest_trampoline,
            locator.return_trap_address,guest_args,&guest_report);
        guest_return = guest_report.result;
        returned_tid = static_cast<std::uint32_t>(guest_return >> 32);
        tag = static_cast<std::uint32_t>(guest_return);
        if (!guest_called || !guest_report.detach_safe ||
            returned_tid != static_cast<std::uint32_t>(tid) ||
            tag != kRunPassTag) uncertain = true;
    }

    if (uncertain) {
        const bool killed = KillUncertainProcess(pid);
        std::fprintf(stderr,
            "HABI1_ONE_SHOT passed=0 stage=remote_call process_killed=%d "
            "calibrated=%d calibration_safe=%d guest_called=%d guest_safe=%d "
            "guest_signal=%d guest_status=0x%x guest_errno=%d guest_rip=0x%llx "
            "guest_return=0x%016llx returned_tid=%u tag=0x%08x\n",
            killed?1:0,calibrated?1:0,calibration_report.detach_safe?1:0,
            guest_called?1:0,guest_report.detach_safe?1:0,
            guest_report.stop_signal,guest_report.wait_status,guest_report.error,
            static_cast<unsigned long long>(guest_report.rip),
            static_cast<unsigned long long>(guest_return),returned_tid,tag);
        return 17;
    }
    if (ptrace(PTRACE_DETACH,tid,nullptr,nullptr) == -1) {
        const bool killed = KillUncertainProcess(pid);
        std::fprintf(stderr,
            "HABI1_ONE_SHOT passed=0 stage=detach process_killed=%d\n",
            killed?1:0);
        return 18;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!IsAlive(pid, expected_ticks) || TracerPid(pid) != 0) {
        (void)KillUncertainProcess(pid);
        return Fail("post_detach_identity", 19);
    }

    FileImage receipt{};
    // Receipt contents are unique to this run; SHA is reported after strict
    // semantic parsing rather than pinned in advance.
    if (!ReadPinnedRegularFile(kReceiptPath, "", &receipt)) {
        // Empty expected hash is intentionally not accepted by the generic
        // pinned reader, so read the now-created regular file directly.
        struct stat receipt_status{};
        if (lstat(kReceiptPath,&receipt_status) != 0 ||
            !S_ISREG(receipt_status.st_mode) || S_ISLNK(receipt_status.st_mode))
            { (void)KillUncertainProcess(pid); return Fail("receipt_file",20); }
        const int fd=open(kReceiptPath,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0) { (void)KillUncertainProcess(pid); return Fail("receipt_open",20); }
        struct stat opened{};
        bool ok=fstat(fd,&opened)==0 && opened.st_dev==receipt_status.st_dev &&
                opened.st_ino==receipt_status.st_ino && opened.st_size>0 &&
                opened.st_size<=16384;
        if(ok){
            receipt.bytes.resize(static_cast<std::size_t>(opened.st_size));
            std::size_t done=0;
            while(done<receipt.bytes.size()){
                const ssize_t n=read(fd,receipt.bytes.data()+done,receipt.bytes.size()-done);
                if(n<=0){ok=false;break;} done+=static_cast<std::size_t>(n);
            }
        }
        ok=close(fd)==0 && ok;
        if(!ok) { (void)KillUncertainProcess(pid); return Fail("receipt_read",20); }
        receipt.status=opened;
        receipt.sha256=Sha256Bytes(receipt.bytes.data(),receipt.bytes.size());
    }
    if (!ParseReceipt(receipt,pid,tid,nonce)) {
        (void)KillUncertainProcess(pid);
        return Fail("receipt_semantics",21);
    }
    const bool terminated = KillUncertainProcess(pid);
    if (!terminated) return Fail("success_termination",22);
    std::printf(
        "HABI1_ONE_SHOT passed=1 pid=%d tid=%d start_ticks=%llu nonce=%llu "
        "bias=%ld guest_calls=1 pass_tag=0x%08x rollback=1 detach=1 "
        "process_terminated=1 receipt_sha256=%s\n",
        pid,tid,expected_ticks,nonce,calibrated_bias,kRunPassTag,
        receipt.sha256.c_str());
    return 0;
}
#endif
