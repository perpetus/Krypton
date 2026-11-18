// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_SPACE_HOSTS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_SPACE_HOSTS_H_

#include <cstdint>
#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/files/file_path.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "krypton_ui/abi.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"

class Profile;

namespace krypton_ui {
class BrowserAdapter;

// One physical window can present Spaces of several persistent profiles. The
// window's own Browser owns the frame, caption and backdrop. Each other
// profile is hosted by a real Browser of that profile (its own TabStripModel,
// sessions, extensions and services) whose widget is a child of the frame and
// whose KryptonUI shell shares the frame. Exactly one profile's shell and page
// are presented. Owned by the frame's BrowserAdapter; UI thread only.
class SpaceHosts final : public views::WidgetObserver {
 public:
  explicit SpaceHosts(BrowserAdapter& frame);
  SpaceHosts(const SpaceHosts&) = delete;
  SpaceHosts& operator=(const SpaceHosts&) = delete;
  ~SpaceHosts() override;

  // Presents `space`, hosted by the persistent profile at `profile`. Loads the
  // profile and creates its host on first use. Newer requests supersede older
  // ones; nothing falls back to another profile.
  void Present(KrSpaceId space, const base::FilePath& profile);
  void PresentFrame();
  // Chromium asked a host to show or activate (a link, a restore, a command).
  void PresentHost(BrowserAdapter& host, bool activate);

  void AddHost(BrowserAdapter& host);
  void RemoveHost(BrowserAdapter& host);
  [[nodiscard]] BrowserAdapter* HostFor(const base::FilePath& profile) const;
  [[nodiscard]] bool IsPresented(const BrowserAdapter& host) const;
  // The presented host; null while the frame's own profile is presented.
  [[nodiscard]] BrowserAdapter* presented() const { return presented_.get(); }
  [[nodiscard]] bool empty() const;
  template <typename Visit>
  void ForEachHost(Visit visit) const {
    for (const auto& host : hosts_) {
      if (host) {
        visit(*host);
      }
    }
  }

  // Closes every host through its own unload handling, presenting each in
  // turn. Returns true when none remain and the frame may close now. While
  // the browser quits, Chromium has already run unload handlers, so every
  // host closes at once.
  [[nodiscard]] bool CloseHostsForFrameClose(bool quitting);
  // The frame's adapter is closing: hosts release its preferences and close.
  void DetachHosts();

 private:
  void Loaded(uint64_t generation,
              KrSpaceId space,
              base::FilePath path,
              Profile* profile);
  void Apply(BrowserAdapter* presenter);
  void UpdateActivation();
  void CloseNextHost();
  void HostRemoved(bool continue_close,
                   bool was_presented,
                   uint64_t generation);
  void EndCloseAttempt();
  // views::WidgetObserver:
  void OnWidgetBoundsChanged(views::Widget* widget,
                             const gfx::Rect& bounds) override;
  void OnWidgetActivationChanged(views::Widget* widget, bool active) override;
  void OnWidgetDestroying(views::Widget* widget) override;

  const raw_ref<BrowserAdapter> frame_;
  std::vector<base::WeakPtr<BrowserAdapter>> hosts_;
  base::WeakPtr<BrowserAdapter> presented_;  // Null: the frame's own profile.
  uint64_t generation_ = 0;
  bool frame_closing_ = false;
  bool quitting_ = false;
  base::WeakPtr<BrowserAdapter> closing_;  // The host a frame close awaits.
  base::ScopedObservation<views::Widget, views::WidgetObserver> observation_{
      this};
  base::CallbackListSubscription close_cancelled_;
  base::WeakPtrFactory<SpaceHosts> weak_factory_{this};
};

// Set while SpaceHosts or session restore creates a host Browser; read by the
// new BrowserAdapter's constructor.
class ScopedSpaceHostCreation {
 public:
  explicit ScopedSpaceHostCreation(base::WeakPtr<BrowserAdapter> frame);
  ScopedSpaceHostCreation(const ScopedSpaceHostCreation&) = delete;
  ScopedSpaceHostCreation& operator=(const ScopedSpaceHostCreation&) = delete;
  ~ScopedSpaceHostCreation();

 private:
  base::WeakPtr<BrowserAdapter> previous_;
};
[[nodiscard]] base::WeakPtr<BrowserAdapter> PendingSpaceHostFrame();

// Durable window identities of live frames, for restoring their hosts.
void RegisterFrameWindow(const std::string& id,
                         base::WeakPtr<BrowserAdapter> frame);
void UnregisterFrameWindow(const std::string& id);
[[nodiscard]] base::WeakPtr<BrowserAdapter> FindFrameWindow(
    const std::string& id);
// A host restored before its frame existed runs as an ordinary window of its
// profile until that frame appears and adopts its tabs.
void RegisterOrphanHost(const std::string& frame_id,
                        base::WeakPtr<BrowserAdapter> host);
[[nodiscard]] std::vector<base::WeakPtr<BrowserAdapter>> TakeOrphanHosts(
    const std::string& frame_id);
}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_SPACE_HOSTS_H_
