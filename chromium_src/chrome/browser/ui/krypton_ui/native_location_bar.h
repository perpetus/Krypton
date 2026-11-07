// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_NATIVE_LOCATION_BAR_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_NATIVE_LOCATION_BAR_H_

#include <cstdint>
#include <memory>
#include <string>

#include "base/containers/flat_map.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/types/expected.h"
#include "chrome/browser/ui/location_bar/location_bar.h"
#include "components/security_state/core/security_state.h"
#include "krypton_ui/abi.h"

class BrowserWindowInterface;
class OmniboxController;
namespace ui {
class ImageModel;
}
namespace views {
class View;
}

namespace krypton_ui {
class ControlAnchors;
class WindowSession;

// Chromium owns classification, search, keyword policy, autocomplete and
// navigation. Only editing and suggestion presentation cross the C ABI.
// Initialize after installing the owning browser adapter in BrowserView.
class NativeLocationBar final : public LocationBar {
 public:
  NativeLocationBar(
      BrowserWindowInterface& browser,
      CommandUpdater& commands,
      LocationBarModel& model,
      WindowSession& session,
      ControlAnchors& anchors,
      views::View& host,
      base::RepeatingCallback<
          base::expected<KrImageId, std::string>(const ui::ImageModel&)> images,
      base::RepeatingCallback<void(KrImageId)> release_image,
      base::RepeatingClosure changed);
  ~NativeLocationBar() override;
  NativeLocationBar(const NativeLocationBar&) = delete;
  NativeLocationBar& operator=(const NativeLocationBar&) = delete;

  void Initialize();
  void Close();
  void OnNativeState(const KrAddressState& state);
  void Query(uint32_t request, std::string_view text);
  void CancelQuery(uint32_t request);
  void SelectSuggestion(uint32_t request, uint64_t generation, size_t index);
  bool HandleKey(uint32_t key, uint32_t modifiers);
  // Capture before native dismissal cancels the query. Closures own Chromium's
  // input/result snapshot and revalidate the original tab before navigating.
  [[nodiscard]] base::OnceClosure CaptureSuggestion(
      uint32_t request,
      uint64_t generation,
      size_t index,
      WindowOpenDisposition disposition);
  [[nodiscard]] base::OnceClosure CaptureNavigation(
      std::string_view text,
      WindowOpenDisposition disposition);
  void OnAnchorChanged();

  void FocusLocation(bool user_initiated, bool clear_focus_if_failed) override;
  void FocusSearch() override;
  void UpdateFocusBehavior(bool toolbar_visible) override;
  void UpdateContentSettingsIcons() override;
  void SaveStateToContents(content::WebContents* contents) override;
  void Revert() override;
  OmniboxView* GetOmniboxView() override;
  OmniboxPopupView* GetOmniboxPopupView() override;
  OmniboxController* GetOmniboxController() override;
  bool ShouldCloseOmniboxPopup(ui::MouseEvent* event) override;
  content::WebContents* GetWebContents() override;
  LocationBarModel* GetLocationBarModel() override;
  std::optional<bubble_anchor_util::AnchorConfiguration> GetChipAnchor()
      override;
  ChipController* GetChipController() override;
  void AnnounceAlert(const std::u16string& announcement) override;
  void OnChanged() override;
  void UpdateWithoutTabRestore() override;
  ui::TrackedElement* GetAnchorOrNull() override;
  BrowserWindowInterface* GetBrowser() override;
  Profile* GetProfile() override;
  bool IsInitialized() const override;
  bool IsVisible() const override;
  bool IsDrawn() const override;
  bool IsFullscreen() const override;
  bool IsEditingOrEmpty() const override;
  bool IsMouseHovered() const override;
  bool IsFocusWithin() const override;
  void InvalidateLayout() override;
  gfx::Rect Bounds() const override;
  gfx::Rect BoundsInScreen() const override;
  gfx::Size MinimumSize() const override;
  gfx::Size PreferredSize() const override;
  void Update(content::WebContents* contents) override;
  void ResetTabState(content::WebContents* contents) override;
  bool HasSecurityStateChanged() override;
  LocationBarTesting* GetLocationBarForTesting() override;

 private:
  class Editor;
  class Popup;
  struct Snapshot;
  void PublishEdit();
  void PublishSuggestions();
  void ScheduleSuggestions();
  void PublishSelection();

  const raw_ref<BrowserWindowInterface> browser_;
  const raw_ref<LocationBarModel> location_model_;
  const raw_ref<WindowSession> session_;
  const raw_ref<ControlAnchors> anchors_;
  const raw_ref<views::View> host_;
  const base::RepeatingClosure changed_;
  const base::RepeatingCallback<base::expected<KrImageId, std::string>(
      const ui::ImageModel&)>
      images_;
  const base::RepeatingCallback<void(KrImageId)> release_image_;
  bool suggestions_pending_ = false;
  std::unique_ptr<OmniboxController> controller_;
  std::unique_ptr<Editor> editor_;
  std::unique_ptr<Popup> popup_;
  base::flat_map<uint64_t, std::unique_ptr<Snapshot>> snapshots_;
  uint32_t request_ = 0;
  uint64_t generation_ = 0;
  uint64_t edit_revision_ = 0;
  bool open_ = false;
  bool focused_ = false;
  bool query_started_ = false;
  bool initial_steady_ = false, accept_control_ = false;
  size_t visible_result_count_ = 0;
  bool toolbar_visible_ = true;
  security_state::SecurityLevel security_level_ = security_state::NONE;
  base::WeakPtrFactory<NativeLocationBar> weak_factory_{this};
};
}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_NATIVE_LOCATION_BAR_H_
