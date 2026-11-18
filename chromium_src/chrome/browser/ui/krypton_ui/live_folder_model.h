// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_LIVE_FOLDER_MODEL_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_LIVE_FOLDER_MODEL_H_

#include "base/types/expected.h"
#include "base/values.h"
#include "krypton_ui/abi.h"
#include "url/gurl.h"
#include <string>
#include <vector>

namespace krypton_ui {
inline constexpr char kLiveFolderCachePref[] = "krypton_ui.live_folder_cache";
inline constexpr size_t kMaxLiveFolders = 16;
inline constexpr size_t kMaxLiveItems = 100;
inline constexpr size_t kMaxLiveResponseBytes = 1024 * 1024;
// Matched by KryptonUI to offer GitHub sign-in; keep the text identical.
inline constexpr char kLiveFolderGithubSignedOut[] = "Not signed in to GitHub.";
struct LiveFolderConfig {
  uint32_t kind = 0;
  std::string source;
  uint32_t interval_minutes = 30;
  uint32_t max_items = 20;
  bool operator==(const LiveFolderConfig &) const = default;
  bool enabled() const { return kind != 0; }
};
struct LiveFolderItem {
  std::string id;
  std::string title;
  std::string url;
  bool operator==(const LiveFolderItem &) const = default;
};
using LiveFolderItems = std::vector<LiveFolderItem>;
using LiveFolderResult = base::expected<LiveFolderItems, std::string>;
bool ValidLiveFolderConfig(const LiveFolderConfig &config);
GURL LiveFolderRequestUrl(const LiveFolderConfig &config);
LiveFolderConfig ReadLiveFolderConfig(const base::DictValue &row);
void WriteLiveFolderConfig(base::DictValue &row,
                           const LiveFolderConfig &config);
// XML syntax is decoded by the sandboxed Data Decoder. Mapping and GitHub
// response parsing run on a ThreadPool worker, never on the browser UI thread.
LiveFolderResult ParseLiveFeed(base::Value tree, GURL base_url,
                               LiveFolderConfig config);
LiveFolderResult ParseLiveGithub(std::string body, LiveFolderConfig config);
// Classifies a bounded non-200 GitHub body. It never yields list items.
LiveFolderResult ParseLiveGithubError(int status, std::string body);
} // namespace krypton_ui
#endif
