// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"

#include "base/functional/bind.h"
#include "chrome/browser/image_editor/screenshot_flow.h"
#include "chrome/browser/media/webrtc/capture_policy_utils.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/sharing_hub/sharing_hub_window_controller.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/grit/generated_resources.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/l10n/l10n_util.h"

namespace krypton_ui {
void BrowserAdapter::Notify(std::string_view text) {
  if (const auto* api = session_.api();
      api && KR_HAS_SLOT(api, KrShellUI, show_notification) &&
      api->show_notification)
    api->show_notification(api->self, {text.data(), text.size()});
}

void BrowserAdapter::CopyCurrentLink() {
  auto* contents = browser_->GetTabStripModel()->GetActiveWebContents();
  if (!contents || !chrome::CanCopyUrl(&*browser_))
    return;
  chrome::CopyURLs({contents});
  Notify(l10n_util::GetStringUTF8(IDS_LINK_COPIED_TOAST_BODY));
}

bool BrowserAdapter::CanTakeScreenshot() const {
  auto* tab = browser_->GetActiveTabInterface();
  if (!tab || screenshot_)
    return false;
  auto* contents = tab->GetContents();
  return contents && !contents->IsCrashed() && contents->GetNativeView() &&
         contents->GetVisibleURL().SchemeIsHTTPOrHTTPS() &&
         sharing_hub::SharingHubWindowController::From(&*browser_) &&
         capture_policy::GetAllowedCaptureLevel(contents->GetLastCommittedURL(),
                                                contents) !=
             AllowedScreenCaptureLevel::kDisallowed;
}

void BrowserAdapter::TakeScreenshot(tabs::TabInterface* tab) {
  if (!tab || browser_->GetActiveTabInterface() != tab || !CanTakeScreenshot())
    return;
  screenshot_ =
      std::make_unique<image_editor::ScreenshotFlow>(tab->GetContents());
  screenshot_->Start(base::BindOnce(
      [](base::WeakPtr<BrowserAdapter> owner,
         base::WeakPtr<tabs::TabInterface> tab,
         const image_editor::ScreenshotCaptureResult& result) {
        if (!owner)
          return;
        // Release the flow after its own input/navigation callback unwinds.
        std::ignore = owner->session_.Defer(base::BindOnce(
            [](base::WeakPtr<BrowserAdapter> owner,
               base::WeakPtr<tabs::TabInterface> tab,
               image_editor::ScreenshotCaptureResult result) {
              if (!owner)
                return;
              owner->screenshot_.reset();
              if (!tab ||
                  owner->browser_->GetActiveTabInterface() != tab.get() ||
                  result.result_code !=
                      image_editor::ScreenshotCaptureResultCode::SUCCESS ||
                  result.image.IsEmpty())
                return;
              auto* contents = tab->GetContents();
              if (capture_policy::GetAllowedCaptureLevel(
                      contents->GetLastCommittedURL(), contents) ==
                  AllowedScreenCaptureLevel::kDisallowed)
                return;
              if (auto* controller =
                      sharing_hub::SharingHubWindowController::From(
                          &*owner->browser_))
                controller->ShowScreenshotCapturedBubble(contents,
                                                         result.image);
            },
            owner, tab, result));
      },
      weak_factory_.GetWeakPtr(), tab->GetWeakPtr()));
}
}  // namespace krypton_ui
