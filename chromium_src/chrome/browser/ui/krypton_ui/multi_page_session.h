// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_MULTI_PAGE_SESSION_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_MULTI_PAGE_SESSION_H_

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace content {
class WebContents;
}
namespace krypton_ui {
inline constexpr char kSplitSessionKey[] = "krypton_ui.split";
struct SplitSessionData {
  uint64_t group = 0;
  uint32_t count = 0;
  uint32_t index = 0;
  uint32_t layout = 0;
  std::array<float, 3> weights{0.5f, 0.5f, 0.5f};
  friend bool operator==(const SplitSessionData&,
                         const SplitSessionData&) = default;
};
[[nodiscard]] std::optional<SplitSessionData> DecodeSplitSessionData(
    std::string_view encoded);
[[nodiscard]] std::string EncodeSplitSessionData(const SplitSessionData& state);
[[nodiscard]] bool IsTransientGlance(const content::WebContents* contents);
void SetTransientGlance(content::WebContents* contents,
                        bool transient,
                        content::WebContents* parent = nullptr);
[[nodiscard]] content::WebContents* GlanceSessionParent(
    const content::WebContents* contents);
[[nodiscard]] std::optional<SplitSessionData> SplitSessionForTab(
    const content::WebContents* contents);
void SetSplitSessionForTab(content::WebContents* contents,
                           std::optional<SplitSessionData> state);
void AddMultiPageSessionData(const content::WebContents* contents,
                             std::map<std::string, std::string>& extra_data);
void RestoreMultiPageSessionData(
    content::WebContents* contents,
    const std::map<std::string, std::string>& extra_data);
}  // namespace krypton_ui
#endif
