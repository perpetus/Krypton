#ifndef CHROME_BROWSER_UI_KRYPTON_UI_SIDEBAR_SERVICES_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_SIDEBAR_SERVICES_H_

#include <memory>

#include "base/functional/callback.h"
#include "chrome/browser/ui/krypton_ui/include/krypton_ui/abi.h"

class BrowserWindowInterface;
namespace tabs {
class TabInterface;
}
namespace ui {
class MenuModel;
}
namespace krypton_ui {
class WindowSession;
class SidebarServices {
 public:
  using TabIdentity = base::RepeatingCallback<KrTabId(tabs::TabInterface&)>;
  SidebarServices(BrowserWindowInterface&, WindowSession&, TabIdentity);
  ~SidebarServices();
  SidebarServices(const SidebarServices&) = delete;
  SidebarServices& operator=(const SidebarServices&) = delete;
  void Initialize();
  void DownloadCommand(KrDownloadId id, uint32_t command);
  void MediaCommand(KrMediaId id, uint32_t command);
  void MediaSeek(KrMediaId id, double seconds);
  void LibraryVisibility(uint32_t section, bool visible);
  void LibraryCommand(uint32_t section, KrLibraryItemId id, uint32_t command);
  std::unique_ptr<ui::MenuModel> CreateMenu(uint32_t kind, uint64_t target);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace krypton_ui
#endif
