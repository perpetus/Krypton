#include "chrome/browser/ui/krypton_ui/sidebar_media.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/containers/flat_set.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/media/webrtc/media_capture_devices_dispatcher.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/krypton_ui/window_session.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/media_session.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "services/media_session/public/cpp/media_metadata.h"
#include "services/media_session/public/cpp/media_position.h"
#include "services/media_session/public/mojom/media_session.mojom.h"

namespace krypton_ui {
namespace {
using Action = media_session::mojom::MediaSessionAction;
KrStr Borrow(const std::string& text) {
  return {text.data(), text.size()};
}
MediaStreamCaptureIndicator* Capture() {
  return MediaCaptureDevicesDispatcher::GetInstance()
      ->GetMediaStreamCaptureIndicator()
      .get();
}
}  // namespace
class SidebarMedia::Tab final
    : public content::WebContentsObserver,
      public media_session::mojom::MediaSessionObserver {
 public:
  Tab(SidebarMedia& owner, content::WebContents& contents, KrTabId id)
      : content::WebContentsObserver(&contents), owner_(owner), id_(id) {
    Bind();
    // A capture-only tab may never create a playback media session. Publish
    // existing capture state when attaching, without queuing idle tabs.
    if (Sharing())
      Update();
  }
  ~Tab() override { Remove(); }
  void Bind() {
    if (!web_contents() || receiver_.is_bound())
      return;
    if (auto* session = content::MediaSession::GetIfExists(web_contents())) {
      actions_ = session->GetMediaSessionActionsSync();
      session->AddObserver(receiver_.BindNewPipeAndPassRemote());
      Update();
    }
  }
  void Update() {
    if (scheduled_)
      return;
    scheduled_ = true;
    std::ignore = owner_->session_->Defer(
        base::BindOnce(&Tab::Publish, weak_factory_.GetWeakPtr()));
  }
  void Command(uint32_t command) {
    if (!web_contents())
      return;
    auto* session = content::MediaSession::GetIfExists(web_contents());
    const bool sharing = Sharing();
    switch (command) {
      case KR_MEDIA_FOCUS: {
        auto* strip = owner_->browser_->GetTabStripModel();
        const int index = strip->GetIndexOfWebContents(web_contents());
        if (index >= 0)
          strip->ActivateTabAt(index);
        owner_->browser_->GetWindow()->Activate();
        return;
      }
      case KR_MEDIA_TOGGLE_MUTE:
        web_contents()->SetAudioMuted(!web_contents()->IsAudioMuted());
        return;
      case KR_MEDIA_CLOSE:
        if (sharing) {
          const int types = MediaStreamCaptureIndicator::kUserMedia |
                            MediaStreamCaptureIndicator::kDisplayMedia;
          Capture()->StopMediaCapturing(web_contents(), types);
          if (session && HasAction(Action::kHangUp))
            session->DidReceiveAction(Action::kHangUp);
        }
        if (session)
          session->Suspend(content::MediaSession::SuspendType::kUI);
        dismissed_ = true;
        Update();
        return;
      default:
        break;
    }
    if (!session)
      return;
    switch (command) {
      case KR_MEDIA_PLAY_PAUSE: {
        const auto info = session->GetMediaSessionInfoSync();
        if (!info || !info->is_controllable)
          return;
        const bool playing = info->playback_state ==
                             media_session::mojom::MediaPlaybackState::kPlaying;
        if (playing)
          session->Suspend(content::MediaSession::SuspendType::kUI);
        else
          session->Resume(content::MediaSession::SuspendType::kUI);
        break;
      }
      case KR_MEDIA_PREVIOUS:
        if (HasAction(Action::kPreviousTrack))
          session->PreviousTrack();
        break;
      case KR_MEDIA_NEXT:
        if (HasAction(Action::kNextTrack))
          session->NextTrack();
        break;
      case KR_MEDIA_TOGGLE_PICTURE_IN_PICTURE: {
        const auto info = session->GetMediaSessionInfoSync();
        const auto action =
            info && info->picture_in_picture_state ==
                        media_session::mojom::MediaPictureInPictureState::
                            kInPictureInPicture
                ? Action::kExitPictureInPicture
                : Action::kEnterPictureInPicture;
        if (HasAction(action)) {
          if (action == Action::kExitPictureInPicture)
            session->ExitPictureInPicture();
          else
            session->EnterPictureInPicture();
        }
        break;
      }
      case KR_MEDIA_TOGGLE_MICROPHONE:
        if (HasAction(Action::kToggleMicrophone))
          session->DidReceiveAction(Action::kToggleMicrophone);
        break;
      case KR_MEDIA_TOGGLE_CAMERA:
        if (HasAction(Action::kToggleCamera))
          session->DidReceiveAction(Action::kToggleCamera);
        break;
      default:
        break;
    }
  }
  void Seek(double seconds) {
    if (!web_contents() || !std::isfinite(seconds) || seconds < 0 ||
        !HasAction(Action::kSeekTo))
      return;
    if (auto* session = content::MediaSession::GetIfExists(web_contents())) {
      if (const auto position = session->GetMediaSessionPosition();
          position && position->duration().is_positive())
        session->SeekTo(base::Seconds(
            std::min(seconds, position->duration().InSecondsF())));
    }
  }
  KrMediaId id() const { return id_; }

 private:
  bool Sharing() const {
    return web_contents() && (Capture()->IsCapturingUserMedia(web_contents()) ||
                              Capture()->IsCapturingTab(web_contents()) ||
                              Capture()->IsCapturingWindow(web_contents()) ||
                              Capture()->IsCapturingDisplay(web_contents()));
  }
  bool HasAction(Action action) const {
    if (!web_contents())
      return false;
    auto* session = content::MediaSession::GetIfExists(web_contents());
    if (!session)
      return false;
    return std::ranges::find(actions_, action) != actions_.end();
  }
  void Remove() {
    if (!published_)
      return;
    if (const auto* api = owner_->session_->api();
        api && KR_HAS_SLOT(api, KrShellUI, remove_media_session) &&
        api->remove_media_session)
      api->remove_media_session(api->self, id_);
    published_ = false;
  }
  void Publish() {
    scheduled_ = false;
    const auto* api = owner_->session_->api();
    if (!web_contents() || !api ||
        !KR_HAS_SLOT(api, KrShellUI, upsert_media_session) ||
        !api->upsert_media_session) {
      Remove();
      return;
    }
    const bool sharing = Sharing();
    auto* session = content::MediaSession::GetIfExists(web_contents());
    const auto info = session ? session->GetMediaSessionInfoSync() : nullptr;
    const bool playing =
        info && info->playback_state ==
                    media_session::mojom::MediaPlaybackState::kPlaying;
    if (playing && !was_playing_)
      dismissed_ = false;
    was_playing_ = playing;
    if (!sharing &&
        (!info || !info->is_controllable || (dismissed_ && !playing))) {
      Remove();
      return;
    }
    uint32_t flags = playing ? KR_MEDIA_PLAYING : 0;
    if (sharing)
      flags |= KR_MEDIA_SHARING;
    if (web_contents()->IsAudioMuted())
      flags |= KR_MEDIA_MUTED;
    if (info && info->is_controllable)
      flags |= KR_MEDIA_CAN_PLAY_PAUSE;
    const auto has = [&](Action action) { return HasAction(action); };
    if (has(Action::kPreviousTrack))
      flags |= KR_MEDIA_CAN_PREVIOUS;
    if (has(Action::kNextTrack))
      flags |= KR_MEDIA_CAN_NEXT;
    if (has(Action::kSeekTo))
      flags |= KR_MEDIA_CAN_SEEK;
    if (has(Action::kEnterPictureInPicture) ||
        has(Action::kExitPictureInPicture))
      flags |= KR_MEDIA_CAN_PICTURE_IN_PICTURE;
    if (has(Action::kToggleMicrophone))
      flags |= KR_MEDIA_CAN_TOGGLE_MICROPHONE;
    if (has(Action::kToggleCamera))
      flags |= KR_MEDIA_CAN_TOGGLE_CAMERA;
    if (info &&
        info->microphone_state == media_session::mojom::MicrophoneState::kMuted)
      flags |= KR_MEDIA_MICROPHONE_MUTED;
    if (info &&
        info->camera_state == media_session::mojom::CameraState::kTurnedOff)
      flags |= KR_MEDIA_CAMERA_MUTED;
    if (info && info->picture_in_picture_state ==
                    media_session::mojom::MediaPictureInPictureState::
                        kInPictureInPicture)
      flags |= KR_MEDIA_PICTURE_IN_PICTURE;
    const auto& metadata =
        session ? session->GetMediaSessionMetadata() : empty_metadata_;
    const auto title = base::UTF16ToUTF8(
        metadata.title.empty() ? web_contents()->GetTitle() : metadata.title);
    const auto artist = base::UTF16ToUTF8(metadata.artist);
    const auto position =
        session ? session->GetMediaSessionPosition() : std::nullopt;
    const auto finite = [](double value) {
      return std::isfinite(value) && value >= 0 ? value : 0.;
    };
    const KrMediaSession value{
        sizeof(KrMediaSession),
        id_,
        id_,
        flags,
        Borrow(title),
        Borrow(artist),
        0,
        position ? finite(position->GetPosition().InSecondsF()) : 0.,
        position ? finite(position->duration().InSecondsF()) : 0.,
        position ? finite(position->playback_rate()) : 0.};
    api->upsert_media_session(api->self, &value);
    published_ = true;
  }
  void MediaStartedPlaying(const MediaPlayerInfo&,
                           const content::MediaPlayerId&) override {
    Bind();
    Update();
  }
  void MediaStoppedPlaying(
      const MediaPlayerInfo&,
      const content::MediaPlayerId&,
      content::WebContentsObserver::MediaStoppedReason) override {
    Update();
  }
  void MediaMutedStatusChanged(const content::MediaPlayerId&, bool) override {
    Update();
  }
  void DidUpdateAudioMutingState(bool) override { Update(); }
  void TitleWasSet(content::NavigationEntry*) override { Update(); }
  void WebContentsDestroyed() override {
    receiver_.reset();
    Remove();
  }
  void MediaSessionInfoChanged(
      media_session::mojom::MediaSessionInfoPtr) override {
    Update();
  }
  void MediaSessionMetadataChanged(
      const std::optional<media_session::MediaMetadata>&) override {
    Update();
  }
  void MediaSessionActionsChanged(const std::vector<Action>& actions) override {
    actions_ = actions;
    Update();
  }
  void MediaSessionImagesChanged(
      const base::flat_map<media_session::mojom::MediaSessionImageType,
                           std::vector<media_session::MediaImage>>&) override {}
  void MediaSessionPositionChanged(
      const std::optional<media_session::MediaPosition>&) override {
    Update();
  }
  const raw_ref<SidebarMedia> owner_;
  const KrMediaId id_;
  const media_session::MediaMetadata empty_metadata_;
  std::vector<Action> actions_;
  bool published_ = false, scheduled_ = false, dismissed_ = false,
       was_playing_ = false;
  mojo::Receiver<media_session::mojom::MediaSessionObserver> receiver_{this};
  base::WeakPtrFactory<Tab> weak_factory_{this};
};

SidebarMedia::SidebarMedia(BrowserWindowInterface& browser,
                           WindowSession& session,
                           TabIdentity identity)
    : browser_(browser), session_(session), identity_(std::move(identity)) {}
SidebarMedia::~SidebarMedia() {
  TabStripModelObserver::StopObservingAll(this);
}
void SidebarMedia::Initialize() {
  const auto* api = session_->api();
  if (!api || !KR_HAS_SLOT(api, KrShellUI, upsert_media_session) ||
      !api->upsert_media_session ||
      !KR_HAS_SLOT(api, KrShellUI, remove_media_session) ||
      !api->remove_media_session)
    return;
  browser_->GetTabStripModel()->AddObserver(this);
  capture_observation_.Observe(Capture());
  SyncTabs();
}
void SidebarMedia::SyncTabs() {
  auto* strip = browser_->GetTabStripModel();
  base::flat_set<content::WebContents*> live;
  for (int i = 0; i < strip->count(); ++i) {
    auto* tab = strip->GetTabAtIndex(i);
    auto* contents = tab->GetContents();
    live.insert(contents);
    if (!tabs_.contains(contents))
      tabs_[contents] =
          std::make_unique<Tab>(*this, *contents, identity_.Run(*tab));
  }
  for (auto it = tabs_.begin(); it != tabs_.end();) {
    if (!live.contains(it->first))
      it = tabs_.erase(it);
    else
      ++it;
  }
}
void SidebarMedia::OnTabStripModelChanged(TabStripModel*,
                                          const TabStripModelChange& change,
                                          const TabStripSelectionChange&) {
  if (change.type() == TabStripModelChange::kInserted ||
      change.type() == TabStripModelChange::kRemoved ||
      change.type() == TabStripModelChange::kReplaced)
    SyncTabs();
}
void SidebarMedia::CaptureChanged(content::WebContents* contents) {
  const auto it = tabs_.find(contents);
  if (it != tabs_.end()) {
    it->second->Bind();
    it->second->Update();
  }
}
void SidebarMedia::OnIsCapturingVideoChanged(content::WebContents* c, bool) {
  CaptureChanged(c);
}
void SidebarMedia::OnIsCapturingAudioChanged(content::WebContents* c, bool) {
  CaptureChanged(c);
}
void SidebarMedia::OnIsBeingMirroredChanged(content::WebContents* c, bool) {
  CaptureChanged(c);
}
void SidebarMedia::OnIsCapturingTabChanged(content::WebContents* c, bool) {
  CaptureChanged(c);
}
void SidebarMedia::OnIsCapturingWindowChanged(content::WebContents* c, bool) {
  CaptureChanged(c);
}
void SidebarMedia::OnIsCapturingDisplayChanged(content::WebContents* c, bool) {
  CaptureChanged(c);
}
void SidebarMedia::Command(KrMediaId id, uint32_t command) {
  for (const auto& [contents, tab] : tabs_)
    if (tab->id() == id) {
      tab->Command(command);
      return;
    }
}
void SidebarMedia::Seek(KrMediaId id, double seconds) {
  for (const auto& [contents, tab] : tabs_)
    if (tab->id() == id) {
      tab->Seek(seconds);
      return;
    }
}
}  // namespace krypton_ui
