#ifndef CHROME_BROWSER_UI_KRYPTON_UI_SIDEBAR_LIBRARY_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_SIDEBAR_LIBRARY_H_

#include <cstdint>
#include <memory>

class BrowserWindowInterface;
namespace ui {
class MenuModel;
}
namespace krypton_ui {
class WindowSession;
class SidebarDownloads;
class SidebarLibrary {
 public:
  SidebarLibrary(BrowserWindowInterface& browser,
                 WindowSession& session,
                 SidebarDownloads& downloads);
  ~SidebarLibrary();
  SidebarLibrary(const SidebarLibrary&) = delete;
  SidebarLibrary& operator=(const SidebarLibrary&) = delete;
  void Initialize();
  void Visibility(uint32_t section, bool visible);
  void Command(uint32_t section, uint64_t item, uint32_t command);
  std::unique_ptr<ui::MenuModel> CreateMenu(uint64_t item);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace krypton_ui
#endif
