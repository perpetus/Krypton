// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/workspace_session.h"
#include "chrome/browser/ui/krypton_ui/multi_page_session.h"

#include "base/strings/string_number_conversions.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"

namespace krypton_ui {
namespace {
class WorkspaceTabData : public content::WebContentsUserData<WorkspaceTabData> {
 public:
  uint64_t id = 0;
  uint64_t essential = 0;
  uint64_t folder = 0;
  std::string icon;

 private:
  friend class content::WebContentsUserData<WorkspaceTabData>;
  explicit WorkspaceTabData(content::WebContents* contents)
      : content::WebContentsUserData<WorkspaceTabData>(*contents) {}
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};
WEB_CONTENTS_USER_DATA_KEY_IMPL(WorkspaceTabData);
}  // namespace
std::string TabIconForTab(const content::WebContents* contents) {
  const auto* data = WorkspaceTabData::FromWebContents(contents);
  return data ? data->icon : std::string();
}
void SetTabIconForTab(content::WebContents* contents, std::string_view glyph) {
  WorkspaceTabData::CreateForWebContents(contents);
  WorkspaceTabData::FromWebContents(contents)->icon = glyph;
}
uint64_t ParseWorkspaceId(std::string_view value) {
  uint64_t id = 0;
  return base::StringToUint64(value, &id) ? id : 0;
}
uint64_t WorkspaceForTab(const content::WebContents* contents) {
  const auto* data = WorkspaceTabData::FromWebContents(contents);
  return data ? data->id : 0;
}
void SetWorkspaceForTab(content::WebContents* contents, uint64_t id) {
  WorkspaceTabData::CreateForWebContents(contents);
  WorkspaceTabData::FromWebContents(contents)->id = id;
}
uint64_t EssentialForTab(const content::WebContents* contents) {
  const auto* data = WorkspaceTabData::FromWebContents(contents);
  return data ? data->essential : 0;
}
uint64_t FolderForTab(const content::WebContents* contents) {
  const auto* data = WorkspaceTabData::FromWebContents(contents);
  return data ? data->folder : 0;
}
void SetFolderForTab(content::WebContents* contents, uint64_t id) {
  WorkspaceTabData::CreateForWebContents(contents);
  WorkspaceTabData::FromWebContents(contents)->folder = id;
}
void SetEssentialForTab(content::WebContents* contents, uint64_t id) {
  WorkspaceTabData::CreateForWebContents(contents);
  WorkspaceTabData::FromWebContents(contents)->essential = id;
}
WorkspaceTabSessionState DecodeWorkspaceSessionData(
    const std::map<std::string, std::string>& extra_data) {
  const auto read = [&](const char* key) {
    const auto found = extra_data.find(key);
    return found == extra_data.end() ? uint64_t{0}
                                     : ParseWorkspaceId(found->second);
  };
  return {read(kWorkspaceSessionKey), read(kEssentialSessionKey),
          read(kFolderSessionKey)};
}
void EncodeWorkspaceSessionData(
    WorkspaceTabSessionState state,
    std::map<std::string, std::string>& extra_data) {
  if (state.workspace)
    extra_data[kWorkspaceSessionKey] = base::NumberToString(state.workspace);
  else
    extra_data.erase(kWorkspaceSessionKey);
  extra_data[kEssentialSessionKey] = base::NumberToString(state.essential);
  if (state.folder)
    extra_data[kFolderSessionKey] = base::NumberToString(state.folder);
  else
    extra_data.erase(kFolderSessionKey);
}
void AddWorkspaceSessionData(const content::WebContents* contents,
                             std::map<std::string, std::string>& extra_data) {
  AddMultiPageSessionData(contents, extra_data);
  const auto icon = TabIconForTab(contents);
  if (icon.empty())
    extra_data.erase(kTabIconSessionKey);
  else
    extra_data[kTabIconSessionKey] = icon;
  EncodeWorkspaceSessionData(
      {WorkspaceForTab(contents), EssentialForTab(contents),
       FolderForTab(contents)},
      extra_data);
}
void RestoreWorkspaceForTab(
    content::WebContents* contents,
    const std::map<std::string, std::string>& extra_data) {
  RestoreMultiPageSessionData(contents, extra_data);
  if (const auto found = extra_data.find(kTabIconSessionKey);
      found != extra_data.end() && found->second.size() <= 128)
    SetTabIconForTab(contents, found->second);
  const auto state = DecodeWorkspaceSessionData(extra_data);
  SetWorkspaceForTab(contents, state.workspace);
  SetEssentialForTab(contents, state.essential);
  SetFolderForTab(contents, state.folder);
}
}  // namespace krypton_ui
