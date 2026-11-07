#include "chrome/browser/ui/krypton_ui/sidebar_downloads.h"
#include "ui/base/base_window.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/download/download_item_model.h"
#include "chrome/browser/download/download_ui_context_menu.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/browser/ui/krypton_ui/window_session.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "content/public/browser/browser_context.h"
#include "ui/menus/simple_menu_model.h"

namespace krypton_ui {
namespace {
KrStr Borrow(const std::string& text) {
  return {text.data(), text.size()};
}

class DownloadContext final : public DownloadUiContextMenu {
 public:
  explicit DownloadContext(base::WeakPtr<DownloadUIModel> model)
      : DownloadUiContextMenu(model) {}
  using DownloadUiContextMenu::GetMenuModel;
};
// The options and command delegate are the real download context menu. Only
// the small menu structure is copied so ownership fits the native transport.
class OwnedDownloadMenu final : public ui::SimpleMenuModel,
                                public DownloadUIModel::Delegate {
 public:
  static std::unique_ptr<OwnedDownloadMenu> Create(
      download::DownloadItem& item) {
    auto model = std::make_unique<DownloadItemModel>(&item);
    auto context = std::make_unique<DownloadContext>(model->GetWeakPtr());
    if (!context->GetMenuModel())
      return nullptr;
    return std::unique_ptr<OwnedDownloadMenu>(
        new OwnedDownloadMenu(std::move(model), std::move(context)));
  }

 private:
  OwnedDownloadMenu(std::unique_ptr<DownloadItemModel> model,
                    std::unique_ptr<DownloadContext> context)
      : ui::SimpleMenuModel(context.get()),
        model_(std::move(model)),
        context_(std::move(context)) {
    model_->SetDelegate(this);
    Copy(*this, *context_->GetMenuModel());
  }
  void OnDownloadDestroyed(const ContentId&) override {
    context_->OnDownloadDestroyed();
  }
  void Copy(ui::SimpleMenuModel& output, ui::MenuModel& source) {
    for (size_t i = 0; i < source.GetItemCount(); ++i) {
      const auto command = source.GetCommandIdAt(i);
      const auto label = source.GetLabelAt(i);
      switch (source.GetTypeAt(i)) {
        case ui::MenuModel::TYPE_SEPARATOR:
          output.AddSeparator(ui::NORMAL_SEPARATOR);
          break;
        case ui::MenuModel::TYPE_CHECK:
          output.AddCheckItem(command, label);
          break;
        case ui::MenuModel::TYPE_RADIO:
          output.AddRadioItem(command, label, source.GetGroupIdAt(i));
          break;
        case ui::MenuModel::TYPE_SUBMENU:
        case ui::MenuModel::TYPE_ACTIONABLE_SUBMENU: {
          auto child = std::make_unique<ui::SimpleMenuModel>(context_.get());
          Copy(*child, *source.GetSubmenuModelAt(i));
          output.AddSubMenu(command, label, child.get());
          children_.push_back(std::move(child));
          break;
        }
        default:
          output.AddItem(command, label);
          break;
      }
    }
  }
  std::unique_ptr<DownloadItemModel> model_;
  std::unique_ptr<DownloadContext> context_;
  std::vector<std::unique_ptr<ui::SimpleMenuModel>> children_;
};
}  // namespace
SidebarDownloads::SidebarDownloads(BrowserWindowInterface& browser,
                                   WindowSession& session)
    : browser_(browser), session_(session) {}
SidebarDownloads::~SidebarDownloads() = default;
void SidebarDownloads::Initialize() {
  const auto* api = session_->api();
  if (!api || !KR_HAS_SLOT(api, KrShellUI, upsert_download) ||
      !api->upsert_download || !KR_HAS_SLOT(api, KrShellUI, remove_download) ||
      !api->remove_download)
    return;
  auto* manager = browser_->GetProfile()->GetDownloadManager();
  manager_.Observe(manager);
  content::DownloadManager::DownloadVector downloads;
  manager->GetAllDownloads(&downloads);
  for (const auto& item : downloads)
    Track(*item);
  manager->CheckForHistoryFilesRemoval();
  initialized_ = manager->IsManagerInitialized();
  if (initialized_)
    PublishAll();
}
void SidebarDownloads::SetLibraryChanged(base::RepeatingClosure callback) {
  library_changed_ = std::move(callback);
}
void SidebarDownloads::Track(download::DownloadItem& item) {
  items_[item.GetId()] = &item;
  if (!observations_.IsObservingSource(&item))
    observations_.AddObservation(&item);
}
void SidebarDownloads::OnDownloadCreated(content::DownloadManager*,
                                         download::DownloadItem* item) {
  Track(*item);
  if (initialized_ && item->GetState() == download::DownloadItem::IN_PROGRESS)
    started_.insert(item->GetId());
  Schedule(item->GetId());
}
void SidebarDownloads::OnManagerInitialized() {
  initialized_ = true;
  browser_->GetProfile()->GetDownloadManager()->CheckForHistoryFilesRemoval();
  PublishAll();
}
void SidebarDownloads::ManagerGoingDown(content::DownloadManager*) {
  observations_.RemoveAllObservations();
  if (const auto* api = session_->api();
      api && KR_HAS_SLOT(api, KrShellUI, remove_download) &&
      api->remove_download) {
    for (const auto id : published_)
      api->remove_download(api->self, id);
  }
  published_.clear();
  items_.clear();
  dirty_.clear();
  completed_media_.clear();
  library_dirty_ = true;
  manager_.Reset();
  initialized_ = false;
  NotifyLibraryChanged();
}
void SidebarDownloads::OnDownloadUpdated(download::DownloadItem* item) {
  Schedule(item->GetId());
}
void SidebarDownloads::OnDownloadRemoved(download::DownloadItem* item) {
  Remove(*item);
}
void SidebarDownloads::OnDownloadDestroyed(download::DownloadItem* item) {
  Remove(*item);
}
void SidebarDownloads::Remove(download::DownloadItem& item) {
  const auto id = item.GetId();
  if (observations_.IsObservingSource(&item))
    observations_.RemoveObservation(&item);
  items_.erase(id);
  dirty_.erase(id);
  started_.erase(id);
  if (published_.erase(id)) {
    if (const auto* api = session_->api();
        api && KR_HAS_SLOT(api, KrShellUI, remove_download) &&
        api->remove_download)
      api->remove_download(api->self, id);
  }
  if (completed_media_.erase(id))
    library_dirty_ = true;
  NotifyLibraryChanged();
}
void SidebarDownloads::Schedule(uint32_t id) {
  dirty_.insert(id);
  if (!initialized_ || scheduled_)
    return;
  scheduled_ = true;
  std::ignore = session_->Defer(
      base::BindOnce(&SidebarDownloads::Flush, weak_factory_.GetWeakPtr()));
}
void SidebarDownloads::Flush() {
  scheduled_ = false;
  for (const auto id : dirty_) {
    if (auto* item = Find(id))
      Publish(*item);
  }
  dirty_.clear();
  NotifyLibraryChanged();
}
void SidebarDownloads::PublishAll() {
  std::vector<download::DownloadItem*> order;
  order.reserve(items_.size());
  for (const auto& [id, item] : items_)
    order.push_back(item);
  std::ranges::sort(order, [](const auto* a, const auto* b) {
    return a->GetStartTime() < b->GetStartTime();
  });
  for (auto* item : order)
    Publish(*item);
  dirty_.clear();
  NotifyLibraryChanged();
}
void SidebarDownloads::NotifyLibraryChanged() {
  if (!library_dirty_)
    return;
  library_dirty_ = false;
  if (library_changed_)
    library_changed_.Run();
}
void SidebarDownloads::Publish(download::DownloadItem& item) {
  const auto* api = session_->api();
  if (!api || !KR_HAS_SLOT(api, KrShellUI, upsert_download) ||
      !api->upsert_download)
    return;
  DownloadItemModel model(&item);
  const auto mime = item.GetMimeType();
  const bool complete_media =
      item.GetState() == download::DownloadItem::COMPLETE &&
      !item.GetFileExternallyRemoved() &&
      (mime.starts_with("image/") || mime.starts_with("video/"));
  if (complete_media || completed_media_.contains(item.GetId()))
    library_dirty_ = true;
  if (complete_media)
    completed_media_.insert(item.GetId());
  else
    completed_media_.erase(item.GetId());
  if (item.GetState() == download::DownloadItem::COMPLETE &&
      item.GetFileExternallyRemoved()) {
    if (published_.erase(item.GetId()) &&
        KR_HAS_SLOT(api, KrShellUI, remove_download) && api->remove_download)
      api->remove_download(api->self, item.GetId());
    started_.erase(item.GetId());
    return;
  }
  const auto name = item.GetFileNameToReportUser().AsUTF8Unsafe();
  const auto status = base::UTF16ToUTF8(model.GetStatusText());
  uint32_t state = KR_DOWNLOAD_IN_PROGRESS;
  switch (item.GetState()) {
    case download::DownloadItem::IN_PROGRESS:
      state = item.IsPaused() ? KR_DOWNLOAD_PAUSED : KR_DOWNLOAD_IN_PROGRESS;
      break;
    case download::DownloadItem::COMPLETE:
      state = KR_DOWNLOAD_COMPLETE;
      break;
    case download::DownloadItem::CANCELLED:
      state = KR_DOWNLOAD_CANCELLED;
      break;
    case download::DownloadItem::INTERRUPTED:
      state = item.CanResume() ? KR_DOWNLOAD_PAUSED : KR_DOWNLOAD_FAILED;
      break;
    default:
      return;
  }
  const int percent = item.PercentComplete();
  uint32_t flags = percent >= 0 ? KR_DOWNLOAD_PROGRESS_KNOWN : 0;
  if (started_.erase(item.GetId()) && browser_->GetWindow()->IsActive())
    flags |= KR_DOWNLOAD_STARTED;
  if (item.GetState() == download::DownloadItem::COMPLETE &&
      !item.GetFileExternallyRemoved())
    flags |= KR_DOWNLOAD_FILE_EXISTS;
  if (item.GetOpenWhenComplete())
    flags |= KR_DOWNLOAD_OPEN_WHEN_DONE;
  if (item.IsDangerous() || item.IsInsecure())
    flags |= KR_DOWNLOAD_BLOCKED;
  const KrDownload value{
      sizeof(KrDownload),
      item.GetId(),
      state,
      flags,
      static_cast<float>(std::clamp(percent, 0, 100)) / 100.f,
      Borrow(name),
      Borrow(status),
      0};
  api->upsert_download(api->self, &value);
  published_.insert(item.GetId());
}
download::DownloadItem* SidebarDownloads::Find(uint64_t id) const {
  if (id > std::numeric_limits<uint32_t>::max())
    return nullptr;
  const auto it = items_.find(static_cast<uint32_t>(id));
  return it == items_.end() ? nullptr : it->second.get();
}
void SidebarDownloads::Command(uint64_t id, uint32_t command) {
  auto* item = Find(id);
  if (!item)
    return;
  switch (command) {
    case KR_DOWNLOAD_CANCEL:
      item->Cancel(true);
      break;
    case KR_DOWNLOAD_SHOW_IN_FOLDER:
      if (item->GetState() == download::DownloadItem::COMPLETE &&
          !item->GetFileExternallyRemoved())
        item->ShowDownloadInShell();
      break;
    case KR_DOWNLOAD_OPEN:
      if (item->IsDangerous() || item->IsInsecure()) {
        // Keep the browser's existing warning/confirmation flow authoritative.
        chrome::ShowDownloads(&*browser_);
      } else if (item->GetState() == download::DownloadItem::COMPLETE) {
        if (!item->GetFileExternallyRemoved() && item->CanOpenDownload())
          item->OpenDownload();
      } else if (item->GetState() == download::DownloadItem::IN_PROGRESS) {
        if (item->IsPaused()) {
          if (item->CanResume())
            item->Resume(true);
        } else {
          item->SetOpenWhenComplete(!item->GetOpenWhenComplete());
        }
      } else if (item->CanResume()) {
        item->Resume(true);
      } else if (item->GetOriginalUrl().is_valid()) {
        NavigateParams params(browser_->GetProfile(), item->GetOriginalUrl(),
                              ui::PAGE_TRANSITION_LINK);
        params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
        Navigate(&params);
      }
      break;
    default:
      break;
  }
}
std::unique_ptr<ui::MenuModel> SidebarDownloads::CreateMenu(uint64_t id) {
  auto* item = Find(id);
  return item ? OwnedDownloadMenu::Create(*item) : nullptr;
}
}  // namespace krypton_ui
