// Copyright 2026 The Krypton Authors. GPL-3.0.
// The caller is the existing sandboxed speech service. No network capability,
// transcript logger, audio file writer or background polling is provided here.
#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "moonshine-streaming-model.h"
#include "soda_async_impl.h"
#include "voice_model_manifest.h"

namespace {
constexpr size_t kMaximumConfig = 65536;
constexpr size_t kMaximumModel = 192 * 1024 * 1024;
constexpr size_t kMaximumTranscript = 32768;
constexpr size_t kAudioRate = 16000;
constexpr size_t kQueueSamples = kAudioRate * 2;
constexpr size_t kQuantum = 1600;
constexpr size_t kDecodeSamples = 4000;
constexpr size_t kMaximumSegment = kAudioRate * 20;

struct Wire {
  std::string_view data;
  bool Number(uint64_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 64 && !data.empty(); shift += 7) {
      const auto byte = static_cast<uint8_t>(data.front());
      data.remove_prefix(1);
      if (shift == 63 && byte > 1)
        return false;
      value |= static_cast<uint64_t>(byte & 127) << shift;
      if (!(byte & 128))
        return true;
    }
    return false;
  }
  bool Bytes(std::string_view& value) {
    uint64_t count;
    if (!Number(count) || count > data.size())
      return false;
    value = data.substr(0, static_cast<size_t>(count));
    data.remove_prefix(static_cast<size_t>(count));
    return true;
  }
  bool Skip(unsigned type) {
    uint64_t number;
    std::string_view bytes;
    if (type == 0)
      return Number(number);
    if (type == 2)
      return Bytes(bytes);
    const size_t count = type == 1 ? 8 : type == 5 ? 4 : 0;
    if (!count || data.size() < count)
      return false;
    data.remove_prefix(count);
    return true;
  }
};
void Number(std::string& out, uint64_t value) {
  while (value > 127) {
    out.push_back(static_cast<char>((value & 127) | 128));
    value >>= 7;
  }
  out.push_back(static_cast<char>(value));
}
void Field(std::string& out, unsigned field, std::string_view value) {
  Number(out, (uint64_t{field} << 3) | 2);
  Number(out, value.size());
  out.append(value);
}
bool ValidUtf8(std::string_view value) {
  return value.empty() ||
         (value.find('\0') == value.npos &&
          MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                              static_cast<int>(value.size()), nullptr, 0) > 0);
}
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Handle() {
    if (value != INVALID_HANDLE_VALUE && value)
      CloseHandle(value);
  }
};
struct Configuration {
  uint32_t channels = 0;
  uint32_t rate = 0;
  bool caption = false;
  std::filesystem::path directory;
  std::vector<std::unique_ptr<Handle>> model_files;
};
std::optional<Configuration> Parse(SerializedSodaConfig config) {
  if (!config.callback || !config.soda_config || config.soda_config_size <= 0 ||
      static_cast<size_t>(config.soda_config_size) > kMaximumConfig)
    return {};
  Configuration result;
  Wire wire{{config.soda_config, static_cast<size_t>(config.soda_config_size)}};
  std::string directory;
  while (!wire.data.empty()) {
    uint64_t tag, value;
    if (!wire.Number(tag) || !(tag >> 3))
      return {};
    const auto field = tag >> 3;
    const auto type = static_cast<unsigned>(tag & 7);
    if ((field == 1 || field == 2 || field == 8) && type == 0) {
      if (!wire.Number(value) || value > 192000)
        return {};
      if (field == 1)
        result.channels = static_cast<uint32_t>(value);
      else if (field == 2)
        result.rate = static_cast<uint32_t>(value);
      else
        result.caption = value == 2;
    } else if (field == 7 && type == 2) {
      std::string_view text;
      if (!wire.Bytes(text) || text.empty() || text.size() > 16000 ||
          !ValidUtf8(text))
        return {};
      directory = text;
    } else if (!wire.Skip(type)) {
      return {};
    }
  }
  if (!result.channels || result.channels > 8 || result.rate < 8000 ||
      result.rate > 192000 || directory.empty())
    return {};
  result.directory = std::filesystem::path(std::u8string_view(
      reinterpret_cast<const char8_t*>(directory.data()), directory.size()));
  if (!result.directory.is_absolute() || directory.starts_with("\\\\") ||
      directory.starts_with("//"))
    return {};
  for (const auto& part : result.directory)
    if (part == L".." || part == L".")
      return {};
  return result;
}
class Region {
 public:
  Region(HANDLE handle, const krypton_voice::ModelFile& file) {
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    LARGE_INTEGER length{};
    if (handle == INVALID_HANDLE_VALUE ||
        GetFileType(handle) != FILE_TYPE_DISK ||
        !GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes,
                                      sizeof(attributes)) ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !GetFileSizeEx(handle, &length) || length.QuadPart <= 0 ||
        static_cast<uint64_t>(length.QuadPart) != file.size ||
        file.size > kMaximumModel)
      throw std::runtime_error("Invalid local speech asset");
    size_ = static_cast<size_t>(file.size);
    mapping_.value =
        CreateFileMappingW(handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping_.value)
      throw std::runtime_error("Cannot map local speech asset");
    data_ = static_cast<const uint8_t*>(
        MapViewOfFile(mapping_.value, FILE_MAP_READ, 0, 0, 0));
    if (!data_)
      throw std::runtime_error("Cannot map local speech asset");
    std::array<uint8_t, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                   const_cast<PUCHAR>(data_), static_cast<ULONG>(size_),
                   digest.data(), static_cast<ULONG>(digest.size())) < 0 ||
        digest != file.sha256) {
      UnmapViewOfFile(data_);
      data_ = nullptr;
      throw std::runtime_error("Speech asset verification failed");
    }
  }
  Region(const Region&) = delete;
  Region& operator=(const Region&) = delete;
  ~Region() {
    if (data_)
      UnmapViewOfFile(data_);
  }
  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }

 private:
  Handle mapping_;
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
};

class Recognizer {
 public:
  Recognizer(Configuration config, SerializedSodaConfig callback)
      : config_(std::move(config)),
        callback_(callback.callback),
        callback_handle_(callback.callback_handle) {}
  ~Recognizer() {
    {
      std::lock_guard lock(mutex_);
      closing_ = true;
    }
    wake_.notify_one();
    if (thread_.joinable())
      thread_.join();
    // Audio remains only in bounded memory and is discarded at teardown.
    SecureZeroMemory(queue_.data(), sizeof(queue_));
  }
  void Start() {
    std::lock_guard lock(mutex_);
    if (thread_.joinable())
      return;
    thread_ = std::thread([this] { Run(); });
  }
  void Add(const char* audio, int count) {
    if (!audio || count <= 0 || count > 1024 * 1024 ||
        count % (sizeof(int16_t) * config_.channels))
      return;
    std::lock_guard lock(mutex_);
    if (closing_ || done_ || stopped_)
      return;
    const size_t frames =
        static_cast<size_t>(count) / (sizeof(int16_t) * config_.channels);
    // Stateless linear downsampling has one fractional phase across chunks.
    // The utility receives capture-device PCM, not UI-thread audio buffers.
    const double step = static_cast<double>(config_.rate) / kAudioRate;
    while (position_ < static_cast<double>(frames)) {
      const auto frame = static_cast<size_t>(position_);
      float sample = 0;
      for (uint32_t c = 0; c < config_.channels; ++c) {
        int16_t value;
        std::memcpy(&value,
                    audio + (frame * config_.channels + c) * sizeof(value),
                    sizeof(value));
        sample += static_cast<float>(value) / 32768;
      }
      if (queued_ == queue_.size()) {
        // Fail closed on sustained overload instead of buffering indefinitely.
        done_ = true;
        break;
      }
      queue_[(head_ + queued_++) % queue_.size()] = sample / config_.channels;
      position_ += step;
    }
    position_ -= static_cast<double>(frames);
    wake_.notify_one();
  }
  void Done() {
    std::lock_guard lock(mutex_);
    done_ = true;
    wake_.notify_one();
  }

 private:
  void Event(unsigned type) {
    std::string message;
    Number(message, 8);
    Number(message, type);
    callback_(message.data(), static_cast<int>(message.size()),
              callback_handle_);
  }
  void Text(const std::string& text, bool final) {
    if (text.size() > kMaximumTranscript || !ValidUtf8(text))
      throw std::runtime_error("Invalid recognition output");
    if (text.empty() || (!final && text == last_text_))
      return;
    std::string result;
    Field(result, 1, text);
    Number(result, 16);
    Number(result, final ? 2 : 1);
    if (final) {
      Number(result, 24);
      Number(result, 1);
    }
    std::string response;
    Number(response, 8);
    Number(response, 1);
    Field(response, 2, result);
    last_text_ = text;
    callback_(response.data(), static_cast<int>(response.size()),
              callback_handle_);
  }
  void Run() noexcept {
    try {
      const auto* pack = krypton_voice::FindPack(
          config_.directory.parent_path().filename().string(),
          config_.directory.filename().string());
      if (!pack || pack->files.size() != config_.model_files.size())
        throw std::runtime_error("Unknown speech pack");
      std::vector<std::unique_ptr<Region>> regions;
      regions.reserve(pack->files.size());
      for (size_t i = 0; i < pack->files.size(); ++i) {
        regions.push_back(std::make_unique<Region>(
            config_.model_files[i]->value, pack->files[i]));
      }
      auto asset = [&](std::string_view name) -> const Region& {
        for (size_t i = 0; i < pack->files.size(); ++i)
          if (pack->files[i].name == name)
            return *regions[i];
        throw std::runtime_error("Missing speech asset");
      };
      // The model object is destroyed before its readonly mappings.
      MoonshineStreamingModel model;
      const auto& json = asset("streaming_config.json");
      if (json.size() > 16384 ||
          model.load_config_from_string(std::string(
              reinterpret_cast<const char*>(json.data()), json.size())) != 0)
        throw std::runtime_error("Invalid streaming configuration");
      const auto& frontend = asset("frontend.model.ort");
      const auto& weights = asset("frontend.weights.ort");
      const auto& encoder = asset("encoder.ort");
      const auto& adapter = asset("adapter.ort");
      const auto& cross = asset("cross_kv.ort");
      const auto& decoder = asset("decoder_kv.ort");
      const auto& tokenizer = asset("tokenizer.bin");
      if (model.load_from_memory(
              frontend.data(), frontend.size(), encoder.data(), encoder.size(),
              adapter.data(), adapter.size(), cross.data(), cross.size(),
              decoder.data(), decoder.size(), tokenizer.data(),
              tokenizer.size(), model.config, pack->architecture,
              weights.data(), weights.size()))
        throw std::runtime_error("Cannot initialize streaming model");
      std::unique_ptr<MoonshineStreamingState> state(model.create_state());
      if (!state)
        throw std::runtime_error("Cannot initialize streaming state");
      Event(4);
      std::array<float, kQuantum> samples{};
      size_t segment = 0, silent = 0, since_decode = 0;
      bool heard_voice = false;
      float noise_floor = 0.002f;
      for (;;) {
        size_t count;
        bool final;
        {
          std::unique_lock lock(mutex_);
          wake_.wait(lock, [&] { return closing_ || done_ || queued_; });
          if (closing_)
            return;
          count = std::min(queued_, samples.size());
          for (size_t i = 0; i < count; ++i) {
            samples[i] = queue_[head_];
            queue_[head_] = 0;
            head_ = (head_ + 1) % queue_.size();
          }
          queued_ -= count;
          final = done_ && !queued_;
        }
        if (count) {
          double energy = 0;
          for (size_t i = 0; i < count; ++i)
            energy += samples[i] * samples[i];
          const float rms = static_cast<float>(std::sqrt(energy / count));
          const bool voice = rms > std::max(0.008f, noise_floor * 3);
          if (!voice)
            noise_floor = noise_floor * 0.98f + rms * 0.02f;
          heard_voice |= voice;
          silent = voice ? 0 : silent + count;
          segment += count;
          since_decode += count;
          int features = 0;
          if (model.process_audio_chunk(state.get(), samples.data(), count,
                                        &features))
            throw std::runtime_error("Streaming frontend failed");
        }
        // Audio-sample endpointing has no wall-clock timer or idle wakeup.
        const bool endpoint = heard_voice && silent >= kAudioRate * 3 / 4;
        const bool boundary = final || endpoint || segment >= kMaximumSegment;
        if (boundary || since_decode >= kDecodeSamples) {
          int frames = 0;
          if (model.encode(state.get(), boundary, &frames))
            throw std::runtime_error("Streaming encoder failed");
          if (state->memory_len > 0) {
            int* output = nullptr;
            int length = 0;
            if (model.decode_full(state.get(), nullptr, 0, &output, &length))
              throw std::runtime_error("Streaming decoder failed");
            std::unique_ptr<int, decltype(&std::free)> owned(output,
                                                             &std::free);
            if (length < 0 || length > 512)
              throw std::runtime_error("Streaming output exceeds limit");
            std::vector<int64_t> tokens;
            tokens.reserve(static_cast<size_t>(length));
            for (int i = 0; i < length; ++i)
              tokens.push_back(output[i]);
            Text(model.tokens_to_text(tokens), boundary);
          }
          since_decode = 0;
        }
        if (final || (boundary && !config_.caption))
          break;
        if (boundary) {
          state->reset(model.config);
          segment = silent = 0;
          heard_voice = false;
          last_text_.clear();
        }
      }
    } catch (...) {
      // Never unwind through the C ABI or expose audio/transcript to logs.
    }
    {
      std::lock_guard lock(mutex_);
      stopped_ = true;
    }
    Event(2);
  }

  Configuration config_;
  SerializedSodaEventHandler callback_;
  void* callback_handle_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::thread thread_;
  std::array<float, kQueueSamples> queue_{};
  size_t head_ = 0, queued_ = 0;
  double position_ = 0;
  bool closing_ = false, done_ = false, stopped_ = false;
  std::string last_text_;
};
std::mutex instances_mutex;
std::map<void*, std::unique_ptr<Recognizer>> instances;
}  // namespace

extern "C" {
__declspec(dllexport) void* CreateExtendedSodaAsync(
    SerializedSodaConfig) noexcept {
  // Krypton never opens model paths from the restricted utility.
  return nullptr;
}

__declspec(dllexport) void* KryptonCreateExtendedSodaAsyncWithModelFiles(
    SerializedSodaConfig config,
    const HANDLE* files,
    size_t count) noexcept {
  try {
    auto parsed = Parse(config);
    if (!parsed || !files || count == 0 || count > 16)
      return nullptr;
    parsed->model_files.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      auto model_file = std::make_unique<Handle>();
      if (!DuplicateHandle(GetCurrentProcess(), files[i], GetCurrentProcess(),
                           &model_file->value, GENERIC_READ, FALSE, 0)) {
        return nullptr;
      }
      parsed->model_files.push_back(std::move(model_file));
    }
    std::lock_guard lock(instances_mutex);
    if (instances.size() >= 4)
      return nullptr;
    auto value = std::make_unique<Recognizer>(std::move(*parsed), config);
    void* id = value.get();
    instances.emplace(id, std::move(value));
    return id;
  } catch (...) {
    return nullptr;
  }
}
__declspec(dllexport) void DeleteExtendedSodaAsync(void* id) noexcept {
  std::unique_ptr<Recognizer> value;
  {
    std::lock_guard lock(instances_mutex);
    auto it = instances.find(id);
    if (it == instances.end())
      return;
    value = std::move(it->second);
    instances.erase(it);
  }
}
__declspec(dllexport) void ExtendedSodaStart(void* id) noexcept {
  try {
    std::lock_guard lock(instances_mutex);
    if (const auto it = instances.find(id); it != instances.end())
      it->second->Start();
  } catch (...) {
  }
}
__declspec(dllexport) void ExtendedAddAudio(void* id,
                                            const char* data,
                                            int size) noexcept {
  try {
    std::lock_guard lock(instances_mutex);
    if (const auto it = instances.find(id); it != instances.end())
      it->second->Add(data, size);
  } catch (...) {
  }
}
__declspec(dllexport) void ExtendedSodaMarkDone(void* id) noexcept {
  try {
    std::lock_guard lock(instances_mutex);
    if (const auto it = instances.find(id); it != instances.end())
      it->second->Done();
  } catch (...) {
  }
}
void UpdateRecognitionContext(void*, RecognitionContext) {
  // Context biasing is deliberately unsupported; this adapter never retains
  // document text or user vocabulary outside the bounded recognition stream.
}
}
