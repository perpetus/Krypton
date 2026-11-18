// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/space_hosts.h"

#include <algorithm>
#include <map>
#include <tuple>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/sessions/session_restore.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"

namespace krypton_ui {
namespace {
base::WeakPtr<BrowserAdapter>& Pending() {
  static base::NoDestructor<base::WeakPtr<BrowserAdapter>> pending;
  return *pending;
}
std::map<std::string, base::WeakPtr<BrowserAdapter>>& Frames() {
  static base::NoDestructor<
      std::map<std::string, base::WeakPtr<BrowserAdapter>>>
      frames;
  return *frames;
}
std::multimap<std::string, base::WeakPtr<BrowserAdapter>>& Orphans() {
  static base::NoDestructor<
      std::multimap<std::string, base::WeakPtr<BrowserAdapter>>>
      orphans;
  return *orphans;
}
}  // namespace

ScopedSpaceHostCreation::ScopedSpaceHostCreation(
    base::WeakPtr<BrowserAdapter> frame)
    : previous_(Pending()) {
  Pending() = std::move(frame);
}
ScopedSpaceHostCreation::~ScopedSpaceHostCreation() {
  Pending() = std::move(previous_);
}
base::WeakPtr<BrowserAdapter> PendingSpaceHostFrame() {
  return Pending();
}

void RegisterFrameWindow(const std::string& id,
                         base::WeakPtr<BrowserAdapter> frame) {
  Frames()[id] = std::move(frame);
}
void UnregisterFrameWindow(const std::string& id) {
  Frames().erase(id);
}
base::WeakPtr<BrowserAdapter> FindFrameWindow(const std::string& id) {
  const auto found = Frames().find(id);
  return found == Frames().end() ? nullptr : found->second;
}
void RegisterOrphanHost(const std::string& frame_id,
                        base::WeakPtr<BrowserAdapter> host) {
  Orphans().emplace(frame_id, std::move(host));
}
std::vector<base::WeakPtr<BrowserAdapter>> TakeOrphanHosts(
    const std::string& frame_id) {
  std::vector<base::WeakPtr<BrowserAdapter>> result;
  const auto [begin, end] = Orphans().equal_range(frame_id);
  for (auto it = begin; it != end; ++it) {
    if (it->second) {
      result.push_back(it->second);
    }
  }
  Orphans().erase(frame_id);
  return result;
}

SpaceHosts::SpaceHosts(BrowserAdapter& frame) : frame_(frame) {
  if (auto* widget = frame.FrameWidget()) {
    observation_.Observe(widget);
  }
}

SpaceHosts::~SpaceHosts() = default;

void SpaceHosts::Present(KrSpaceId space, const base::FilePath& profile) {
  EndCloseAttempt();
  const auto generation = ++generation_;
  if (auto* host = HostFor(profile)) {
    // Select the Space's tab while hidden, then present it.
    if (host->ShowSpace(space)) {
      Apply(host);
    }
    return;
  }
  auto* manager =
      g_browser_process ? g_browser_process->profile_manager() : nullptr;
  // Loading reads the profile off the UI thread. A later request or this
  // window's teardown invalidates the reply.
  if (!manager ||
      !manager->LoadProfileByPath(
          profile, /*incognito=*/false,
          base::BindOnce(&SpaceHosts::Loaded, weak_factory_.GetWeakPtr(),
                         generation, space, profile))) {
    frame_->NotifySpaceUnavailable();
  }
}

void SpaceHosts::Loaded(uint64_t generation,
                        KrSpaceId space,
                        base::FilePath path,
                        Profile* profile) {
  if (generation != generation_ || frame_closing_) {
    return;
  }
  // The mapping may have changed while loading; never open another profile.
  if (!profile || profile->IsOffTheRecord() || profile->GetPath() != path ||
      !frame_->SpaceUsesProfile(space, path)) {
    frame_->NotifySpaceUnavailable();
    return;
  }
  if (!HostFor(path)) {
    const ScopedSpaceHostCreation scope(frame_->GetWeakPtr());
    std::ignore = CreateBrowserWindow(
        BrowserWindowCreateParams(*profile, /*from_user_gesture=*/true));
  }
  if (auto* host = HostFor(path)) {
    // A new host gets its first tab before Chromium's first show.
    if (host->ShowSpace(space)) {
      Apply(host);
    }
  } else {
    frame_->NotifySpaceUnavailable();
  }
}

void SpaceHosts::PresentFrame() {
  EndCloseAttempt();
  ++generation_;
  Apply(nullptr);
}

void SpaceHosts::PresentHost(BrowserAdapter& host, bool activate) {
  if (quitting_ || !host.browser_initialized()) {
    return;
  }
  if (!IsPresented(host)) {
    // Startup restore shows every host; only the active Space's profile takes
    // the frame. Later requests come from links, commands or extensions.
    if (SessionRestore::IsRestoring(host.HostProfile()) &&
        !frame_->ActiveSpaceUses(host.HostProfile()->GetPath())) {
      return;
    }
    EndCloseAttempt();
    ++generation_;
    Apply(&host);
    host.FollowActiveTab();
  }
  if (activate) {
    if (auto* widget = frame_->FrameWidget()) {
      widget->Activate();
    }
  }
}

void SpaceHosts::AddHost(BrowserAdapter& host) {
  std::erase_if(hosts_, [&](const auto& value) {
    return !value || value.get() == &host;
  });
  hosts_.push_back(host.GetWeakPtr());
  host.FitToFrame();
  host.SetPresented(false);
}

void SpaceHosts::RemoveHost(BrowserAdapter& host) {
  const bool was_presented = presented_.get() == &host;
  // Continue a frame close only after the host it was closing, or any host
  // while quitting. A cancelled beforeunload leaves its host open; the next
  // Space switch ends that close attempt.
  const bool continue_close =
      frame_closing_ && (quitting_ || closing_.get() == &host);
  std::erase_if(hosts_, [&](const auto& value) {
    return !value || value.get() == &host;
  });
  if (was_presented) {
    presented_ = nullptr;
  }
  if (closing_.get() == &host) {
    closing_ = nullptr;
  }
  if (was_presented || continue_close) {
    // RemoveHost runs inside BrowserView::OnWidgetDestroying. Even showing
    // the frame can re-enter layout/focus while that widget is being torn
    // down. Do all presentation and close continuation after it unwinds.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostNonNestableTask(
        FROM_HERE,
        base::BindOnce(&SpaceHosts::HostRemoved, weak_factory_.GetWeakPtr(),
                       continue_close, was_presented, generation_));
  }
}

void SpaceHosts::HostRemoved(bool continue_close,
                             bool was_presented,
                             uint64_t generation) {
  if (generation != generation_) {
    return;
  }
  if (continue_close && frame_closing_) {
    if (empty()) {
      frame_closing_ = false;
      quitting_ = false;
      // Show the frame for its own beforeunload dialogs, then let Chromium
      // finish closing it. Close may destroy this coordinator synchronously.
      auto frame = frame_->GetWeakPtr();
      frame->SetPresented(true);
      if (frame) {
        frame->CloseFrameWindow();
      }
    } else if (!quitting_) {
      CloseNextHost();
    }
    return;
  }
  if (!was_presented || frame_closing_ || presented_) {
    return;
  }
  // A profile was closed/deleted independently. Restore the owner only if
  // no newer Space selection or frame-close request superseded this callback.
  const auto weak = weak_factory_.GetWeakPtr();
  frame_->SetPresented(true);
  if (weak) {
    frame_->RecoverActiveSpace();
    UpdateActivation();
  }
}

BrowserAdapter* SpaceHosts::HostFor(const base::FilePath& profile) const {
  for (const auto& host : hosts_) {
    if (host && host->HostProfile() &&
        host->HostProfile()->GetPath() == profile) {
      return host.get();
    }
  }
  return nullptr;
}

bool SpaceHosts::IsPresented(const BrowserAdapter& host) const {
  return presented_.get() == &host;
}

bool SpaceHosts::empty() const {
  return std::ranges::none_of(hosts_, [](const auto& host) { return !!host; });
}

bool SpaceHosts::CloseHostsForFrameClose(bool quitting) {
  std::erase_if(hosts_, [](const auto& host) { return !host; });
  if (hosts_.empty()) {
    return true;
  }
  if (!close_cancelled_) {
    // Subscribe only when closing an initialized frame, not while session
    // restore may still be constructing it. Recover its Space only after
    // Chromium explicitly cancels the close, never while tabs are unloading.
    close_cancelled_ =
        frame_->browser_->RegisterBrowserCloseCancelled(base::BindRepeating(
            [](base::WeakPtr<SpaceHosts> self, BrowserWindowInterface*,
               BrowserWindowInterface::ClosingStatus) {
              if (!self) {
                return;
              }
              self->frame_closing_ = false;
              self->quitting_ = false;
              self->closing_ = nullptr;
              self->frame_->RecoverActiveSpace();
            },
            weak_factory_.GetWeakPtr()));
  }
  frame_closing_ = true;
  quitting_ = quitting;
  if (!quitting) {
    CloseNextHost();
    return false;
  }
  // Copy: closing may synchronously remove hosts.
  const auto hosts = hosts_;
  for (const auto& host : hosts) {
    if (host) {
      host->CloseHostedWindow();
    }
  }
  return false;
}

void SpaceHosts::EndCloseAttempt() {
  if (!quitting_) {
    frame_closing_ = false;
    closing_ = nullptr;
  }
}

void SpaceHosts::DetachHosts() {
  frame_closing_ = true;
  ++generation_;
  presented_ = nullptr;
  // Copy: detaching closes hosts, which may remove them synchronously.
  const auto hosts = std::exchange(hosts_, {});
  for (const auto& host : hosts) {
    if (host) {
      host->DetachFromFrame();
    }
  }
}

void SpaceHosts::CloseNextHost() {
  for (const auto& host : hosts_) {
    if (host) {
      // Present it first: its beforeunload prompt must be visible.
      closing_ = host;
      Apply(host.get());
      host->CloseHostedWindow();
      return;
    }
  }
}

void SpaceHosts::Apply(BrowserAdapter* presenter) {
  if (presenter && !presenter->browser_initialized()) {
    return;
  }
  auto* current = presented_.get();
  if (current == presenter) {
    if (!presenter) {
      frame_->SetPresented(true);
    }
    UpdateActivation();
    return;
  }
  if (current) {
    current->SetPresented(false);
  } else {
    frame_->SetPresented(false);
  }
  presented_ = presenter ? presenter->GetWeakPtr() : nullptr;
  if (presenter) {
    presenter->SetPresented(true);
  } else {
    frame_->SetPresented(true);
  }
  UpdateActivation();
}

void SpaceHosts::UpdateActivation() {
  auto* widget = frame_->FrameWidget();
  const bool active = widget && widget->IsActive();
  for (const auto& host : hosts_) {
    if (host) {
      host->SetHostActive(active && IsPresented(*host));
    }
  }
  frame_->SetFrameActive(active && !presented_);
}

void SpaceHosts::OnWidgetBoundsChanged(views::Widget*, const gfx::Rect&) {
  for (const auto& host : hosts_) {
    if (host) {
      host->FitToFrame();
    }
  }
}

void SpaceHosts::OnWidgetActivationChanged(views::Widget*, bool) {
  // Run after BrowserView applies the frame's own activation bookkeeping.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<SpaceHosts> self) {
                       if (self) {
                         self->UpdateActivation();
                       }
                     },
                     weak_factory_.GetWeakPtr()));
}

void SpaceHosts::OnWidgetDestroying(views::Widget*) {
  observation_.Reset();
  // Host widgets are children of this widget; never leave them parented to a
  // destroyed frame or observing its profile's preferences.
  DetachHosts();
}
}  // namespace krypton_ui
