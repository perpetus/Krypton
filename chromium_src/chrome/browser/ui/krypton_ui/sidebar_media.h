#ifndef CHROME_BROWSER_UI_KRYPTON_UI_SIDEBAR_MEDIA_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_SIDEBAR_MEDIA_H_

#include <memory>

#include "base/containers/flat_map.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ref.h"
#include "base/scoped_observation.h"
#include "chrome/browser/media/webrtc/media_stream_capture_indicator.h"
#include "chrome/browser/ui/krypton_ui/include/krypton_ui/abi.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"

class BrowserWindowInterface;
namespace tabs {
class TabInterface;
}
namespace krypton_ui {
class WindowSession;
class SidebarMedia final : public TabStripModelObserver,
                           public MediaStreamCaptureIndicator::Observer {
 public:
  using TabIdentity = base::RepeatingCallback<KrTabId(tabs::TabInterface&)>;
  SidebarMedia(BrowserWindowInterface& browser,
               WindowSession& session,
               TabIdentity identity);
  ~SidebarMedia() override;
  void Initialize();
  void Command(KrMediaId id, uint32_t command);
  void Seek(KrMediaId id, double seconds);

 private:
  class Tab;
  void SyncTabs();
  void CaptureChanged(content::WebContents* contents);
  void OnTabStripModelChanged(TabStripModel*,
                              const TabStripModelChange&,
                              const TabStripSelectionChange&) override;
  void OnIsCapturingVideoChanged(content::WebContents*, bool) override;
  void OnIsCapturingAudioChanged(content::WebContents*, bool) override;
  void OnIsBeingMirroredChanged(content::WebContents*, bool) override;
  void OnIsCapturingTabChanged(content::WebContents*, bool) override;
  void OnIsCapturingWindowChanged(content::WebContents*, bool) override;
  void OnIsCapturingDisplayChanged(content::WebContents*, bool) override;
  const raw_ref<BrowserWindowInterface> browser_;
  const raw_ref<WindowSession> session_;
  TabIdentity identity_;
  base::flat_map<content::WebContents*, std::unique_ptr<Tab>> tabs_;
  base::ScopedObservation<MediaStreamCaptureIndicator,
                          MediaStreamCaptureIndicator::Observer>
      capture_observation_{this};
};
}  // namespace krypton_ui
#endif
