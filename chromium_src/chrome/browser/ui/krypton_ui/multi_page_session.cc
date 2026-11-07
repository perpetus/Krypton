// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/multi_page_session.h"

#include <cmath>

#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"
#include "krypton_ui/abi.h"

namespace krypton_ui {
namespace {
class MultiPageTabData : public content::WebContentsUserData<MultiPageTabData> {
 public:
  bool transient = false;
  base::WeakPtr<content::WebContents> parent;
  std::optional<SplitSessionData> split;

 private:
  friend content::WebContentsUserData<MultiPageTabData>;
  explicit MultiPageTabData(content::WebContents* contents)
      : content::WebContentsUserData<MultiPageTabData>(*contents) {}
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};
WEB_CONTENTS_USER_DATA_KEY_IMPL(MultiPageTabData);
bool Valid(const SplitSessionData& state) {
  if (!state.group || state.count < 2 || state.count > 4 ||
      state.index >= state.count || state.layout > KR_SPLIT_MAIN_BOTTOM)
    return false;
  for (const float weight : state.weights) {
    if (!std::isfinite(weight) || weight < 0.07f || weight > 0.93f)
      return false;
  }
  return true;
}
}  // namespace

std::optional<SplitSessionData> DecodeSplitSessionData(
    std::string_view encoded) {
  // Bound untrusted session metadata before allocating a vector of fields.
  if (encoded.size() > 256)
    return std::nullopt;
  const auto fields = base::SplitStringPiece(
      encoded, ";", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  SplitSessionData state;
  if (fields.size() != 8 || fields[0] != "1" ||
      !base::StringToUint64(fields[1], &state.group) ||
      !base::StringToUint(fields[2], &state.count) ||
      !base::StringToUint(fields[3], &state.index) ||
      !base::StringToUint(fields[4], &state.layout))
    return std::nullopt;
  for (size_t i = 0; i < state.weights.size(); ++i) {
    double value = 0;
    if (!base::StringToDouble(fields[i + 5], &value) || !std::isfinite(value))
      return std::nullopt;
    state.weights[i] = static_cast<float>(value);
  }
  return Valid(state) ? std::optional(state) : std::nullopt;
}
std::string EncodeSplitSessionData(const SplitSessionData& state) {
  if (!Valid(state))
    return {};
  std::string encoded = "1;" + base::NumberToString(state.group) + ";" +
                        base::NumberToString(state.count) + ";" +
                        base::NumberToString(state.index) + ";" +
                        base::NumberToString(state.layout);
  for (const auto weight : state.weights)
    encoded += ";" + base::NumberToString(weight);
  return encoded;
}
bool IsTransientGlance(const content::WebContents* contents) {
  if (!contents)
    return false;
  const auto* data = MultiPageTabData::FromWebContents(contents);
  return data && data->transient;
}
void SetTransientGlance(content::WebContents* contents,
                        bool transient,
                        content::WebContents* parent) {
  MultiPageTabData::CreateForWebContents(contents);
  auto* data = MultiPageTabData::FromWebContents(contents);
  data->transient = transient;
  data->parent = transient && parent ? parent->GetWeakPtr() : nullptr;
}
content::WebContents* GlanceSessionParent(
    const content::WebContents* contents) {
  if (!contents)
    return nullptr;
  const auto* data = MultiPageTabData::FromWebContents(contents);
  return data ? data->parent.get() : nullptr;
}
std::optional<SplitSessionData> SplitSessionForTab(
    const content::WebContents* contents) {
  if (!contents)
    return std::nullopt;
  const auto* data = MultiPageTabData::FromWebContents(contents);
  return data ? data->split : std::nullopt;
}
void SetSplitSessionForTab(content::WebContents* contents,
                           std::optional<SplitSessionData> state) {
  MultiPageTabData::CreateForWebContents(contents);
  MultiPageTabData::FromWebContents(contents)->split = state;
}
void AddMultiPageSessionData(const content::WebContents* contents,
                             std::map<std::string, std::string>& extra_data) {
  if (const auto state = SplitSessionForTab(contents))
    extra_data[kSplitSessionKey] = EncodeSplitSessionData(*state);
  else
    extra_data.erase(kSplitSessionKey);
}
void RestoreMultiPageSessionData(
    content::WebContents* contents,
    const std::map<std::string, std::string>& extra_data) {
  const auto value = extra_data.find(kSplitSessionKey);
  SetSplitSessionForTab(contents, value == extra_data.end()
                                      ? std::nullopt
                                      : DecodeSplitSessionData(value->second));
}
}  // namespace krypton_ui
