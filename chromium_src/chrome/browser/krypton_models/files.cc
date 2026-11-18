// Copyright 2026 The Krypton Authors. GPL-3.0.
#include "chrome/browser/krypton_models/files.h"
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/files/memory_mapped_file.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "crypto/hash.h"
namespace krypton::models {
bool VerifyFile(const base::FilePath& path,
                uint64_t size,
                std::string_view sha256,
                uint64_t maximum_size) {
  base::MemoryMappedFile map;
  if (!size || size > maximum_size || base::IsLink(path) ||
      base::IsLink(path.DirName()) || !map.Initialize(path) ||
      map.length() != size)
    return false;
  const auto hash = crypto::hash::Sha256(base::span(map.data(), map.length()));
  return base::EqualsCaseInsensitiveASCII(base::HexEncode(hash), sha256);
}
base::FilePath Child(const base::FilePath& root, std::string_view relative) {
  const auto path = base::FilePath::FromUTF8Unsafe(relative);
  return path.IsAbsolute() || path.ReferencesParent() ||
                 relative.find(':') != relative.npos
             ? base::FilePath()
             : root.Append(path);
}
}  // namespace krypton::models
