// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_LIVE_FOLDERS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_LIVE_FOLDERS_H_
#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "krypton_ui/abi.h"
namespace content {
class WebContents;
}
namespace krypton_ui {
class BrowserAdapter;
class LiveFolderService;
struct LiveFolderTabState;
// A separate observer avoids provider checks in ordinary navigation, paint,
// tab publication and input paths. The profile owns the shared fetch service.
class LiveFolders final : public TabStripModelObserver {
public:
  explicit LiveFolders(BrowserAdapter &owner);
  ~LiveFolders() override;
  LiveFolders(const LiveFolders &) = delete;
  LiveFolders &operator=(const LiveFolders &) = delete;
  KrResult Command(const KrFolderCommand &command);

private:
  friend class LiveFolderService;
  bool Ready() const;
  void DeferStart();
  void Start();
  void PublishStatus();
  void StoreTabState(content::WebContents *contents, LiveFolderTabState state);
  void
  OnTabStripModelChanged(TabStripModel *model,
                         const TabStripModelChange &change,
                         const TabStripSelectionChange &selection) override;
  const raw_ref<BrowserAdapter> owner_;
  raw_ptr<LiveFolderService> service_ = nullptr;
  bool observing_ = false;
  base::CallbackListSubscription restored_;
  base::WeakPtrFactory<LiveFolders> weak_factory_{this};
};
} // namespace krypton_ui
#endif
