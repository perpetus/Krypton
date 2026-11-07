#include "chrome/browser/ui/krypton_ui/sidebar_services.h"

#include <utility>

#include "chrome/browser/ui/krypton_ui/sidebar_downloads.h"
#include "chrome/browser/ui/krypton_ui/sidebar_library.h"
#include "chrome/browser/ui/krypton_ui/sidebar_media.h"
#include "ui/base/models/menu_model.h"

namespace krypton_ui {
class SidebarServices::Impl {
 public:
  Impl(BrowserWindowInterface& browser,
       WindowSession& session,
       TabIdentity identity)
      : downloads(browser, session),
        media(browser, session, std::move(identity)),
        library(browser, session, downloads) {}
  SidebarDownloads downloads;
  SidebarMedia media;
  SidebarLibrary library;
  bool initialized = false;
};
SidebarServices::SidebarServices(BrowserWindowInterface& browser,
                                 WindowSession& session,
                                 TabIdentity identity)
    : impl_(std::make_unique<Impl>(browser, session, std::move(identity))) {}
SidebarServices::~SidebarServices() = default;
void SidebarServices::Initialize() {
  if (impl_->initialized)
    return;
  impl_->initialized = true;
  impl_->library.Initialize();
  impl_->downloads.Initialize();
  impl_->media.Initialize();
}
void SidebarServices::DownloadCommand(KrDownloadId id, uint32_t command) {
  impl_->downloads.Command(id, command);
}
void SidebarServices::MediaCommand(KrMediaId id, uint32_t command) {
  impl_->media.Command(id, command);
}
void SidebarServices::MediaSeek(KrMediaId id, double seconds) {
  impl_->media.Seek(id, seconds);
}
void SidebarServices::LibraryVisibility(uint32_t section, bool visible) {
  impl_->library.Visibility(section, visible);
}
void SidebarServices::LibraryCommand(uint32_t section,
                                     KrLibraryItemId id,
                                     uint32_t command) {
  impl_->library.Command(section, id, command);
}
std::unique_ptr<ui::MenuModel> SidebarServices::CreateMenu(uint32_t kind,
                                                           uint64_t target) {
  if (kind == KR_MENU_DOWNLOAD)
    return impl_->downloads.CreateMenu(target);
  if (kind == KR_MENU_LIBRARY_ITEM)
    return impl_->library.CreateMenu(target);
  return nullptr;
}
}  // namespace krypton_ui
