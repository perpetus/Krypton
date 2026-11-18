// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/live_folder_session.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"
#include <algorithm>
namespace krypton_ui {
namespace {
class LiveItemData final : public content::WebContentsUserData<LiveItemData> {
public:
  LiveFolderTabState state;

private:
  friend class content::WebContentsUserData<LiveItemData>;
  explicit LiveItemData(content::WebContents *contents)
      : content::WebContentsUserData<LiveItemData>(*contents) {}
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};
WEB_CONTENTS_USER_DATA_KEY_IMPL(LiveItemData);
} // namespace
LiveFolderTabState LiveFolderStateForTab(const content::WebContents *contents) {
  const auto *data = LiveItemData::FromWebContents(contents);
  return data ? data->state : LiveFolderTabState();
}
std::string EncodeLiveFolderTabState(const LiveFolderTabState &state) {
  return state.folder ? base::NumberToString(state.folder) + ":" + state.item +
                            (state.accessed ? ":1" : ":0")
                      : std::string();
}
void SetLiveFolderTabState(content::WebContents *contents,
                           LiveFolderTabState state) {
  if (!state.folder) {
    contents->RemoveUserData(LiveItemData::UserDataKey());
    return;
  }
  LiveItemData::CreateForWebContents(contents);
  LiveItemData::FromWebContents(contents)->state = std::move(state);
}
void AddLiveFolderSessionData(const content::WebContents *contents,
                              std::map<std::string, std::string> &extra_data) {
  const auto value = EncodeLiveFolderTabState(LiveFolderStateForTab(contents));
  if (value.empty())
    extra_data.erase(kLiveFolderItemSessionKey);
  else
    extra_data[kLiveFolderItemSessionKey] = value;
}
void RestoreLiveFolderSessionData(
    content::WebContents *contents,
    const std::map<std::string, std::string> &extra_data) {
  const auto found = extra_data.find(kLiveFolderItemSessionKey);
  if (found == extra_data.end() || found->second.size() > 88)
    return;
  const std::string_view text(found->second);
  const auto colon = text.find(':');
  if (colon == std::string_view::npos || text.size() != colon + 67)
    return;
  uint64_t folder = 0;
  const auto item = text.substr(colon + 1, 64);
  if (!base::StringToUint64(text.substr(0, colon), &folder) || !folder ||
      !std::ranges::all_of(item, [](char c) { return base::IsHexDigit(c); }) ||
      text[colon + 65] != ':' || (text.back() != '0' && text.back() != '1'))
    return;
  SetLiveFolderTabState(contents,
                        {folder, std::string(item), text.back() == '1'});
}
} // namespace krypton_ui
