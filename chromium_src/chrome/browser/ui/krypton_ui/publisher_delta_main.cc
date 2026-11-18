// Copyright 2026 The Krypton Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/logging/logging_settings.h"
#include "base/numerics/safe_conversions.h"
#include "base/process/memory.h"
#include "base/strings/string_number_conversions.h"
#include "components/zucchini/encoded_view.h"
#include "components/zucchini/image_index.h"
#include "components/zucchini/mapped_file.h"
#include "components/zucchini/patch_writer.h"
#include "components/zucchini/suffix_array.h"
#include "components/zucchini/zucchini.h"
#include "components/zucchini/zucchini_gen.h"

namespace {

constexpr size_t kMinChunkBytes = 4 * 1024 * 1024;
constexpr size_t kOldOverlapBytes = 4 * 1024 * 1024;
constexpr size_t kMaxInputBytes = size_t{2} * 1024 * 1024 * 1024;

// Each worker owns its suffix array and patch streams. Overlap allows matching
// bytes that shifted across a chunk boundary without indexing the entire base
// once per worker. The serialized result uses ordinary Zucchini raw elements.
std::optional<zucchini::PatchElementWriter> GenerateChunk(
    zucchini::ConstBufferView old_image,
    zucchini::ConstBufferView new_image,
    size_t index,
    size_t count) {
  const size_t new_lo = new_image.size() * index / count;
  const size_t new_hi = new_image.size() * (index + 1) / count;
  const zucchini::BufferRegion new_region{new_lo, new_hi - new_lo};
  const auto new_chunk = new_image[new_region];

  // Avoid suffix sorting altogether for unchanged chunks at the same offset.
  if (old_image.covers(new_region) && old_image[new_region].equals(new_chunk)) {
    zucchini::PatchElementWriter element(
        {zucchini::Element(new_region), zucchini::Element(new_region)});
    zucchini::EquivalenceSink equivalences;
    equivalences.PutNext(
        {0, 0, base::checked_cast<zucchini::offset_t>(new_region.size)});
    element.SetEquivalenceSink(std::move(equivalences));
    element.SetExtraDataSink({});
    element.SetRawDeltaSink({});
    element.SetReferenceDeltaSink({});
    LOG(INFO) << "Chunk " << index + 1 << '/' << count << ": unchanged";
    return std::optional<zucchini::PatchElementWriter>(std::move(element));
  }

  const size_t old_core_lo = old_image.size() * index / count;
  const size_t old_core_hi = old_image.size() * (index + 1) / count;
  const size_t old_lo = old_core_lo - std::min(old_core_lo, kOldOverlapBytes);
  const size_t old_hi =
      old_core_hi + std::min(old_image.size() - old_core_hi, kOldOverlapBytes);
  const zucchini::BufferRegion old_region{old_lo, old_hi - old_lo};
  const auto old_chunk = old_image[old_region];
  const zucchini::ImageIndex old_index(old_chunk);
  const zucchini::EncodedView old_view(old_index);
  const auto suffix_array =
      zucchini::MakeSuffixArray<zucchini::InducedSuffixSort>(
          old_view, old_view.Cardinality());
  zucchini::PatchElementWriter element(
      {zucchini::Element(old_region), zucchini::Element(new_region)});
  if (!zucchini::GenerateRawElement(suffix_array, old_chunk, new_chunk,
                                    &element)) {
    return std::nullopt;
  }
  LOG(INFO) << "Chunk " << index + 1 << '/' << count << ": generated";
  return std::optional<zucchini::PatchElementWriter>(std::move(element));
}

zucchini::status::Code Generate(const base::CommandLine& command_line) {
  const auto& args = command_line.GetArgs();
  size_t requested_threads = 0;
  if (!command_line.HasSwitch("gen") || args.size() != 3 ||
      (command_line.HasSwitch("threads") &&
       !base::StringToSizeT(command_line.GetSwitchValueASCII("threads"),
                            &requested_threads))) {
    std::cerr << "Usage: KryptonDeltaGenerator -gen <old> <new> <patch> "
                 "[-threads=0]\n0 selects all available CPU threads.\n";
    return zucchini::status::kStatusInvalidParam;
  }

  const base::FilePath old_path(args[0]);
  const base::FilePath new_path(args[1]);
  const base::FilePath patch_path(args[2]);
  zucchini::MappedFileReader old_file(
      base::File(old_path, base::File::FLAG_OPEN | base::File::FLAG_READ));
  zucchini::MappedFileReader new_file(
      base::File(new_path, base::File::FLAG_OPEN | base::File::FLAG_READ));
  if (old_file.HasError() || new_file.HasError()) {
    LOG(ERROR) << "Could not map inputs: " << old_file.error() << ' '
               << new_file.error();
    return zucchini::status::kStatusFileReadError;
  }
  const auto old_image = old_file.region();
  const auto new_image = new_file.region();
  if (old_image.empty() || new_image.empty() ||
      old_image.size() > kMaxInputBytes || new_image.size() > kMaxInputBytes) {
    LOG(ERROR) << "Publisher inputs must contain between 1 byte and 2 GiB.";
    return zucchini::status::kStatusInvalidParam;
  }

  // Check output access before suffix sorting. Zucchini's mapped writer needs
  // DELETE access so failed or interrupted generations leave no partial patch.
  base::File output_file(patch_path, base::File::FLAG_CREATE |
                                         base::File::FLAG_READ |
                                         base::File::FLAG_WRITE |
                                         base::File::FLAG_WIN_SHARE_DELETE |
                                         base::File::FLAG_CAN_DELETE_ON_CLOSE);
  if (!output_file.IsValid()) {
    LOG(ERROR) << "Could not create the new patch file.";
    return zucchini::status::kStatusFileWriteError;
  }
  if (!output_file.DeleteOnClose(true)) {
    output_file.Close();
    base::DeleteFile(patch_path);
    LOG(ERROR) << "Could not enable failed-output cleanup.";
    return zucchini::status::kStatusFileWriteError;
  }

  const size_t available_threads =
      std::max(1u, std::thread::hardware_concurrency());
  const size_t thread_count =
      requested_threads == 0 ? available_threads : requested_threads;
  const size_t useful_chunks = std::max(
      size_t{1}, std::min(old_image.size(), new_image.size()) / kMinChunkBytes);
  const size_t chunk_count = std::min(thread_count, useful_chunks);
  LOG(INFO) << "Parallel raw Zucchini generation: " << chunk_count
            << " workers, " << available_threads << " available CPU threads, "
            << kOldOverlapBytes << " bytes old-region overlap; "
            << "executable scanning skipped.";

  std::vector<std::optional<zucchini::PatchElementWriter>> elements(
      chunk_count);
  {
    std::vector<std::jthread> workers;
    workers.reserve(chunk_count);
    for (size_t index = 0; index < chunk_count; ++index) {
      workers.emplace_back([&, index] {
        auto element = GenerateChunk(old_image, new_image, index, chunk_count);
        if (element) {
          elements[index].emplace(std::move(*element));
        }
      });
    }
    // jthread destruction joins every worker before the views or slots expire.
  }

  zucchini::EnsemblePatchWriter writer(old_image, new_image);
  writer.ReserveElements(chunk_count);
  for (auto& element : elements) {
    if (!element) {
      LOG(ERROR) << "A raw patch worker failed; no patch was written.";
      return zucchini::status::kStatusFatal;
    }
    writer.AddElement(std::move(*element));
  }
  // Never truncate an existing result or either input. Failed writes are
  // automatically removed by MappedFileWriter.
  zucchini::MappedFileWriter patch_file(patch_path, std::move(output_file),
                                        writer.SerializedSize());
  if (patch_file.HasError()) {
    LOG(ERROR) << "Could not create patch: " << patch_file.error();
    return zucchini::status::kStatusFileWriteError;
  }
  if (!writer.SerializeInto(patch_file.region())) {
    return zucchini::status::kStatusPatchWriteError;
  }
  return patch_file.Keep() ? zucchini::status::kStatusSuccess
                           : zucchini::status::kStatusFileWriteError;
}

}  // namespace

int main(int argc, const char* argv[]) {
  base::CommandLine::Init(argc, argv);
  base::EnableTerminationOnHeapCorruption();
  base::EnableTerminationOnOutOfMemory();
  logging::LoggingSettings settings;
  settings.logging_dest = logging::LOG_TO_STDERR;
  if (!logging::InitLogging(settings)) {
    return zucchini::status::kStatusFatal;
  }
  const auto started = std::chrono::steady_clock::now();
  const auto result = Generate(*base::CommandLine::ForCurrentProcess());
  LOG(INFO) << "Publisher delta generation finished in "
            << std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                             started)
                   .count()
            << " seconds; exit " << static_cast<int>(result);
  return static_cast<int>(result);
}
