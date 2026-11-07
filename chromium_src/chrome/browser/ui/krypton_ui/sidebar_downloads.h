#ifndef CHROME_BROWSER_UI_KRYPTON_UI_SIDEBAR_DOWNLOADS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_SIDEBAR_DOWNLOADS_H_

#include <memory>

#include "base/containers/flat_map.h"
#include "base/containers/flat_set.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_multi_source_observation.h"
#include "base/scoped_observation.h"
#include "components/download/public/common/download_item.h"
#include "content/public/browser/download_manager.h"

class BrowserWindowInterface;
namespace ui {
class MenuModel;
}
namespace krypton_ui {
class WindowSession;
class SidebarDownloads final : public content::DownloadManager::Observer,
                               public download::DownloadItem::Observer {
 public:
  SidebarDownloads(BrowserWindowInterface& browser, WindowSession& session);
  ~SidebarDownloads() override;
  void Initialize();
  void Command(uint64_t id, uint32_t command);
  std::unique_ptr<ui::MenuModel> CreateMenu(uint64_t id);
  download::DownloadItem* Find(uint64_t id) const;
  void SetLibraryChanged(base::RepeatingClosure callback);
  const base::flat_map<uint32_t, raw_ptr<download::DownloadItem>>& items()
      const {
    return items_;
  }

 private:
  void OnDownloadCreated(content::DownloadManager*,
                         download::DownloadItem*) override;
  void OnManagerInitialized() override;
  void ManagerGoingDown(content::DownloadManager*) override;
  void OnDownloadUpdated(download::DownloadItem*) override;
  void OnDownloadRemoved(download::DownloadItem*) override;
  void OnDownloadDestroyed(download::DownloadItem*) override;
  void Track(download::DownloadItem& item);
  void Schedule(uint32_t id);
  void Flush();
  void Publish(download::DownloadItem& item);
  void Remove(download::DownloadItem& item);
  void PublishAll();
  void NotifyLibraryChanged();
  const raw_ref<BrowserWindowInterface> browser_;
  const raw_ref<WindowSession> session_;
  base::ScopedObservation<content::DownloadManager,
                          content::DownloadManager::Observer>
      manager_{this};
  base::ScopedMultiSourceObservation<download::DownloadItem,
                                     download::DownloadItem::Observer>
      observations_{this};
  base::flat_map<uint32_t, raw_ptr<download::DownloadItem>> items_;
  base::flat_set<uint32_t> dirty_, published_;
  base::flat_set<uint32_t> started_;
  base::flat_set<uint32_t> completed_media_;
  base::RepeatingClosure library_changed_;
  bool initialized_ = false, scheduled_ = false;
  bool library_dirty_ = false;
  base::WeakPtrFactory<SidebarDownloads> weak_factory_{this};
};
}  // namespace krypton_ui
#endif
