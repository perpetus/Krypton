// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/document_store.h"

#include <optional>
#include <utility>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/supports_user_data.h"
#include "base/task/thread_pool.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "crypto/sha2.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/codec/png_codec.h"

namespace krypton_ui {
namespace {
constexpr size_t kDocumentLimit = 8 * 1024 * 1024;
constexpr size_t kImageLimit = 8 * 1024 * 1024;
constexpr size_t kTotalImagesLimit = 48 * 1024 * 1024;
constexpr size_t kImageCountLimit = 128;
constexpr size_t kListLimit = 1000;
constexpr char kStoreKey[] = "krypton.documents.store";
base::DictValue Error(std::string_view message) {
  return base::DictValue().Set("error", message);
}
const char* Filename(DocumentKind kind) {
  return kind == DocumentKind::kNote ? "document.md" : "document.excalidraw";
}
std::string Revision(std::string_view content) {
  return base::HexEncodeLower(crypto::SHA256HashString(content));
}
std::optional<std::string> Read(const base::FilePath& path, size_t maximum) {
  std::string bytes;
  if (base::IsLink(path) ||
      !base::ReadFileToStringWithMaxSize(path, &bytes, maximum))
    return std::nullopt;
  return bytes;
}
bool SafeDirectory(const base::FilePath& path) {
  return !base::IsLink(path) && base::CreateDirectory(path);
}
bool Write(const base::FilePath& path, std::string_view bytes) {
  return !base::IsLink(path) &&
         base::ImportantFileWriter::WriteFileAtomically(path, bytes);
}
std::string Title(const base::FilePath& dir, DocumentKind kind) {
  auto title = Read(dir.AppendASCII("title.txt"), 512);
  return title && base::IsStringUTF8(*title) && !title->empty() ? *title
         : kind == DocumentKind::kNote ? "Untitled note"
                                       : "Untitled easel";
}
// A renderer can submit only PNG pixels. Bound the encoded bytes and dimensions
// before decoding; re-encode on the blocking sequence, dropping metadata.
std::optional<std::string> CleanPng(std::string_view encoded) {
  if (encoded.size() > kImageLimit * 4 / 3 + 8)
    return std::nullopt;
  std::string bytes;
  if (!base::Base64Decode(encoded, &bytes) || bytes.size() < 24 ||
      bytes.size() > kImageLimit ||
      bytes.substr(0, 8) != std::string("\x89PNG\r\n\x1a\n", 8))
    return std::nullopt;
  auto integer = [&bytes](size_t offset) {
    uint32_t value = 0;
    for (size_t i = offset; i < offset + 4; ++i)
      value = (value << 8) | static_cast<unsigned char>(bytes[i]);
    return value;
  };
  const uint32_t width = integer(16), height = integer(20);
  if (!width || !height || width > 16384 || height > 16384 ||
      uint64_t{width} * height > 32 * 1024 * 1024)
    return std::nullopt;
  auto bitmap = gfx::PNGCodec::Decode(base::as_byte_span(bytes));
  if (bitmap.drawsNothing())
    return std::nullopt;
  auto clean = gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, false);
  if (!clean || clean->size() > kImageLimit)
    return std::nullopt;
  return std::string(reinterpret_cast<const char*>(clean->data()),
                     clean->size());
}
class StoreData final : public base::SupportsUserData::Data {
 public:
  explicit StoreData(scoped_refptr<DocumentStore> value)
      : store(std::move(value)) {}
  const scoped_refptr<DocumentStore> store;
};
}  // namespace

bool IsDocumentId(std::string_view id) {
  return id.size() == 36 &&
         base::Uuid::ParseLowercase(std::string(id)).is_valid();
}
scoped_refptr<DocumentStore> DocumentStore::ForProfile(Profile* profile) {
  if (!profile || profile->IsOffTheRecord() || !profile->IsRegularProfile())
    return nullptr;
  if (auto* data = static_cast<StoreData*>(profile->GetUserData(kStoreKey)))
    return data->store;
  auto store = base::MakeRefCounted<DocumentStore>(
      profile->GetPath().AppendASCII("KryptonDocuments"));
  profile->SetUserData(kStoreKey, std::make_unique<StoreData>(store));
  return store;
}
DocumentStore::DocumentStore(base::FilePath root)
    : root_(std::move(root)),
      runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})) {}
DocumentStore::~DocumentStore() = default;
void DocumentStore::Request(DocumentKind kind,
                            base::DictValue request,
                            Reply reply) {
  runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&DocumentStore::Run, base::WrapRefCounted(this), kind,
                     std::move(request)),
      std::move(reply));
}
void DocumentStore::CreateCapture(std::string png, Reply reply) {
  runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&DocumentStore::SaveCapture, base::WrapRefCounted(this),
                     std::move(png)),
      std::move(reply));
}
base::DictValue DocumentStore::SaveCapture(std::string png) {
  auto created =
      Run(DocumentKind::kEasel,
          base::DictValue().Set("operation", "load").Set("id", "new"));
  const auto* id = created.FindString("id");
  const auto* revision = created.FindString("revision");
  if (!id || !revision)
    return created;
  auto inserted = Run(DocumentKind::kEasel, base::DictValue()
                                                .Set("operation", "insertImage")
                                                .Set("id", *id)
                                                .Set("png", std::move(png)));
  const auto* image_id = inserted.FindString("image");
  const auto* pixels = inserted.FindString("png");
  if (!image_id || !pixels)
    return inserted;
  std::string decoded;
  if (!base::Base64Decode(*pixels, &decoded))
    return Error("Could not decode the capture.");
  auto bitmap = gfx::PNGCodec::Decode(base::as_byte_span(decoded));
  auto element =
      base::DictValue()
          .Set("id", base::Uuid::GenerateRandomV4().AsLowercaseString())
          .Set("type", "image")
          .Set("x", 0)
          .Set("y", 0)
          .Set("width", bitmap.width())
          .Set("height", bitmap.height())
          .Set("fileId", *image_id)
          .Set("status", "saved")
          .Set("scale", base::ListValue().Append(1).Append(1))
          .Set("angle", 0)
          .Set("strokeColor", "transparent")
          .Set("backgroundColor", "transparent")
          .Set("fillStyle", "solid")
          .Set("strokeWidth", 1)
          .Set("strokeStyle", "solid")
          .Set("roughness", 0)
          .Set("opacity", 100)
          .Set("seed", 1)
          .Set("version", 1)
          .Set("versionNonce", 1)
          .Set("isDeleted", false)
          .Set("groupIds", base::ListValue())
          .Set("boundElements", base::Value())
          .Set("updated", 0)
          .Set("link", base::Value())
          .Set("locked", false);
  auto serialized = base::WriteJson(
      base::DictValue()
          .Set("type", "excalidraw")
          .Set("version", 2)
          .Set("elements", base::ListValue().Append(std::move(element)))
          .Set("appState", base::DictValue())
          .Set("kryptonCapture", true));
  if (!serialized)
    return Error("Could not serialize the capture.");
  auto saved = Run(DocumentKind::kEasel, base::DictValue()
                                             .Set("operation", "save")
                                             .Set("id", *id)
                                             .Set("revision", *revision)
                                             .Set("content", *serialized));
  if (saved.FindString("error"))
    return saved;
  Run(DocumentKind::kEasel, base::DictValue()
                                .Set("operation", "rename")
                                .Set("id", *id)
                                .Set("title", "Page capture"));
  return base::DictValue().Set("id", *id);
}
void DocumentStore::Export(DocumentKind kind,
                           std::string id,
                           std::string format,
                           std::string png,
                           base::FilePath destination,
                           Reply reply) {
  runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&DocumentStore::WriteExport, base::WrapRefCounted(this),
                     kind, std::move(id), std::move(format), std::move(png),
                     std::move(destination)),
      std::move(reply));
}
base::FilePath DocumentStore::Directory(DocumentKind kind,
                                        std::string_view id) const {
  return root_.AppendASCII(kind == DocumentKind::kNote ? "notes" : "easels")
      .AppendASCII(id);
}
base::DictValue DocumentStore::Run(DocumentKind kind, base::DictValue request) {
  const auto* operation = request.FindString("operation");
  const auto* requested = request.FindString("id");
  if (!operation || !requested)
    return Error("Invalid document request.");
  const bool creating = *operation == "load" && *requested == "new";
  const std::string id =
      creating ? base::Uuid::GenerateRandomV4().AsLowercaseString()
               : *requested;
  if (!IsDocumentId(id))
    return Error("Invalid document id.");
  const auto dir = Directory(kind, id);
  if (!SafeDirectory(root_) || !SafeDirectory(dir.DirName()) ||
      base::IsLink(dir))
    return Error("Document storage is unavailable.");
  if (*operation == "list") {
    base::ListValue rows;
    base::FileEnumerator files(dir.DirName(), false,
                               base::FileEnumerator::DIRECTORIES);
    for (auto file = files.Next(); !file.empty(); file = files.Next()) {
      const std::string candidate = file.BaseName().AsUTF8Unsafe();
      if (!IsDocumentId(candidate) || base::IsLink(file) ||
          !base::PathExists(file.AppendASCII(Filename(kind))))
        continue;
      if (rows.size() == kListLimit)
        return Error("Too many documents to list; existing tabs still work.");
      rows.Append(base::DictValue()
                      .Set("id", candidate)
                      .Set("title", Title(file, kind)));
    }
    return base::DictValue().Set("documents", std::move(rows));
  }
  if (creating) {
    if (!SafeDirectory(dir) ||
        !Write(dir.AppendASCII(Filename(kind)),
               kind == DocumentKind::kNote
                   ? ""
                   : "{\"type\":\"excalidraw\",\"version\":2,\"elements\":[],"
                     "\"appState\":{}}"))
      return Error("Could not create the document.");
  }
  auto content = Read(dir.AppendASCII(Filename(kind)), kDocumentLimit);
  if (!content)
    return Error("Document is missing or exceeds the size limit.");
  const std::string revision = Revision(*content);
  if (*operation == "load") {
    base::DictValue images;
    size_t total = 0;
    const auto image_dir = dir.AppendASCII("images");
    if (base::IsLink(image_dir))
      return Error("Invalid image directory.");
    base::FileEnumerator files(image_dir, false, base::FileEnumerator::FILES);
    for (auto file = files.Next(); !file.empty(); file = files.Next()) {
      const auto key = file.RemoveExtension().BaseName().AsUTF8Unsafe();
      if (!IsDocumentId(key) || file.Extension() != FILE_PATH_LITERAL(".png"))
        continue;
      auto bytes = Read(file, kImageLimit);
      if (!bytes || (total += bytes->size()) > kTotalImagesLimit ||
          images.size() == kImageCountLimit)
        return Error("Document images exceed the size limit.");
      images.Set(key, base::Base64Encode(*bytes));
    }
    return base::DictValue()
        .Set("id", id)
        .Set("title", Title(dir, kind))
        .Set("content", *content)
        .Set("revision", revision)
        .Set("images", std::move(images));
  }
  if (*operation == "rename") {
    const auto* title = request.FindString("title");
    if (!title || title->empty() || title->size() > 512 ||
        !base::IsStringUTF8(*title) ||
        title->find_first_of("\r\n\0", 0, 3) != std::string::npos)
      return Error("Use a title of 1 to 512 UTF-8 bytes.");
    if (!Write(dir.AppendASCII("title.txt"), *title))
      return Error("Could not rename the document.");
    return base::DictValue().Set("title", *title);
  }
  if (*operation == "insertImage") {
    const auto* data = request.FindString("png");
    auto png = data ? CleanPng(*data) : std::nullopt;
    if (!png)
      return Error("Use a PNG image up to 8 MiB and 32 million pixels.");
    const auto images = dir.AppendASCII("images");
    if (!SafeDirectory(images))
      return Error("Could not create the image directory.");
    size_t total = png->size(), count = 0;
    base::FileEnumerator files(images, false, base::FileEnumerator::FILES);
    for (auto file = files.Next(); !file.empty(); file = files.Next()) {
      auto size = base::GetFileSize(file);
      if (!size || *size < 0 ||
          (total += static_cast<size_t>(*size)) > kTotalImagesLimit ||
          ++count >= kImageCountLimit)
        return Error("Document images exceed the size limit.");
    }
    const std::string image_id =
        base::Uuid::GenerateRandomV4().AsLowercaseString();
    if (!Write(images.AppendASCII(image_id + ".png"), *png))
      return Error("Could not save the image.");
    return base::DictValue()
        .Set("image", image_id)
        .Set("png", base::Base64Encode(*png));
  }
  const auto* expected = request.FindString("revision");
  if (!expected || *expected != revision)
    return Error("This document changed in another tab. Reload before saving.");
  if (*operation == "delete") {
    if (base::IsLink(dir.AppendASCII("images")))
      return Error("Invalid image directory.");
    // Tombstone the document before removing only the known image/title files.
    // Never recursively follow arbitrary directory content supplied by a page.
    if (!base::DeleteFile(dir.AppendASCII(Filename(kind))))
      return Error("Could not delete the document.");
    base::DeleteFile(dir.AppendASCII("title.txt"));
    base::FileEnumerator images(dir.AppendASCII("images"), false,
                                base::FileEnumerator::FILES);
    for (auto file = images.Next(); !file.empty(); file = images.Next()) {
      if (IsDocumentId(file.RemoveExtension().BaseName().AsUTF8Unsafe()) &&
          file.Extension() == FILE_PATH_LITERAL(".png") && !base::IsLink(file))
        base::DeleteFile(file);
    }
    return base::DictValue().Set("deleted", true);
  }
  if (*operation != "save")
    return Error("Unsupported document operation.");
  const auto* replacement = request.FindString("content");
  if (!replacement || replacement->size() > kDocumentLimit ||
      !base::IsStringUTF8(*replacement))
    return Error("Document exceeds the 8 MiB limit.");
  if (kind == DocumentKind::kEasel) {
    auto scene = base::JSONReader::ReadDict(*replacement, base::JSON_PARSE_RFC);
    if (!scene || !scene->FindList("elements") || scene->Find("files"))
      return Error("Invalid easel scene; images must use local assets.");
  }
  if (!Write(dir.AppendASCII(Filename(kind)), *replacement))
    return Error("Could not save the document.");
  return base::DictValue().Set("revision", Revision(*replacement));
}
base::DictValue DocumentStore::WriteExport(DocumentKind kind,
                                           std::string id,
                                           std::string format,
                                           std::string png,
                                           base::FilePath destination) {
  if (!IsDocumentId(id) || base::IsLink(root_) ||
      base::IsLink(Directory(kind, id)) ||
      base::IsLink(Directory(kind, id).DirName()) ||
      base::IsLink(Directory(kind, id).AppendASCII("images")))
    return Error("Invalid document.");
  const auto dir = Directory(kind, id);
  if (format == "png") {
    auto bytes = CleanPng(png);
    return bytes && Write(destination, *bytes)
               ? base::DictValue().Set("exported", true)
               : Error("Could not export the PNG.");
  }
  auto content = Read(dir.AppendASCII(Filename(kind)), kDocumentLimit);
  if (!content)
    return Error("Could not read the document.");
  // Export one self-contained scene or Markdown plus a relative asset folder.
  base::DictValue scene_files;
  const auto export_images =
      destination.RemoveExtension().AddExtensionASCII("assets");
  base::FileEnumerator images(dir.AppendASCII("images"), false,
                              base::FileEnumerator::FILES);
  size_t total = 0, count = 0;
  for (auto file = images.Next(); !file.empty(); file = images.Next()) {
    const std::string image_id =
        file.RemoveExtension().BaseName().AsUTF8Unsafe();
    if (!IsDocumentId(image_id) ||
        file.Extension() != FILE_PATH_LITERAL(".png"))
      continue;
    auto bytes = Read(file, kImageLimit);
    if (!bytes || (total += bytes->size()) > kTotalImagesLimit ||
        ++count > kImageCountLimit)
      return Error("Too many images to export.");
    if (kind == DocumentKind::kEasel) {
      scene_files.Set(image_id,
                      base::DictValue()
                          .Set("id", image_id)
                          .Set("mimeType", "image/png")
                          .Set("created", 0)
                          .Set("dataURL", "data:image/png;base64," +
                                              base::Base64Encode(*bytes)));
    } else {
      if (!SafeDirectory(export_images) ||
          !Write(export_images.Append(file.BaseName()), *bytes))
        return Error("Could not export the images.");
      base::ReplaceSubstringsAfterOffset(
          &*content, 0, "images/" + image_id + ".png",
          export_images.BaseName().AsUTF8Unsafe() + "/" + image_id + ".png");
    }
  }
  if (kind == DocumentKind::kEasel) {
    auto scene = base::JSONReader::ReadDict(*content, base::JSON_PARSE_RFC);
    if (!scene)
      return Error("Invalid scene.");
    scene->Set("files", std::move(scene_files));
    content = base::WriteJson(*scene);
  }
  return content && Write(destination, *content)
             ? base::DictValue().Set("exported", true)
             : Error("Could not export the document.");
}
}  // namespace krypton_ui
