// Copyright 2026 The Krypton Authors. GPL-3.0.
#ifndef CHROME_BROWSER_KRYPTON_MODELS_FILES_H_
#define CHROME_BROWSER_KRYPTON_MODELS_FILES_H_
#include <cstdint>
#include <string_view>
#include "base/files/file_path.h"
namespace krypton::models {
// Worker-only helpers shared by the translation and speech pack installers.
// Hash verification never causes a network request or creates a directory.
bool VerifyFile(const base::FilePath& path,
                uint64_t size,
                std::string_view sha256,
                uint64_t maximum_size);
base::FilePath Child(const base::FilePath& root, std::string_view relative);
}  // namespace krypton::models
#endif
