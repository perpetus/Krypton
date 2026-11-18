// Copyright 2026 The Krypton Authors. GPL-3.0.
// TranslateKit-compatible entry points for the sandboxed local inference
// service. Only the supplied read-only storage callbacks can provide model
// bytes.
#include "translate_kit_structs.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "translator/service.h"

namespace {
namespace bergamot = marian::bergamot;
constexpr size_t kMaxConfig = 32 * 1024;
constexpr size_t kMaxInput = 8192;
constexpr size_t kMaxOutput = 64 * 1024;
constexpr uint64_t kMaxMappedFile = 128 * 1024 * 1024;
constexpr size_t kMaxModels = 4;
constexpr std::string_view kVersion = "2026.11.16.0";

struct Storage {
  FileExistsFn exists = nullptr;
  OpenForReadOnlyMemoryMapFn open = nullptr;
  DeleteReadOnlyMemoryRegionFn close = nullptr;
  ReadOnlyMemoryRegionDataFn data = nullptr;
  ReadOnlyMemoryRegionLengthFn length = nullptr;
  uintptr_t context = 0;
  explicit operator bool() const {
    return exists && open && close && data && length;
  }
};
Storage storage;
std::mutex mutex;

// These objects outlive the borrowed AlignedMemory descriptors and model graph.
class Region {
 public:
  Region(const Storage& backend, const std::string& name) : backend_(backend) {
    bool directory = false;
    if (!backend_.exists(name.data(), name.size(), &directory,
                         backend_.context) ||
        directory)
      return;
    handle_ = backend_.open(name.data(), name.size(), backend_.context);
    if (!handle_)
      return;
    const uint64_t length = backend_.length(handle_, backend_.context);
    if (length == 0 || length > kMaxMappedFile)
      return;
    data_ = backend_.data(handle_, backend_.context);
    size_ = static_cast<size_t>(length);
    if (reinterpret_cast<uintptr_t>(data_) % 256 != 0)
      data_ = nullptr;
  }
  ~Region() {
    if (handle_)
      backend_.close(handle_, backend_.context);
  }
  Region(const Region&) = delete;
  Region& operator=(const Region&) = delete;
  explicit operator bool() const { return data_ && size_; }
  const void* data() const { return data_; }
  size_t size() const { return size_; }
  bergamot::AlignedMemory Borrow() const {
    return bergamot::AlignedMemory::BorrowReadOnly(data_, size_);
  }

 private:
  Storage backend_;
  uintptr_t handle_ = 0;
  const void* data_ = nullptr;
  size_t size_ = 0;
};

bool Utf8(std::string_view input) {
  for (size_t i = 0; i < input.size();) {
    const auto lead = static_cast<unsigned char>(input[i++]);
    if (!lead)
      return false;
    if (lead < 128)
      continue;
    uint32_t value;
    size_t extra;
    uint32_t minimum;
    if (lead >= 0xc2 && lead <= 0xdf) {
      value = lead & 31;
      extra = 1;
      minimum = 0x80;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      value = lead & 15;
      extra = 2;
      minimum = 0x800;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      value = lead & 7;
      extra = 3;
      minimum = 0x10000;
    } else
      return false;
    if (extra > input.size() - i)
      return false;
    while (extra--) {
      const auto byte = static_cast<unsigned char>(input[i++]);
      if ((byte & 0xc0) != 0x80)
        return false;
      value = (value << 6) | (byte & 63);
    }
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff))
      return false;
  }
  return true;
}

class Wire {
 public:
  explicit Wire(std::string_view bytes) : bytes_(bytes) {}
  bool empty() const { return bytes_.empty(); }
  bool Varint(uint64_t& result) {
    result = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
      if (bytes_.empty())
        return false;
      const auto byte = static_cast<unsigned char>(bytes_.front());
      bytes_.remove_prefix(1);
      if (shift == 63 && byte > 1)
        return false;
      result |= uint64_t{byte & 127u} << shift;
      if (!(byte & 128))
        return true;
    }
    return false;
  }
  bool Bytes(std::string_view& result) {
    uint64_t length;
    if (!Varint(length) || length > bytes_.size())
      return false;
    result = bytes_.substr(0, static_cast<size_t>(length));
    bytes_.remove_prefix(static_cast<size_t>(length));
    return true;
  }

 private:
  std::string_view bytes_;
};

bool Language(std::string_view value) {
  return !value.empty() && value.size() <= 16 &&
         std::all_of(value.begin(), value.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
         });
}
struct Package {
  std::string first, second, path;
};
std::optional<std::vector<Package>> Packages(std::string_view data) {
  Wire wire(data);
  std::vector<Package> result;
  while (!wire.empty()) {
    uint64_t tag;
    std::string_view bytes;
    if (result.size() >= 40 || !wire.Varint(tag) || tag != 10 ||
        !wire.Bytes(bytes))
      return std::nullopt;
    Wire item(bytes);
    std::array<std::string, 3> fields;
    while (!item.empty()) {
      if (!item.Varint(tag) || (tag != 10 && tag != 18 && tag != 26) ||
          !item.Bytes(bytes))
        return std::nullopt;
      auto& field = fields[static_cast<size_t>(tag >> 3) - 1];
      if (!field.empty() || bytes.empty() || bytes.size() > 80 ||
          bytes.find('\0') != bytes.npos)
        return std::nullopt;
      field.assign(bytes);
    }
    // The service assigns virtual, numeric package roots. Never accept a real
    // path or user-supplied traversal here, even though storage is sandboxed.
    if (!Language(fields[0]) || !Language(fields[1]) ||
        fields[0] == fields[1] || (fields[0] != "en" && fields[1] != "en") ||
        fields[2].size() < 4 || fields[2].substr(0, 3) != "X:\\" ||
        !std::all_of(fields[2].begin() + 3, fields[2].end(),
                     [](char c) { return c >= '0' && c <= '9'; }))
      return std::nullopt;
    result.push_back(
        {std::move(fields[0]), std::move(fields[1]), std::move(fields[2])});
  }
  return result;
}

struct Model {
  uint64_t used = 0;
  std::vector<std::unique_ptr<Region>> regions;
  std::shared_ptr<bergamot::TranslationModel> model;
};
using Pair = std::pair<std::string, std::string>;
struct Kit {
  bergamot::BlockingService service{bergamot::BlockingService::Config{}};
  std::vector<Package> packages;
  std::map<Pair, std::shared_ptr<Model>> models;
  uint64_t model_use = 0;

  std::shared_ptr<Model> Load(const Pair& pair) {
    if (const auto it = models.find(pair); it != models.end()) {
      it->second->used = ++model_use;
      return it->second;
    }
    if (models.size() >= kMaxModels) {
      auto victim = models.end();
      for (auto candidate = models.begin(); candidate != models.end();
           ++candidate)
        if (candidate->second.use_count() == 1 &&
            (victim == models.end() ||
             candidate->second->used < victim->second->used))
          victim = candidate;
      if (victim == models.end())
        return nullptr;
      models.erase(victim);
    }
    const auto it =
        std::find_if(packages.begin(), packages.end(), [&](const auto& p) {
          return (p.first == pair.first && p.second == pair.second) ||
                 (p.second == pair.first && p.first == pair.second);
        });
    if (it == packages.end())
      return nullptr;
    const std::string prefix =
        it->path + "\\" + pair.first + "-" + pair.second + "\\";
    auto result = std::make_shared<Model>();
    result->used = ++model_use;
    auto map = [&](const char* file) -> const Region* {
      auto region = std::make_unique<Region>(storage, prefix + file);
      if (!*region)
        return nullptr;
      const auto* pointer = region.get();
      result->regions.push_back(std::move(region));
      return pointer;
    };
    bergamot::MemoryBundle memory;
    const auto* weights = map("model.bin");
    if (!weights)
      return nullptr;
    memory.models.push_back(weights->Borrow());
    if (const auto* shortlist = map("lex.bin"))
      memory.shortlist = shortlist->Borrow();
    if (const auto* shared = map("vocab.spm")) {
      auto vocab = std::make_shared<bergamot::AlignedMemory>(shared->Borrow());
      memory.vocabs = {vocab, vocab};
    } else {
      const auto* source = map("source_vocab.spm");
      const auto* target = map("target_vocab.spm");
      if (!source || !target)
        return nullptr;
      memory.vocabs = {
          std::make_shared<bergamot::AlignedMemory>(source->Borrow()),
          std::make_shared<bergamot::AlignedMemory>(target->Borrow())};
    }
    // Configuration is owned code, never parsed from page or model-pack input.
    // One replica, no worker threads, bounded greedy decoding and workspace.
    // Marian's embedding and output layers read vocabs[encoder/decoder index]
    // to detect factored (.fsv) vocabularies. With only in-memory vocabularies
    // that list was empty, and the out-of-range read crashed the sandboxed
    // service on its first sentence. These names fill both slots; they are
    // not factored and are never opened, as the buffers above take precedence.
    constexpr char config[] =
        "beam-size: 1\nnormalize: 1.0\nword-penalty: 0\n"
        "max-length-break: 128\nmax-length-factor: 2.0\n"
        "mini-batch-words: 256\nworkspace: 128\ncpu-threads: 1\n"
        "skip-cost: true\nquiet: true\nssplit-mode: paragraph\n"
        "gemm-precision: int8shiftAll\ncheck-bytearray: false\n"
        "vocabs: [memory-source.spm, memory-target.spm]\n";
    result->model = std::make_shared<bergamot::TranslationModel>(
        config, std::move(memory), 1);
    models.emplace(pair, result);
    return result;
  }
};
struct Translator {
  std::shared_ptr<Kit> kit;
  Pair first;
  Pair second;
};
std::map<uintptr_t, std::shared_ptr<Kit>> kits;
std::map<uintptr_t, Translator> translators;
std::weak_ptr<Kit> live_kit;
uintptr_t next_handle = 1;
uintptr_t Handle() {
  if (next_handle == std::numeric_limits<uintptr_t>::max())
    return 0;
  return next_handle++;
}
}  // namespace

#define EXPORT extern "C" __declspec(dllexport)

EXPORT bool GetTranslateKitVersion(TranslateKitVersion* version) noexcept {
  if (!version || !version->buffer || version->buffer_size < kVersion.size())
    return false;
  std::memcpy(version->buffer, kVersion.data(), kVersion.size());
  version->buffer_size = kVersion.size();
  return true;
}

EXPORT void InitializeStorageBackend(FileExistsFn exists,
                                     OpenForReadOnlyMemoryMapFn open,
                                     DeleteReadOnlyMemoryRegionFn close,
                                     ReadOnlyMemoryRegionDataFn data,
                                     ReadOnlyMemoryRegionLengthFn length,
                                     uintptr_t context) noexcept {
  try {
    std::lock_guard lock(mutex);
    if (!live_kit.expired())
      return;
    storage = {exists, open, close, data, length, context};
  } catch (...) {
  }
}

EXPORT uintptr_t CreateTranslateKit() noexcept {
  try {
    std::lock_guard lock(mutex);
    if (!storage || !live_kit.expired())
      return 0;
    marian::setThrowExceptionOnAbort(true);
    auto kit = std::make_shared<Kit>();
    const auto handle = Handle();
    if (!handle)
      return 0;
    live_kit = kit;
    kits.emplace(handle, std::move(kit));
    return handle;
  } catch (...) {
    return 0;
  }
}

EXPORT void DeleteTranslateKit(uintptr_t handle) noexcept {
  try {
    std::lock_guard lock(mutex);
    kits.erase(handle);
  } catch (...) {
  }
}

EXPORT bool TranslateKitSetLanguagePackages(
    uintptr_t handle,
    TranslateKitSetLanguagePackagesArgs args) noexcept {
  try {
    if ((!args.package_config && args.package_config_size) ||
        args.package_config_size > kMaxConfig)
      return false;
    auto packages = Packages({args.package_config ? args.package_config : "",
                              args.package_config_size});
    if (!packages)
      return false;
    std::lock_guard lock(mutex);
    const auto it = kits.find(handle);
    if (it == kits.end())
      return false;
    it->second->packages = std::move(*packages);
    return true;
  } catch (...) {
    return false;
  }
}

EXPORT uintptr_t
TranslateKitCreateTranslator(uintptr_t handle,
                             TranslateKitLanguage source,
                             TranslateKitLanguage target) noexcept {
  try {
    if (!source.language_code || !target.language_code ||
        source.language_code_size > 16 || target.language_code_size > 16)
      return 0;
    std::string from(source.language_code, source.language_code_size);
    std::string to(target.language_code, target.language_code_size);
    if (!Language(from) || !Language(to) || from == to)
      return 0;
    std::lock_guard lock(mutex);
    const auto it = kits.find(handle);
    if (it == kits.end() || translators.size() >= 512)
      return 0;
    auto kit = it->second;
    auto first = kit->Load({from, from == "en" ? to : "en"});
    if (!first)
      return 0;
    auto second = from != "en" && to != "en" ? kit->Load({"en", to}) : nullptr;
    if (from != "en" && to != "en" && !second)
      return 0;
    const auto id = Handle();
    if (!id)
      return 0;
    translators.emplace(
        id, Translator{std::move(kit),
                       {from, from == "en" ? to : "en"},
                       from != "en" && to != "en" ? Pair{"en", to} : Pair{}});
    return id;
  } catch (...) {
    return 0;
  }
}

EXPORT void DeleteTranslator(uintptr_t handle) noexcept {
  try {
    std::lock_guard lock(mutex);
    translators.erase(handle);
  } catch (...) {
  }
}

EXPORT bool TranslatorTranslate(uintptr_t handle,
                                TranslateKitInputText text,
                                void (*callback)(TranslateKitOutputText,
                                                 uintptr_t),
                                uintptr_t context) noexcept {
  try {
    if (!callback || (!text.input_text && text.input_text_size) ||
        text.input_text_size > kMaxInput)
      return false;
    std::string input(text.input_text ? text.input_text : "",
                      text.input_text_size);
    if (!Utf8(input))
      return false;
    std::string output;
    {
      std::lock_guard lock(mutex);
      const auto it = translators.find(handle);
      if (it == translators.end())
        return false;
      if (!input.empty()) {
        auto& translator = it->second;
        auto first = translator.kit->Load(translator.first);
        auto second = translator.second.first.empty()
                          ? nullptr
                          : translator.kit->Load(translator.second);
        if (!first || (!translator.second.first.empty() && !second))
          return false;
        std::vector<std::string> inputs;
        inputs.push_back(std::move(input));
        const std::vector<bergamot::ResponseOptions> options(1);
        auto results = second ? translator.kit->service.pivotMultiple(
                                    first->model, second->model,
                                    std::move(inputs), options)
                              : translator.kit->service.translateMultiple(
                                    first->model, std::move(inputs), options);
        if (results.size() != 1)
          return false;
        output = std::move(results[0].target.text);
        if (output.size() > kMaxOutput || !Utf8(output))
          return false;
      }
    }
    callback({output.data(), output.size()}, context);
    return true;
  } catch (...) {
    return false;
  }
}

EXPORT bool TranslateKitSplitSentences(TranslateKitInputText text,
                                       TranslateKitLanguage,
                                       void (*callback)(TranslateKitOutputText,
                                                        uintptr_t),
                                       uintptr_t context) noexcept {
  // Bergamot performs bounded sentence splitting internally. Avoid a second
  // tokenizer or file-backed segmentation data in this compatibility entry.
  if (!callback || (!text.input_text && text.input_text_size) ||
      text.input_text_size > kMaxInput ||
      !Utf8({text.input_text ? text.input_text : "", text.input_text_size}))
    return false;
  try {
    callback({text.input_text ? text.input_text : "", text.input_text_size},
             context);
    return true;
  } catch (...) {
    return false;
  }
}
