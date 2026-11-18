// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/download/download_prefs.h"
#include "chrome/browser/media/webrtc/capture_policy_utils.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/krypton_ui/document_store.h"
#include "chrome/browser/ui/krypton_ui/document_webui.h"
#include "chrome/browser/ui/krypton_ui/page_capture.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/sharing_hub/sharing_hub_window_controller.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/grit/generated_resources.h"
#include "components/download/public/common/download_url_parameters.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/download_request_utils.h"
#include "content/public/browser/web_contents.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/image/image.h"
#include "ui/native_theme/native_theme.h"
#include "url/url_constants.h"

namespace krypton_ui {
namespace {
using NotifyCallback = base::RepeatingCallback<void(std::string_view)>;
constexpr char kSaveError[] = "Could not save the screenshot.";

constexpr net::NetworkTrafficAnnotationTag kScreenshotSave =
    net::DefineNetworkTrafficAnnotation("krypton_screenshot_save", R"(
      semantics {
        sender: "Krypton screenshots"
        description:
          "Saves a screenshot the user selected on the current page. The image "
          "is passed to the download system as a local data URL."
        trigger: "The user chooses Download in the screenshot overlay."
        data: "An image of part of the current page."
        destination: LOCAL
      }
      policy {
        cookies_allowed: NO
        setting: "Screenshots are taken only when the user requests one."
        policy_exception_justification:
          "No network request is made; the data URL is local."
      })");

struct EncodedScreenshot {
  std::vector<uint8_t> png;
  // Empty when the image is too large for a URL.
  std::string data_url;
};

std::optional<EncodedScreenshot> EncodeScreenshot(SkBitmap bitmap) {
  auto png = gfx::PNGCodec::EncodeBGRASkBitmap(bitmap,
                                               /*discard_transparency=*/false);
  if (!png)
    return std::nullopt;
  EncodedScreenshot encoded;
  std::string data_url =
      base::StrCat({"data:image/png;base64,", base::Base64Encode(*png)});
  if (data_url.size() <= url::kMaxURLChars)
    encoded.data_url = std::move(data_url);
  encoded.png = std::move(*png);
  return encoded;
}

base::FilePath WriteScreenshot(base::FilePath path, std::vector<uint8_t> png) {
  if (!base::CreateDirectory(path.DirName()))
    return {};
  path = base::GetUniquePath(path);
  return !path.empty() && base::WriteFile(path, png) ? path : base::FilePath();
}

std::string ScreenshotName(const GURL& url) {
  std::string host(url.GetHost());
  // Hosts such as IPv6 literals contain characters Windows rejects.
  for (char& c : host) {
    if (!base::IsAsciiAlphaNumeric(c) && c != '.' && c != '-')
      c = '_';
  }
  return base::StrCat({"krypton_screenshot_", host, ".png"});
}

void CopyScreenshot(const gfx::Image& image, const NotifyCallback& notify) {
  const SkBitmap bitmap = image.AsBitmap();
  if (bitmap.drawsNothing())
    return;
  {
    ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
    writer.WriteImage(bitmap);
  }
  notify.Run("Screenshot copied to the clipboard");
}

void SaveScreenshot(base::WeakPtr<tabs::TabInterface> tab,
                    GURL url,
                    NotifyCallback notify,
                    std::optional<EncodedScreenshot> encoded) {
  if (!tab)
    return;
  if (!encoded) {
    notify.Run(kSaveError);
    return;
  }
  content::WebContents* contents = tab->GetContents();
  auto* prefs =
      DownloadPrefs::FromBrowserContext(contents->GetBrowserContext());
  if (prefs->download_restriction() == policy::DownloadRestriction::ALL_FILES) {
    notify.Run("Downloads are blocked by your administrator.");
    return;
  }
  const std::string name = ScreenshotName(url);
  if (!encoded->data_url.empty()) {
    // The download system lists the file with other downloads and follows the
    // user's save-location preference.
    auto params =
        content::DownloadRequestUtils::CreateDownloadForWebContentsMainFrame(
            contents, GURL(encoded->data_url), kScreenshotSave);
    params->set_suggested_name(base::UTF8ToUTF16(name));
    contents->GetBrowserContext()->GetDownloadManager()->DownloadUrl(
        std::move(params));
    return;
  }
  // Images beyond the URL limit go straight to the download folder.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
      base::BindOnce(&WriteScreenshot, prefs->DownloadPath().AppendASCII(name),
                     std::move(encoded->png)),
      base::BindOnce(
          [](NotifyCallback notify, base::FilePath path) {
            notify.Run(path.empty() ? kSaveError
                                    : "Screenshot saved to Downloads");
          },
          std::move(notify)));
}

void DownloadScreenshot(base::WeakPtr<tabs::TabInterface> tab,
                        GURL url,
                        const gfx::Image& image,
                        NotifyCallback notify) {
  // PNG and Base64 encoding of a large selection stay off the UI thread.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&EncodeScreenshot, image.AsBitmap()),
      base::BindOnce(&SaveScreenshot, std::move(tab), std::move(url),
                     std::move(notify)));
}
}  // namespace

void BrowserAdapter::Notify(std::string_view text) {
  if (const auto* api = session_.api();
      api && KR_HAS_SLOT(api, KrShellUI, show_notification) &&
      api->show_notification)
    api->show_notification(api->self, {text.data(), text.size()});
}

bool BrowserAdapter::OpenCommandLauncher() {
  const auto* api = session_.api();
  if (closed_ || !browser_initialized_ || !presented_ || !api ||
      !KR_HAS_SLOT(api, KrShellUI, open_launcher) || !api->open_launcher)
    return false;
  api->open_launcher(api->self);
  return true;
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

void BrowserAdapter::TakeScreenshot(tabs::TabInterface* tab,
                                    PageCaptureMode mode,
                                    bool add_to_easel) {
  if (!tab || browser_->GetActiveTabInterface() != tab || !CanTakeScreenshot())
    return;
  screenshot_ = std::make_unique<PageCapture>(*tab);
  const GURL url = tab->GetContents()->GetLastCommittedURL();
  const auto scheme = ThemeScheme();
  const bool dark =
      scheme == KR_THEME_DARK ||
      (scheme == KR_THEME_SYSTEM &&
       ui::NativeTheme::GetInstanceForNativeUi()->preferred_color_scheme() ==
           ui::NativeTheme::PreferredColorScheme::kDark);
  screenshot_->Start(
      mode, PageCaptureOptions{.add_to_easel = add_to_easel, .dark = dark},
      base::BindOnce(
          [](base::WeakPtr<BrowserAdapter> owner,
             base::WeakPtr<tabs::TabInterface> tab, GURL url, gfx::Image image,
             std::string error, PageCaptureAction action) {
            if (!owner)
              return;
            owner->screenshot_.reset();
            if (!tab || owner->browser_->GetActiveTabInterface() != tab.get() ||
                tab->GetContents()->GetLastCommittedURL() != url)
              return;
            if (!error.empty()) {
              owner->Notify(error);
              return;
            }
            if (image.IsEmpty())
              return;
            auto* contents = tab->GetContents();
            if (capture_policy::GetAllowedCaptureLevel(url, contents) ==
                AllowedScreenCaptureLevel::kDisallowed)
              return;
            const auto notify =
                base::BindRepeating(&BrowserAdapter::Notify, owner);
            if (action == PageCaptureAction::kCopy) {
              CopyScreenshot(image, notify);
              return;
            }
            if (action == PageCaptureAction::kDownload) {
              DownloadScreenshot(tab, url, image, notify);
              return;
            }
            if (action == PageCaptureAction::kEasel) {
              if (!owner->CanCreateDocument())
                return;
              auto png = image.As1xPNGBytes();
              if (!png)
                return;
              auto store =
                  DocumentStore::ForProfile(owner->browser_->GetProfile());
              store->CreateCapture(
                  base::Base64Encode(base::span(*png)),
                  base::BindOnce(
                      [](base::WeakPtr<BrowserAdapter> owner,
                         base::DictValue result) {
                        if (!owner || !owner->CanCreateDocument())
                          return;
                        if (const auto* error = result.FindString("error")) {
                          owner->Notify(*error);
                          return;
                        }
                        const auto* id = result.FindString("id");
                        if (!id || !IsDocumentId(*id))
                          return;
                        NavigateParams params(
                            &*owner->browser_, GURL("chrome://easel/" + *id),
                            ui::PAGE_TRANSITION_AUTO_BOOKMARK);
                        params.disposition =
                            WindowOpenDisposition::kNewForegroundTab;
                        Navigate(&params);
                      },
                      owner));
              return;
            }
            // Visible and full-page captures open the preview, which offers
            // copy, download and editing.
            if (auto* controller =
                    sharing_hub::SharingHubWindowController::From(
                        &*owner->browser_))
              controller->ShowScreenshotCapturedBubble(contents, image);
          },
          weak_factory_.GetWeakPtr(), tab->GetWeakPtr(), url));
}
bool BrowserAdapter::CanCreateDocument() const {
  return !closed_ && browser_initialized_ && presented_ &&
         browser_->GetProfile()->IsRegularProfile() &&
         !browser_->GetProfile()->IsOffTheRecord();
}
void BrowserAdapter::NewDocument(DocumentKind kind) {
  if (!CanCreateDocument())
    return;
  NavigateParams params(
      &*browser_,
      GURL(kind == DocumentKind::kNote ? "chrome://notes/new"
                                       : "chrome://easel/new"),
      ui::PAGE_TRANSITION_AUTO_BOOKMARK);
  params.disposition = WindowOpenDisposition::kNewForegroundTab;
  Navigate(&params);
}
void BrowserAdapter::UpdateDocumentPageTheme(content::WebContents* contents) {
  if (!contents || (contents->GetLastCommittedURL().host() != "notes" &&
                    contents->GetLastCommittedURL().host() != "easel"))
    return;
  const auto scheme = ThemeScheme();
  SetDocumentPageTheme(contents,
                       scheme == KR_THEME_DARK ||
                           (scheme == KR_THEME_SYSTEM &&
                            ui::NativeTheme::GetInstanceForNativeUi()
                                    ->preferred_color_scheme() ==
                                ui::NativeTheme::PreferredColorScheme::kDark));
}
}  // namespace krypton_ui
