#include "camera_raceview_callback_node_v1.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace callback = a9tas::camera_raceview_callback_node_v1;
namespace active = a9tas::camera_active_state_resolver_v1;

namespace {

constexpr std::uintptr_t kBase = 0x10000000;
constexpr std::uintptr_t kManager = 0x20000100;
constexpr std::uintptr_t kNode = 0x20001000;

struct Image {
  std::array<std::uint8_t, 0x3000> bytes{};
};

bool Read(void* context, std::uintptr_t address, void* output,
          std::size_t size) {
  auto* image = static_cast<Image*>(context);
  if (address < 0x20000000 || address + size < address ||
      address + size > 0x20000000 + image->bytes.size())
    return false;
  std::memcpy(output, image->bytes.data() + address - 0x20000000, size);
  return true;
}

template <typename T>
void Put(Image* image, std::uintptr_t address, T value) {
  std::memcpy(image->bytes.data() + address - 0x20000000, &value,
              sizeof(value));
}

Image ValidImage() {
  Image image{};
  Put(&image, kManager + callback::kManagerNodeOffset, kNode);
  Put(&image, kNode, kBase + callback::kExpectedNodeVptrRva);
  Put<std::uint8_t>(&image, kNode + callback::kEnabledOffset, 1);
  Put(&image, kNode + callback::kOwnerOffset, kManager);
  Put(&image, kNode + callback::kSelfOffset,
      kNode + callback::kOwnerOffset);
  Put(&image, kNode + callback::kCallbackOffset,
      kBase + callback::kExpectedCallbackRva);
  Put<std::uintptr_t>(&image, kNode + callback::kContextOffset, 0);
  return image;
}

bool Run() {
  Image image = ValidImage();
  const active::Mapping mappings[] = {
      {kBase, kBase + 0x9000000, true, false, true, true},
      {0x20000000, 0x20003000, true, true, false, true},
  };
  callback::Snapshot snapshot{};
  if (callback::ReadSnapshot({&image, &Read}, mappings, 2, kBase, kManager,
                             &snapshot) != callback::Result::kOk ||
      snapshot.node != kNode || snapshot.owner != kManager)
    return false;
  Put(&image, kNode + callback::kCallbackOffset,
      kBase + callback::kExpectedCallbackRva + 4);
  return callback::ReadSnapshot({&image, &Read}, mappings, 2, kBase, kManager,
                                &snapshot) ==
         callback::Result::kCallbackMismatch;
}

}  // namespace

static_assert(callback::kNodeSize == 0x68);
static_assert(callback::kCallbackOffset == 0x58);

int main() { return Run() ? 0 : 1; }
