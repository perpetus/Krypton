#include "chrome/browser/krypton_agent/organization.h"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

#include "base/containers/flat_set.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_split.h"
#include "base/strings/utf_string_conversions.h"
#include "base/unguessable_token.h"
#include "chrome/browser/krypton_agent/privacy_filter.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"
#include "chrome/browser/ui/krypton_ui/document_store.h"
#include "chrome/browser/ui/krypton_ui/site_customization.h"
#include "chrome/browser/ui/krypton_ui/workspace_session.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "content/public/browser/web_contents.h"

namespace krypton_agent {
namespace {

constexpr Organization::Operation kOperations[]{
    {"krypton_list_tabs", "List tabs in the shared window.", "", ""},
    {"krypton_list_spaces", "List Spaces in the shared profile.", "", ""},
    {"krypton_list_folders", "List folders in the shared profile.", "",
     "space"},
    {"krypton_list_essentials",
     "List live Essential tabs in the shared window.", "", ""},
    {"krypton_open_tab", "Open a tab after approval.", "url",
     "space folder position", true},
    {"krypton_activate_tab", "Activate a tab.", "tab", ""},
    {"krypton_move_tabs", "Move tabs or assign them to a folder.", "tabs",
     "space folder position"},
    {"krypton_close_tabs", "Close tabs after approval.", "tabs", "", true},
    {"krypton_pin_tabs", "Pin tabs.", "tabs", ""},
    {"krypton_unpin_tabs", "Unpin tabs.", "tabs", ""},
    {"krypton_add_essential", "Add a tab to Essentials.", "tab", ""},
    {"krypton_remove_essential", "Remove an Essential after approval.", "tab",
     "", true},
    {"krypton_create_space", "Create a Space in the shared profile.", "name",
     ""},
    {"krypton_rename_space", "Rename a Space.", "space name", ""},
    {"krypton_switch_space", "Switch to a Space in the shared profile.",
     "space", ""},
    {"krypton_delete_space", "Remove a Space after approval.", "space", "",
     true},
    {"krypton_create_folder", "Create a tab folder.", "name", "space parent"},
    {"krypton_rename_folder", "Rename a tab folder.", "folder name", ""},
    {"krypton_move_folder", "Move a folder within its Space.", "folder",
     "space parent position"},
    {"krypton_remove_folder", "Remove a folder after approval.", "folder", "",
     true},
    {"krypton_create_split", "Combine tabs into a split.", "tabs", "layout"},
    {"krypton_open_glance", "Open a Glance after approval.", "tab url", "",
     true},
    {"krypton_new_note", "Create a note after approval.", "", "", true},
    {"krypton_new_easel", "Create an easel after approval.", "", "", true},
    {"krypton_clear_space", "Clear temporary tabs after approval.", "space", "",
     true},
    {"krypton_set_tab_icon", "Set a tab's icon.", "tab glyph", ""},
    {"krypton_apply_organization", "Review a batch of organization changes.",
     "plan", "", true},
};

using Result = Organization::Result;
using Operation = Organization::Operation;

Result Error(std::string message) {
  return {.text = std::move(message), .error = true};
}

Result Json(base::ListValue list) {
  return {.text = base::WriteJson(list).value_or("[]"), .page_derived = true};
}

std::vector<std::string_view> Keys(std::string_view keys) {
  return base::SplitStringPiece(keys, " ", base::KEEP_WHITESPACE,
                                base::SPLIT_WANT_NONEMPTY);
}

bool NumericKey(std::string_view key) {
  return key == "position" || key == "layout";
}

bool ListKey(std::string_view key) {
  return key == "tabs" || key == "plan";
}

std::string_view String(const base::DictValue& arguments,
                        std::string_view key) {
  const auto* value = arguments.FindString(key);
  return value ? std::string_view(*value) : std::string_view();
}

KrStr Wire(std::string_view text) {
  return {text.data(), text.size()};
}

Result Confirm() {
  return {.text = "Action requires user confirmation and was denied.",
          .error = true,
          .needs_confirmation = true};
}

}  // namespace

Organization::Organization() = default;
Organization::~Organization() = default;

const Operation* Organization::Find(std::string_view name) {
  const auto* found = std::ranges::find(kOperations, name, &Operation::name);
  return found == std::end(kOperations) ? nullptr : found;
}

void Organization::List(base::ListValue& tools) {
  for (const auto& operation : kOperations) {
    base::DictValue properties;
    base::ListValue required;
    for (auto key : Keys(operation.required)) {
      required.Append(key);
    }
    const auto add_property = [&properties](std::string_view key) {
      base::DictValue property;
      if (ListKey(key)) {
        property.Set("type", "array");
        property.Set("items", base::DictValue().Set(
                                  "type", key == "tabs" ? "string" : "object"));
      } else {
        property.Set("type", NumericKey(key) ? "integer" : "string");
      }
      properties.Set(key, std::move(property));
    };
    for (auto key : Keys(operation.required)) {
      add_property(key);
    }
    for (auto key : Keys(operation.optional)) {
      add_property(key);
    }
    tools.Append(
        base::DictValue()
            .Set("name", operation.name)
            .Set("description", operation.description)
            .Set("inputSchema", base::DictValue()
                                    .Set("type", "object")
                                    .Set("additionalProperties", false)
                                    .Set("required", std::move(required))
                                    .Set("properties", std::move(properties))));
  }
}

bool Organization::Valid(const Operation& operation,
                         const base::DictValue& arguments) {
  const auto required = Keys(operation.required);
  const auto optional = Keys(operation.optional);
  for (auto key : required) {
    if (!arguments.contains(key)) {
      return false;
    }
  }
  for (const auto [key, value] : arguments) {
    if (std::ranges::find(required, key) == required.end() &&
        std::ranges::find(optional, key) == optional.end()) {
      return false;
    }
    if (NumericKey(key)) {
      if (!value.is_int() || value.GetInt() < 0) {
        return false;
      }
    } else if (ListKey(key)) {
      if (!value.is_list() || value.GetList().empty() ||
          value.GetList().size() > 100) {
        return false;
      }
      for (const auto& item : value.GetList()) {
        if (key == "tabs" &&
            (!item.is_string() || item.GetString().size() > 64)) {
          return false;
        }
        if (key == "plan") {
          const auto* action = item.GetIfDict();
          const auto* name = action ? action->FindString("name") : nullptr;
          const auto* params = action ? action->FindDict("arguments") : nullptr;
          const auto* entry = name ? Find(*name) : nullptr;
          if (!entry || !params || entry->name.starts_with("krypton_list_") ||
              entry->name == "krypton_apply_organization" ||
              !Valid(*entry, *params)) {
            return false;
          }
        }
      }
    } else if (!value.is_string() ||
               value.GetString().size() > (key == "url" ? 4096u : 256u)) {
      return false;
    }
  }
  return true;
}

std::string Organization::Handle(tabs::TabInterface& tab) {
  const auto existing =
      std::ranges::find(handles_, tab.GetHandle(),
                        [](const auto& entry) { return entry.second; });
  if (existing != handles_.end()) {
    return existing->first;
  }
  std::string value = base::UnguessableToken::Create().ToString();
  handles_.try_emplace(value, tab.GetHandle());
  return value;
}

tabs::TabInterface* Organization::Resolve(std::string_view handle,
                                          BrowserWindowInterface& browser) {
  const auto found = handles_.find(handle);
  if (found == handles_.end()) {
    return nullptr;
  }
  auto* tab = found->second.Get();
  if (!tab || tab->GetBrowserWindowInterface() != &browser ||
      !tab->GetContents() ||
      tab->GetContents()->GetBrowserContext() != browser.GetProfile()) {
    handles_.erase(found);
    return nullptr;
  }
  return tab;
}

std::string Organization::CatalogHandle(char kind, uint64_t id) {
  if (!id) {
    return {};
  }
  const auto key = std::pair(kind, id);
  auto found = std::ranges::find(catalog_, key,
                                 [](const auto& pair) { return pair.second; });
  if (found != catalog_.end()) {
    return found->first;
  }
  auto handle = base::UnguessableToken::Create().ToString();
  catalog_.try_emplace(handle, key);
  return handle;
}

std::optional<uint64_t> Organization::ResolveCatalog(
    char kind,
    std::string_view handle) const {
  if (handle.empty()) {
    return 0;
  }
  auto found = catalog_.find(handle);
  if (found == catalog_.end() || found->second.first != kind) {
    return std::nullopt;
  }
  return found->second.second;
}

Result Organization::Read(const Operation& operation,
                          const base::DictValue& arguments,
                          BrowserWindowInterface& browser,
                          krypton_ui::BrowserAdapter& adapter,
                          PrivacyFilter& filter,
                          const url::Origin& origin) {
  base::ListValue list;
  if (operation.name == "krypton_list_spaces") {
    for (const auto& space : adapter.workspaces_.spaces) {
      if (!adapter.HostsSpace(space.id)) {
        continue;
      }
      list.Append(base::DictValue()
                      .Set("space", CatalogHandle('s', space.id))
                      .Set("name", filter.Text(space.name, origin))
                      .Set("active", space.id == adapter.active_space_));
    }
    return Json(std::move(list));
  }
  if (operation.name == "krypton_list_folders") {
    auto space = ResolveCatalog('s', String(arguments, "space"));
    if (!space || (*space && !adapter.HostsSpace(*space))) {
      return Error("Unknown Space.");
    }
    for (const auto& folder : adapter.folders_.items) {
      if (!adapter.HostsSpace(folder.space) ||
          (*space && *space != folder.space)) {
        continue;
      }
      list.Append(base::DictValue()
                      .Set("folder", CatalogHandle('f', folder.id))
                      .Set("space", CatalogHandle('s', folder.space))
                      .Set("parent", CatalogHandle('f', folder.parent))
                      .Set("name", filter.Text(folder.name, origin)));
    }
    return Json(std::move(list));
  }
  for (auto* tab : *browser.GetTabStripModel()) {
    auto* contents = tab->GetContents();
    if (!contents || contents->GetBrowserContext() != browser.GetProfile()) {
      continue;
    }
    const bool essential = krypton_ui::EssentialForTab(contents) != 0;
    if (operation.name == "krypton_list_essentials" && !essential) {
      continue;
    }
    const auto& url = contents->GetLastCommittedURL();
    const url::Origin tab_origin = url::Origin::Create(url);
    list.Append(
        base::DictValue()
            .Set("tab", Handle(*tab))
            .Set("title", filter.Text(base::UTF16ToUTF8(contents->GetTitle()),
                                      tab_origin))
            .Set("url", filter.Url(url, tab_origin))
            .Set("pinned", tab->IsPinned())
            .Set("essential", essential)
            .Set("audible", contents->IsCurrentlyAudible())
            .Set("active", tab->IsActivated())
            .Set("space",
                 CatalogHandle('s', krypton_ui::WorkspaceForTab(contents)))
            .Set("folder",
                 CatalogHandle('f', krypton_ui::FolderForTab(contents))));
    if (list.size() == 128) {
      break;
    }
  }
  return Json(std::move(list));
}

void Organization::Remember(BrowserWindowInterface& browser,
                            const std::vector<tabs::TabInterface*>& tabs) {
  std::vector<TabState> previous;
  auto* model = browser.GetTabStripModel();
  for (auto* tab : tabs) {
    auto* contents = tab->GetContents();
    previous.push_back({tab->GetHandle(), model->GetIndexOfTab(tab),
                        krypton_ui::WorkspaceForTab(contents),
                        krypton_ui::FolderForTab(contents), tab->IsPinned(),
                        krypton_ui::EssentialForTab(contents) != 0,
                        krypton_ui::TabIconForTab(contents)});
  }
  if (!previous.empty()) {
    undo_.push_back(std::move(previous));
  }
}

Result Organization::Run(const Operation& operation,
                         const base::DictValue& arguments,
                         BrowserWindowInterface& browser,
                         PrivacyFilter& filter,
                         const url::Origin& origin,
                         bool requires_confirmation,
                         bool approved) {
  auto* view = BrowserView::GetBrowserViewForBrowser(&browser);
  auto* adapter = view ? view->krypton_ui() : nullptr;
  if (!adapter || !adapter->ready() || !adapter->presented() ||
      !browser.GetProfile()->IsRegularProfile() ||
      !browser.GetTabStripModel()) {
    return Error("Window is outside the organization session.");
  }
  if (operation.name.starts_with("krypton_list_")) {
    return Read(operation, arguments, browser, *adapter, filter, origin);
  }
  if (!approved && (operation.confirm || requires_confirmation)) {
    return Confirm();
  }

  const auto id = ResolveCatalog('s', String(arguments, "space"));
  const auto folder = ResolveCatalog('f', String(arguments, "folder"));
  const auto parent = ResolveCatalog('f', String(arguments, "parent"));
  if (!id || !folder || !parent || (*id && !adapter->HostsSpace(*id)) ||
      (*folder && !adapter->folders_.Find(*folder)) ||
      (*parent && !adapter->folders_.Find(*parent))) {
    return Error("Unknown Space or folder.");
  }
  const auto name = operation.name;
  const auto label = String(arguments, "name");
  const int position = arguments.FindInt("position").value_or(0);

  if (name == "krypton_create_space") {
    if (adapter->hosted() || label.empty()) {
      return Error("A name and the profile's main window are required.");
    }
    krypton_ui::Workspace value;
    value.name = label;
    adapter->EditWorkspace(KR_WORKSPACE_UPSERT, std::move(value));
    return {.text = "Space created."};
  }
  if (name == "krypton_clear_space") {
    if (!*id || adapter->active_space_ != *id) {
      return Error("Clear applies to the active Space.");
    }
    adapter->ClearTemporaryTabs(*id);
    return {.text = "Clear requested."};
  }
  if (name == "krypton_delete_space") {
    if (adapter->hosted() || !*id || !adapter->FindSpace(*id) ||
        *id == krypton_ui::kDefaultWorkspace) {
      return Error("This Space cannot be removed.");
    }
    auto value = *adapter->FindSpace(*id);
    adapter->EditWorkspace(KR_WORKSPACE_REMOVE, std::move(value));
    return {.text = "Space removal requested."};
  }
  if (name == "krypton_new_note" || name == "krypton_new_easel") {
    if (!adapter->CanCreateDocument()) {
      return Error("Document creation is unavailable.");
    }
    adapter->NewDocument(name == "krypton_new_note"
                             ? krypton_ui::DocumentKind::kNote
                             : krypton_ui::DocumentKind::kEasel);
    return {.text = "Document creation requested."};
  }
  if (name == "krypton_apply_organization") {
    return Error("Atomic organization plans are unavailable.");
  }
  if (name == "krypton_open_tab") {
    GURL url(String(arguments, "url"));
    if (!url.SchemeIsHTTPOrHTTPS() || url.has_username() ||
        url.has_password()) {
      return Error("Only HTTP and HTTPS destinations are allowed.");
    }
    const auto space = *id ? *id : adapter->active_space_;
    if (const auto* destination =
            *folder ? adapter->folders_.Find(*folder) : nullptr;
        destination && destination->space != space) {
      return Error("The destination folder belongs to another Space.");
    }
    NavigateParams params(&browser, url, ui::PAGE_TRANSITION_AUTO_BOOKMARK);
    params.disposition = WindowOpenDisposition::kNewBackgroundTab;
    if (arguments.contains("position")) {
      params.tabstrip_index = position;
    }
    Navigate(&params);
    auto* tab = params.navigated_or_inserted_contents
                    ? tabs::TabInterface::MaybeGetFromContents(
                          params.navigated_or_inserted_contents)
                    : nullptr;
    if (!tab || tab->GetBrowserWindowInterface() != &browser) {
      return Error("The tab did not open in the shared window.");
    }
    adapter->AssignWorkspace(*tab, space);
    if (*folder) {
      adapter->AssignFolder(*tab, *folder);
    }
    return {.text = "Tab opened."};
  }
  if (name == "krypton_rename_space" || name == "krypton_switch_space") {
    const auto* space = *id ? adapter->FindSpace(*id) : nullptr;
    if (!space) {
      return Error("Unknown Space.");
    }
    if (name == "krypton_switch_space") {
      adapter->SwitchWorkspace(*id);
    } else {
      if (adapter->hosted() || label.empty()) {
        return Error("A name and the profile's main window are required.");
      }
      auto value = *space;
      value.name = label;
      adapter->EditWorkspace(KR_WORKSPACE_UPSERT, std::move(value));
    }
    return {.text = "Space updated."};
  }
  if (name == "krypton_create_folder" || name == "krypton_rename_folder" ||
      name == "krypton_move_folder" || name == "krypton_remove_folder") {
    const auto* current = *folder ? adapter->folders_.Find(*folder) : nullptr;
    const auto* parent_folder =
        *parent ? adapter->folders_.Find(*parent) : nullptr;
    const auto destination = parent_folder ? parent_folder->space : *id;
    if (!approved && current && destination && destination != current->space) {
      return Confirm();
    }
    if (!approved && current) {
      int affected = 0;
      for (auto* tab : *browser.GetTabStripModel()) {
        if (adapter->folders_.IsDescendant(
                krypton_ui::FolderForTab(tab->GetContents()), current->id) &&
            ++affected > 5) {
          return Confirm();
        }
      }
    }
    KrFolderCommand command{};
    command.struct_size = sizeof(command);
    command.folder = *folder;
    command.space = *id;
    command.parent = *parent;
    command.index = position;
    command.name = Wire(label);
    if (current) {
      command.glyph = Wire(current->glyph);
    }
    if (name == "krypton_create_folder") {
      command.kind = KR_FOLDER_CREATE;
    } else if (name == "krypton_rename_folder") {
      command.kind = KR_FOLDER_RENAME;
    } else {
      command.kind =
          name == "krypton_remove_folder" ? KR_FOLDER_DELETE : KR_FOLDER_MOVE;
    }
    return adapter->FolderCommand(command) == KR_OK
               ? Result{.text = "Folder updated."}
               : Error("Folder update was refused.");
  }

  std::vector<tabs::TabInterface*> tabs;
  base::flat_set<tabs::TabHandle> unique;
  const auto add_tab = [&](std::string_view handle) {
    auto* tab = Resolve(handle, browser);
    if (!tab || !unique.insert(tab->GetHandle()).second) {
      return false;
    }
    tabs.push_back(tab);
    return true;
  };
  if (const auto* list = arguments.FindList("tabs")) {
    for (const auto& entry : *list) {
      if (!add_tab(entry.GetString())) {
        return Error("Unknown or duplicate tab.");
      }
    }
  } else if (!add_tab(String(arguments, "tab"))) {
    return Error("Unknown tab.");
  }
  if (!approved && tabs.size() > 5) {
    return Confirm();
  }
  if (name == "krypton_activate_tab") {
    adapter->Activate(krypton_ui::BrowserAdapter::Identity(*tabs.front()));
    return {.text = "Tab activated."};
  }

  if (name == "krypton_open_glance") {
    const auto url = String(arguments, "url");
    const GURL parsed(url);
    if (!parsed.SchemeIsHTTPOrHTTPS() || parsed.has_username() ||
        parsed.has_password()) {
      return Error("Only HTTP and HTTPS destinations are allowed.");
    }
    KrMultiPageCommand command{};
    command.struct_size = sizeof(command);
    command.kind = KR_MULTI_PAGE_OPEN_GLANCE;
    command.tab = krypton_ui::BrowserAdapter::Identity(*tabs.front());
    command.url = Wire(url);
    return adapter->MultiPageCommand(command) == KR_OK
               ? Result{.text = "Glance opened."}
               : Error("Glance was refused.");
  }
  auto* model = browser.GetTabStripModel();
  if (name == "krypton_move_tabs") {
    const auto* destination_folder =
        *folder ? adapter->folders_.Find(*folder) : nullptr;
    for (auto* tab : tabs) {
      const auto space = krypton_ui::WorkspaceForTab(tab->GetContents());
      if ((*id && *id != space) ||
          (destination_folder && destination_folder->space != space)) {
        if (!approved) {
          return Confirm();
        }
      }
    }
    if (arguments.contains("position") && position >= model->count()) {
      return Error("Position is outside the tab strip.");
    }
  }
  if (name == "krypton_create_split") {
    const int layout = arguments.FindInt("layout").value_or(0);
    if (tabs.size() != 2 || layout > KR_SPLIT_MAIN_BOTTOM) {
      return Error("The split requires two tabs and a valid layout.");
    }
    if (krypton_ui::WorkspaceForTab(tabs[0]->GetContents()) !=
        krypton_ui::WorkspaceForTab(tabs[1]->GetContents())) {
      return Error("Split tabs must occupy the same Space.");
    }
    return adapter->JoinNativeSplit(
               krypton_ui::BrowserAdapter::Identity(*tabs[0]),
               krypton_ui::BrowserAdapter::Identity(*tabs[1]),
               static_cast<KrSplitLayout>(layout))
               ? Result{.text = "Split created."}
               : Error("Split was refused.");
  }

  Remember(browser, tabs);
  std::vector<tabs::TabHandle> targets(unique.begin(), unique.end());
  for (const auto handle : targets) {
    auto* tab = handle.Get();
    if (!tab || tab->GetBrowserWindowInterface() != &browser) {
      return Error("A tab is no longer in scope.");
    }
    const auto tab_id = krypton_ui::BrowserAdapter::Identity(*tab);
    if (name == "krypton_close_tabs") {
      adapter->CloseTab(tab_id);
    } else if (name == "krypton_move_tabs") {
      const auto* folder_entry =
          *folder ? adapter->folders_.Find(*folder) : nullptr;
      const auto space =
          *id ? *id
              : (folder_entry
                     ? folder_entry->space
                     : krypton_ui::WorkspaceForTab(tab->GetContents()));
      if (arguments.contains("position") || *id || folder_entry) {
        const int index =
            arguments.FindInt("position").value_or(model->GetIndexOfTab(tab));
        adapter->MoveTab(tab_id, space, std::min(index, model->count() - 1));
      }
      if (arguments.contains("folder")) {
        adapter->AssignFolder(*tab, *folder);
      }
    } else if (name == "krypton_pin_tabs" || name == "krypton_unpin_tabs") {
      const uint32_t flags = name == "krypton_pin_tabs" ? KR_TAB_PINNED : 0;
      adapter->SetTabFlags(tab_id, KR_TAB_PINNED, flags);
    } else if (name == "krypton_add_essential" ||
               name == "krypton_remove_essential") {
      adapter->SetEssential(*tab, name == "krypton_add_essential");
    } else if (name == "krypton_set_tab_icon") {
      if (auto* customization = adapter->site_customization()) {
        customization->SetTabIcon(tab_id, String(arguments, "glyph"));
      } else {
        return Error("Tab customization is unavailable.");
      }
    } else {
      return Error("The organization operation is unavailable.");
    }
  }
  return {.text = "Tabs updated."};
}

}  // namespace krypton_agent
