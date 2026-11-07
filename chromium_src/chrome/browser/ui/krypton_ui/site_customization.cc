// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/site_customization.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/pattern.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"
#include "chrome/browser/ui/krypton_ui/multi_page_controller.h"
#include "chrome/browser/ui/krypton_ui/multi_page_session.h"
#include "chrome/browser/ui/krypton_ui/workspace_session.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/common/chrome_render_frame.mojom.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service.h"
#include "components/sessions/content/session_tab_helper.h"
#include "components/tabs/public/tab_interface.h"
#include "components/vector_icons/vector_icons.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/re2/src/re2/re2.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/models/combobox_model.h"
#include "ui/base/models/image_model.h"
#include "ui/base/models/simple_combobox_model.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/mojom/ui_base_types.mojom.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/font_list.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/button/checkbox.h"
#include "ui/views/controls/combobox/combobox.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/textarea/textarea.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/layout_provider.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/dialog_delegate.h"

namespace krypton_ui {
namespace {
constexpr size_t kMaxRules = 256;
constexpr uint64_t kBoostIdPrefix = UINT64_C(0xb000000000000000);
constexpr uint64_t kRouteIdPrefix = UINT64_C(0xc000000000000000);
constexpr uint64_t kRuleIdMask = UINT64_C(0x0fffffffffffffff);
constexpr size_t kMaxCssBytes = 256 * 1024;
constexpr size_t kMaxCombinedCssBytes = 1024 * 1024;
enum class Match { kDomain, kContains, kExact, kRegex };
struct Rule {
  uint64_t id = 0, workspace = 0, edit_order = 0;
  Match match = Match::kDomain;
  std::string title, pattern, css;
  std::unique_ptr<re2::RE2> expression;
  bool enabled = true, new_tabs_only = false;
};
std::string String(const base::DictValue& row, std::string_view key) {
  const auto* value = row.FindString(key);
  return value ? *value : std::string();
}
uint64_t Number(const base::DictValue& row, std::string_view key) {
  return ParseWorkspaceId(String(row, key));
}
void StampBoost(base::DictValue& data, base::DictValue& row) {
  const auto order =
      std::min(Number(data, "boost_edit_order"), kRuleIdMask - 1) + 1;
  const auto value = base::NumberToString(order);
  data.Set("boost_edit_order", value);
  row.Set("edit_order", value);
}
std::string Trim(std::string_view value) {
  return std::string(base::TrimWhitespaceASCII(value, base::TRIM_ALL));
}
std::string Normalize(const GURL& url) {
  std::string value = url.GetWithoutRef().spec();
  const auto scheme = value.find("://");
  if (scheme != std::string::npos) {
    value.erase(0, scheme + 3);
  }
  while (value.ends_with('/')) {
    value.pop_back();
  }
  return value;
}
bool DomainMatches(const GURL& url, std::string_view pattern) {
  if (pattern.starts_with("*.")) {
    pattern.remove_prefix(2);
    return url.DomainIs(pattern);
  }
  return url.host() == pattern;
}
bool Matches(const Rule& rule, const GURL& url, bool enabled_only = true) {
  if ((enabled_only && !rule.enabled) || !url.SchemeIsHTTPOrHTTPS()) {
    return false;
  }
  switch (rule.match) {
    case Match::kDomain:
      return DomainMatches(url, rule.pattern);
    case Match::kContains:
      return base::ToLowerASCII(url.spec())
          .contains(base::ToLowerASCII(rule.pattern));
    case Match::kExact: {
      const GURL target(rule.pattern.contains("://")
                            ? rule.pattern
                            : "https://" + rule.pattern);
      return target.is_valid() && Normalize(url) == Normalize(target);
    }
    case Match::kRegex:
      return rule.expression &&
             re2::RE2::PartialMatch(url.spec(), *rule.expression);
  }
  return false;
}
std::u16string MenuLabel(std::string_view value) {
  auto label = base::UTF8ToUTF16(value);
  base::ReplaceSubstringsAfterOffset(&label, 0, u"&", u"&&");
  return label;
}
KrStr Borrow(std::string_view value) {
  return {value.data(), value.size()};
}
struct BoostValue {
  KrBoost data{.struct_size = sizeof(KrBoost),
               .size = 100,
               .enabled = true,
               .contrast = .75f,
               .brightness = .5f,
               .saturation = .5f,
               .palette_hue = .365583333f,
               .palette_distance = .91f,
               .secondary_rotation = .152777778f};
  std::string name, pattern, css, compiled, zapped, focused;
  KrBoost View() const {
    auto value = data;
    value.name = Borrow(name);
    value.pattern = Borrow(pattern);
    value.css = Borrow(css);
    value.compiled_css = Borrow(compiled);
    value.zapped = Borrow(zapped);
    value.focused = Borrow(focused);
    return value;
  }
  base::DictValue Record() const {
    base::DictValue row;
    row.Set("title", name);
    row.Set("pattern", pattern);
    row.Set("match", static_cast<int>(data.match));
    row.Set("css", compiled);
    row.Set("custom_css", css);
    row.Set("zapped", zapped);
    row.Set("focused", focused);
    row.Set("background", base::NumberToString(data.background));
    row.Set("foreground", base::NumberToString(data.foreground));
    row.Set("accent", base::NumberToString(data.accent));
    row.Set("font", static_cast<int>(data.font));
    row.Set("size", static_cast<int>(data.size));
    row.Set("letter_case", static_cast<int>(data.letter_case));
    row.Set("appearance", static_cast<int>(data.appearance));
    row.Set("enabled", data.enabled);
    row.Set("contrast", data.contrast);
    row.Set("brightness", data.brightness);
    row.Set("saturation", data.saturation);
    row.Set("palette_hue", data.palette_hue);
    row.Set("palette_distance", data.palette_distance);
    row.Set("secondary_rotation", data.secondary_rotation);
    row.Set("invert_colors", data.invert_colors);
    row.Set("color_enabled", data.color_enabled);
    row.Set("auto_theme", data.auto_theme);
    row.Set("palette_set", data.palette_set);
    return row;
  }
};
std::optional<BoostValue> CopyBoost(const KrBoost* input) {
  if (!input || !KR_HAS_FIELD(input, KrBoost, enabled) || !input->tab ||
      input->font >= 20 || input->size < 50 || input->size > 200 ||
      input->letter_case > 3 || input->appearance > 2 || input->match > 3) {
    return std::nullopt;
  }
  for (const auto value :
       {input->name, input->pattern, input->css, input->compiled_css,
        input->zapped, input->focused}) {
    if ((!value.data && value.size) || value.size > kMaxCombinedCssBytes) {
      return std::nullopt;
    }
  }
  BoostValue value;
  value.data.id = input->id;
  value.data.revision = input->revision;
  value.data.tab = input->tab;
  value.data.serial = input->serial;
  value.data.background = input->background;
  value.data.foreground = input->foreground;
  value.data.accent = input->accent;
  value.data.font = input->font;
  value.data.size = input->size;
  value.data.letter_case = input->letter_case;
  value.data.appearance = input->appearance;
  value.data.match = input->match;
  value.data.enabled = input->enabled;
  if (KR_HAS_FIELD(input, KrBoost, palette_set)) {
    const auto normalized = [](float v) {
      return std::isfinite(v) && v >= 0 && v <= 1;
    };
    for (float v : {input->contrast, input->brightness, input->saturation,
                    input->palette_hue, input->palette_distance,
                    input->secondary_rotation}) {
      if (!normalized(v)) {
        return std::nullopt;
      }
    }
    value.data.contrast = input->contrast;
    value.data.brightness = input->brightness;
    value.data.saturation = input->saturation;
    value.data.palette_hue = input->palette_hue;
    value.data.palette_distance = input->palette_distance;
    value.data.secondary_rotation = input->secondary_rotation;
    value.data.invert_colors = input->invert_colors;
    value.data.color_enabled = input->color_enabled;
    value.data.auto_theme = input->auto_theme;
    value.data.palette_set = input->palette_set;
  } else {
    value.data.color_enabled = input->background || input->foreground;
  }
  const auto copy = [](KrStr str) {
    return str.data ? std::string(str.data, str.size) : std::string();
  };
  value.name = copy(input->name);
  value.pattern = Trim(copy(input->pattern));
  value.css = copy(input->css);
  value.compiled = copy(input->compiled_css);
  value.zapped = copy(input->zapped);
  value.focused = copy(input->focused);
  if (value.name.empty() || value.name.size() > 256 || value.pattern.empty() ||
      value.pattern.size() > 2048 || value.css.size() > kMaxCssBytes ||
      value.zapped.size() > 16384 || value.focused.size() > 4096) {
    return std::nullopt;
  }
  for (const auto* text : {&value.name, &value.pattern, &value.css,
                           &value.compiled, &value.zapped, &value.focused}) {
    if (!base::IsStringUTF8(*text) || text->contains('\0')) {
      return std::nullopt;
    }
  }
  if (input->match == 0) {
    value.pattern = base::ToLowerASCII(value.pattern);
    const auto host = value.pattern.starts_with("*.") ? value.pattern.substr(2)
                                                      : value.pattern;
    const GURL url("https://" + host + "/");
    if (!url.is_valid() || url.host() != host ||
        value.pattern.find_first_of("/?#:@ ") != std::string::npos) {
      return std::nullopt;
    }
  } else if (input->match == 2) {
    const GURL url(value.pattern.contains("://") ? value.pattern
                                                 : "https://" + value.pattern);
    if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
      return std::nullopt;
    }
  } else if (input->match == 3) {
    re2::RE2::Options options;
    options.set_log_errors(false);
    re2::RE2 expression(value.pattern, options);
    if (!expression.ok()) {
      return std::nullopt;
    }
  }
  return value;
}
views::Textfield* Field(views::View& form,
                        std::u16string_view label,
                        std::string_view value) {
  auto caption = std::make_unique<views::Label>(std::u16string(label));
  caption->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  form.AddChildView(std::move(caption));
  auto* field = form.AddChildView(std::make_unique<views::Textfield>());
  field->SetText(base::UTF8ToUTF16(value));
  field->GetViewAccessibility().SetName(std::u16string(label));
  return field;
}
views::Combobox* Choices(views::View& form,
                         std::u16string_view label,
                         std::vector<std::u16string> names,
                         size_t selected) {
  auto caption = std::make_unique<views::Label>(std::u16string(label));
  caption->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  form.AddChildView(std::move(caption));
  std::vector<ui::SimpleComboboxModel::Item> items;
  for (auto& name : names) {
    items.emplace_back(std::move(name));
  }
  auto* combo = form.AddChildView(std::make_unique<views::Combobox>(
      std::make_unique<ui::SimpleComboboxModel>(std::move(items))));
  combo->GetViewAccessibility().SetName(std::u16string(label));
  combo->SetSelectedIndex(selected);
  return combo;
}
}  // namespace

class SiteCustomization::Impl {
 public:
  explicit Impl(BrowserAdapter& owner) : owner_(owner) {
    prefs_.Init(Prefs());
    prefs_.Add(kSiteCustomizationPref,
               base::BindRepeating(&Impl::Changed, weak_.GetWeakPtr()));
    Reload();
  }
  ~Impl() {
    weak_.InvalidateWeakPtrs();
    if (editor_) {
      editor_->Close();
    }
  }
  PrefService* Prefs() { return owner_->browser_->GetProfile()->GetPrefs(); }
  bool Private() const {
    return owner_->browser_->GetProfile()->IsOffTheRecord();
  }
  const Rule* Find(bool boost, uint64_t id) const {
    const auto& rules = boost ? boosts_ : routes_;
    const auto found = std::ranges::find(rules, id, &Rule::id);
    return found == rules.end() ? nullptr : &*found;
  }
  void Reload() {
    ++revision_;
    routes_.clear();
    boosts_.clear();
    const auto& data = Prefs()->GetDict(kSiteCustomizationPref);
    if (data.FindInt("version").value_or(1) != 1) {
      return;
    }
    for (bool boost : {false, true}) {
      const auto* list = data.FindList(boost ? "boosts" : "routes");
      if (!list) {
        continue;
      }
      auto& rules = boost ? boosts_ : routes_;
      for (const auto& item : *list) {
        const auto* row = item.GetIfDict();
        if (!row || rules.size() >= kMaxRules) {
          continue;
        }
        Rule rule;
        rule.id = Number(*row, "id");
        rule.edit_order = Number(*row, "edit_order");
        rule.workspace = Number(*row, "workspace");
        rule.title = String(*row, "title");
        rule.pattern = String(*row, "pattern");
        rule.css = String(*row, "css");
        rule.enabled = row->FindBool("enabled").value_or(true);
        rule.new_tabs_only = row->FindBool("new_tabs_only").value_or(false);
        const auto match = row->FindInt("match").value_or(0);
        if (!rule.id || rule.id > kRuleIdMask || rule.pattern.empty() ||
            rule.pattern.size() > 2048 || match < 0 || match > 3 ||
            rule.css.size() > kMaxCssBytes ||
            std::ranges::any_of(
                rules, [&](const auto& r) { return r.id == rule.id; })) {
          continue;
        }
        rule.match = static_cast<Match>(match);
        if (rule.match == Match::kRegex) {
          re2::RE2::Options options;
          options.set_case_sensitive(false);
          options.set_log_errors(false);
          rule.expression = std::make_unique<re2::RE2>(rule.pattern, options);
          if (!rule.expression->ok()) {
            continue;
          }
        }
        rules.push_back(std::move(rule));
      }
    }
  }
  void Changed() {
    Reload();
    PublishBoosts();
    owner_->ScheduleActions();
    for (auto* tab : *owner_->browser_->GetTabStripModel()) {
      owner_->PublishTab(*tab);
      // Explicit preference changes are batched by page, outside input
      // dispatch.
      std::ignore = owner_->session_.Defer(base::BindOnce(
          [](base::WeakPtr<Impl> self, base::WeakPtr<tabs::TabInterface> tab) {
            if (!self || !tab) {
              return;
            }
            tab->GetContents()->ForEachRenderFrameHost(
                [&](content::RenderFrameHost* frame) {
                  self->ApplyStyles(*frame, frame->GetLastCommittedURL());
                });
          },
          weak_.GetWeakPtr(), tab->GetWeakPtr()));
    }
  }
  void ApplyStyles(content::RenderFrameHost& frame, const GURL& url) {
    if (!frame.IsRenderFrameLive() || !url.SchemeIsHTTPOrHTTPS()) {
      return;
    }
    const auto* target =
        preview_ ? owner_->FindTab(preview_->data.tab) : nullptr;
    const bool preview = target && content::WebContents::FromRenderFrameHost(
                                       &frame) == target->GetContents();
    std::vector<std::string> sheets;
    size_t bytes = 0;
    for (const auto& rule : boosts_) {
      if ((!preview || rule.id != preview_->data.id) && Matches(rule, url) &&
          bytes + rule.css.size() <= kMaxCombinedCssBytes) {
        sheets.push_back(rule.css);
        bytes += rule.css.size();
      }
    }
    if (preview && preview_->data.enabled) {
      Rule staged;
      staged.enabled = true;
      staged.match = static_cast<Match>(preview_->data.match);
      staged.pattern = preview_->pattern;
      if (staged.match == Match::kRegex) {
        re2::RE2::Options options;
        options.set_log_errors(false);
        options.set_case_sensitive(false);
        staged.expression = std::make_unique<re2::RE2>(staged.pattern, options);
      }
      if (Matches(staged, url) &&
          bytes + preview_->compiled.size() <= kMaxCombinedCssBytes) {
        sheets.push_back(preview_->compiled);
      }
    }
    mojo::AssociatedRemote<chrome::mojom::ChromeRenderFrame> remote;
    frame.GetRemoteAssociatedInterfaces()->GetInterface(&remote);
    remote->SetKryptonSiteStyle(url.GetWithoutRef(), sheets);
  }
  void NavigationFinished(tabs::TabInterface& tab,
                          content::NavigationHandle& nav) {
    if (!nav.HasCommitted() || nav.IsErrorPage()) {
      return;
    }
    ApplyStyles(*nav.GetRenderFrameHost(), nav.GetURL());
    if (Private() || !nav.IsInPrimaryMainFrame() || nav.IsSameDocument() ||
        EssentialForTab(tab.GetContents()) ||
        IsTransientGlance(tab.GetContents())) {
      return;
    }
    for (const auto& rule : routes_) {
      if ((rule.workspace && !owner_->workspaces_.Contains(rule.workspace)) ||
          !Matches(rule, nav.GetURL()) ||
          (rule.new_tabs_only &&
           nav.GetPreviousPrimaryMainFrameURL().SchemeIsHTTPOrHTTPS())) {
        continue;
      }
      if (!rule.workspace) {
        return;
      }
      if (WorkspaceForTab(tab.GetContents()) == rule.workspace) {
        return;
      }
      const bool selected = tab.IsActivated();
      const auto id = BrowserAdapter::Identity(tab);
      if (owner_->multi_page_ && owner_->multi_page_->Describe(id)) {
        if (!owner_->multi_page_->MoveGroup(id, rule.workspace)) {
          return;
        }
      } else {
        owner_->AssignWorkspace(tab, rule.workspace);
        owner_->PublishTab(tab);
      }
      if (selected) {
        owner_->SwitchWorkspace(rule.workspace);
      }
      return;
    }
  }
  void PublishBoosts() {
    const auto* api = owner_->session_.api();
    if (!api || !KR_HAS_SLOT(api, KrShellUI, remove_library_item) ||
        !api->set_library_section || !api->upsert_library_item ||
        !api->remove_library_item) {
      return;
    }
    const KrLibrarySectionState section{sizeof(section), KR_LIBRARY_BOOSTS,
                                        KR_LIBRARY_SECTION_ENABLED};
    api->set_library_section(api->self, &section);
    for (const auto id : published_) {
      if (!Find((id & ~kRuleIdMask) == kBoostIdPrefix, id & kRuleIdMask)) {
        api->remove_library_item(api->self, id);
      }
    }
    published_.clear();
    for (const auto& rule : boosts_) {
      const KrLibraryItem item{
          sizeof(item),         kBoostIdPrefix | rule.id,
          KR_LIBRARY_BOOSTS,    rule.enabled ? KR_LIBRARY_ITEM_ENABLED : 0u,
          Borrow(rule.title),   Borrow(rule.pattern),
          Borrow(rule.pattern), 0};
      api->upsert_library_item(api->self, &item, 0);
      published_.push_back(kBoostIdPrefix | rule.id);
    }
    const KrLibrarySectionState routing{
        sizeof(routing), KR_LIBRARY_ROUTING,
        static_cast<uint32_t>(Private() ? KR_LIBRARY_SECTION_HIDDEN
                                        : KR_LIBRARY_SECTION_ENABLED)};
    if (api->set_library_section(api->self, &routing) != KR_OK || Private()) {
      return;
    }
    for (const auto& rule : routes_) {
      auto subtitle = rule.pattern;
      if (!rule.workspace) {
        subtitle += " \xE2\x86\x92 Most recent workspace";
      }
      for (const auto& space : owner_->workspaces_.spaces) {
        if (space.id == rule.workspace) {
          subtitle += " \xE2\x86\x92 " + space.name;
          break;
        }
      }
      const KrLibraryItem item{sizeof(item),
                               kRouteIdPrefix | rule.id,
                               KR_LIBRARY_ROUTING,
                               rule.enabled ? KR_LIBRARY_ITEM_ENABLED : 0u,
                               Borrow(rule.title),
                               Borrow(subtitle),
                               {},
                               0};
      api->upsert_library_item(api->self, &item, 0);
      published_.push_back(kRouteIdPrefix | rule.id);
    }
  }
  void DismissLibrary() {
    const auto* api = owner_->session_.api();
    if (api && KR_HAS_SLOT(api, KrShellUI, dismiss_library) &&
        api->dismiss_library) {
      api->dismiss_library(api->self);
    }
  }
  void LibraryCommand(uint32_t section,
                      KrLibraryItemId item,
                      uint32_t command) {
    if (section != KR_LIBRARY_BOOSTS && section != KR_LIBRARY_ROUTING) {
      return;
    }
    const bool boost = section == KR_LIBRARY_BOOSTS;
    if (item &&
        (item & ~kRuleIdMask) != (boost ? kBoostIdPrefix : kRouteIdPrefix)) {
      return;
    }
    const auto id = item & kRuleIdMask;
    if (command == KR_LIBRARY_TOGGLE || command == KR_LIBRARY_REMOVE) {
      Mutate(boost, id, command == KR_LIBRARY_REMOVE);
    } else if (command == KR_LIBRARY_OPEN ||
               (command == KR_LIBRARY_CREATE && !item)) {
      if (boost) {
        DismissLibrary();
      }
      auto* active = owner_->browser_->GetActiveTabInterface();
      const auto tab =
          active ? active->GetWeakPtr() : base::WeakPtr<tabs::TabInterface>();
      const auto url =
          active ? active->GetContents()->GetLastCommittedURL() : GURL();
      std::ignore = owner_->session_.Defer(base::BindOnce(
          [](base::WeakPtr<Impl> self, bool boost, uint64_t id,
             uint64_t revision, base::WeakPtr<tabs::TabInterface> tab,
             GURL url) {
            if (!self || self->revision_ != revision) {
              return;
            }
            if (boost &&
                (!tab ||
                 self->owner_->browser_->GetActiveTabInterface() != tab.get() ||
                 tab->GetContents()->GetLastCommittedURL() != url)) {
              return;
            }
            self->Edit(boost, id, url);
          },
          weak_.GetWeakPtr(), boost, id, revision_, tab, url));
    }
  }
  void Mutate(bool boost, uint64_t id, bool remove) {
    if (Private()) {
      return;
    }
    auto data = Prefs()->GetDict(kSiteCustomizationPref).Clone();
    auto* list = data.FindList(boost ? "boosts" : "routes");
    if (!list) {
      return;
    }
    for (auto it = list->begin(); it != list->end(); ++it) {
      auto* row = it->GetIfDict();
      if (!row || Number(*row, "id") != id) {
        continue;
      }
      if (remove) {
        list->erase(it);
      } else {
        row->Set("enabled", !row->FindBool("enabled").value_or(true));
        if (boost && row->FindBool("enabled").value_or(false)) {
          StampBoost(data, *row);
        }
      }
      Prefs()->SetDict(kSiteCustomizationPref, std::move(data));
      return;
    }
  }
  bool Save(bool boost, uint64_t id, uint64_t expected, base::DictValue row) {
    if (Private() || expected != revision_) {
      return false;
    }
    auto data = Prefs()->GetDict(kSiteCustomizationPref).Clone();
    if (data.FindInt("version").value_or(1) != 1) {
      return false;
    }
    data.Set("version", 1);
    if (boost) {
      StampBoost(data, row);
    }
    auto* list = data.FindList(boost ? "boosts" : "routes");
    if (!list) {
      list =
          data.Set(boost ? "boosts" : "routes", base::ListValue())->GetIfList();
    }
    if (!id) {
      if (list->size() >= kMaxRules) {
        return false;
      }
      id = 1;
      for (const auto& item : *list) {
        if (const auto* r = item.GetIfDict()) {
          const auto used = Number(*r, "id");
          if (used >= kRuleIdMask) {
            return false;
          }
          id = std::max(id, used + 1);
        }
      }
      row.Set("id", base::NumberToString(id));
      list->Append(std::move(row));
    } else {
      bool found = false;
      for (auto& item : *list) {
        if (auto* r = item.GetIfDict(); r && Number(*r, "id") == id) {
          row.Set("id", base::NumberToString(id));
          *r = std::move(row);
          found = true;
          break;
        }
      }
      if (!found) {
        return false;
      }
    }
    Prefs()->SetDict(kSiteCustomizationPref, std::move(data));
    return true;
  }
  void Edit(bool boost, uint64_t id, GURL initial) {
    if (boost) {
      ShowBoost(id, initial);
      return;
    }

    if (Private()) {
      return;
    }
    const auto* rule = id ? Find(boost, id) : nullptr;
    if (id && !rule) {
      return;
    }
    const auto* api = owner_->session_.api();
    if (api && KR_HAS_SLOT(api, KrShellUI, show_routing_editor) &&
        api->show_routing_editor) {
      const auto pattern = rule ? rule->pattern : initial.host();
      const KrRoutingRule draft{sizeof(draft),
                                id,
                                revision_,
                                Borrow(pattern),
                                rule ? rule->workspace : 0,
                                rule ? static_cast<uint32_t>(rule->match) : 1u,
                                !rule || rule->enabled,
                                rule && rule->new_tabs_only};
      api->show_routing_editor(api->self, &draft);
      return;
    }
    if (editor_) {
      editor_->Close();
    }
    auto dialog = std::make_unique<views::DialogDelegate>();
    dialog->SetTitle(boost ? u"Site boost" : u"Workspace routing");
    dialog->SetModalType(ui::mojom::ModalType::kWindow);
    dialog->SetButtonLabel(ui::mojom::DialogButton::kOk, u"Save");
    auto form = std::make_unique<views::View>();
    auto* provider = views::LayoutProvider::Get();
    form->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical,
        provider->GetDialogInsetsForContentType(
            views::DialogContentType::kControl,
            views::DialogContentType::kControl),
        provider->GetDistanceMetric(views::DISTANCE_RELATED_CONTROL_VERTICAL)));
    auto* title = Field(*form, u"Name", rule ? rule->title : initial.host());
    auto* pattern = Field(*form, u"Site or URL pattern",
                          rule ? rule->pattern : initial.host());
    auto* match =
        Choices(*form, u"Match",
                {u"Domain (use *.example.com for subdomains)", u"URL contains",
                 u"URL equals", u"Regular expression"},
                rule ? static_cast<size_t>(rule->match) : 0);
    views::Combobox* workspace = nullptr;
    views::Checkbox* new_only = nullptr;
    views::Textarea* css = nullptr;
    std::vector<KrSpaceId> spaces;
    if (boost) {
      auto caption = std::make_unique<views::Label>(
          u"CSS (colors, fonts, layout and selectors to hide with display: "
          u"none)");
      caption->SetHorizontalAlignment(gfx::ALIGN_LEFT);
      caption->SetMultiLine(true);
      form->AddChildView(std::move(caption));
      css = form->AddChildView(std::make_unique<views::Textarea>());
      css->GetViewAccessibility().SetName(u"Boost CSS");
      css->SetText(base::UTF8ToUTF16(
          rule ? rule->css : "/* Example: body { font-size: 18px; } */\n"));
      css->SetPreferredSize(
          gfx::Size(provider->GetDistanceMetric(
                        views::DISTANCE_MODAL_DIALOG_PREFERRED_WIDTH),
                    css->GetFontList().GetHeight() * 10));
    } else {
      std::vector<std::u16string> names;
      size_t selected = 0;
      for (const auto& space : owner_->workspaces_.spaces) {
        if (space.id == (rule ? rule->workspace : owner_->active_space_)) {
          selected = spaces.size();
        }
        spaces.push_back(space.id);
        names.push_back(base::UTF8ToUTF16(space.name));
      }
      workspace =
          Choices(*form, u"Destination workspace", std::move(names), selected);
      new_only = form->AddChildView(
          std::make_unique<views::Checkbox>(u"Only route newly opened tabs"));
      new_only->SetChecked(rule && rule->new_tabs_only);
    }
    auto* enabled =
        form->AddChildView(std::make_unique<views::Checkbox>(u"Enabled"));
    enabled->SetChecked(!rule || rule->enabled);
    auto* error = form->AddChildView(std::make_unique<views::Label>());
    error->SetMultiLine(true);
    error->SetVisible(false);
    dialog->SetInitiallyFocusedView(title);
    dialog->SetAcceptCallbackWithClose(base::BindRepeating(
        [](base::WeakPtr<Impl> self, bool boost, uint64_t id, uint64_t expected,
           views::Textfield* title, views::Textfield* pattern,
           views::Combobox* match, views::Combobox* workspace,
           std::vector<KrSpaceId> spaces, views::Checkbox* new_only,
           views::Textarea* css, views::Checkbox* enabled,
           views::Label* error) {
          if (!self) {
            return true;
          }
          const auto name = Trim(base::UTF16ToUTF8(title->GetText()));
          auto reference = Trim(base::UTF16ToUTF8(pattern->GetText()));
          const auto mode = match->GetSelectedIndex().value_or(0);
          std::string problem;
          if (name.empty() || name.size() > 256 || reference.empty() ||
              reference.size() > 2048) {
            problem = "Enter a name and a site or URL pattern.";
          }
          if (mode == 0) {
            reference = base::ToLowerASCII(reference);
            const auto host =
                reference.starts_with("*.") ? reference.substr(2) : reference;
            const GURL url("https://" + host + "/");
            if (!url.is_valid() || url.host() != host || url.has_username() ||
                reference.find_first_of("/?#:@ ") != std::string::npos) {
              problem =
                  "Enter a hostname, for example example.com or *.example.com.";
            }
          } else if (mode == 2) {
            const GURL url(reference.contains("://") ? reference
                                                     : "https://" + reference);
            if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
              problem = "Enter an HTTP or HTTPS URL.";
            }
          } else if (mode == 3) {
            re2::RE2::Options options;
            options.set_log_errors(false);
            re2::RE2 expression(reference, options);
            if (!expression.ok()) {
              problem = "Invalid regular expression: " + expression.error();
            }
          }
          const auto code =
              css ? base::UTF16ToUTF8(css->GetText()) : std::string();
          if (code.size() > kMaxCssBytes) {
            problem = "The stylesheet must be smaller than 256 KiB.";
          }
          base::DictValue row;
          row.Set("title", name);
          row.Set("pattern", reference);
          row.Set("match", static_cast<int>(mode));
          row.Set("enabled", enabled->GetChecked());
          if (boost) {
            row.Set("css", code);
          } else {
            const auto destination =
                workspace->GetSelectedIndex().value_or(spaces.size());
            if (destination >= spaces.size() ||
                !self->owner_->workspaces_.Contains(spaces[destination])) {
              problem = "Choose an existing workspace.";
            } else {
              row.Set("workspace", base::NumberToString(spaces[destination]));
            }
            row.Set("new_tabs_only", new_only->GetChecked());
          }
          if (problem.empty() &&
              self->Save(boost, id, expected, std::move(row))) {
            return true;
          }
          if (problem.empty()) {
            problem =
                "Rules changed in another window. Reopen this editor "
                "before saving.";
          }
          error->SetText(base::UTF8ToUTF16(problem));
          error->SetVisible(true);
          return false;
        },
        weak_.GetWeakPtr(), boost, id, revision_, title, pattern, match,
        workspace, std::move(spaces), new_only, css, enabled, error));
    dialog->SetContentsView(std::move(form));
    auto* widget = views::DialogDelegate::CreateDialogWidget(
        std::move(dialog), owner_->view_->GetNativeWindow(),
        owner_->view_->GetNativeWindow());
    editor_ = widget->GetWeakPtr();
    widget->Show();
  }

  const base::DictValue* BoostRecord(uint64_t id) {
    const auto* list =
        Prefs()->GetDict(kSiteCustomizationPref).FindList("boosts");
    if (list) {
      for (const auto& item : *list) {
        if (const auto* row = item.GetIfDict();
            row && Number(*row, "id") == id) {
          return row;
        }
      }
    }
    return nullptr;
  }
  KrResult SaveRoutingRule(const KrRoutingRule* input) {
    if (!input || !KR_HAS_FIELD(input, KrRoutingRule, new_tabs_only) ||
        Private() || input->revision != revision_ || input->match > 3 ||
        (!input->pattern.data && input->pattern.size) ||
        input->pattern.size > 2048 ||
        (input->workspace && !owner_->workspaces_.Contains(input->workspace))) {
      return KR_ERR_INVALID_ARGUMENT;
    }
    auto reference = Trim(input->pattern.data ? std::string(input->pattern.data,
                                                            input->pattern.size)
                                              : std::string());
    if (reference.empty() || reference.contains('\0') ||
        !base::IsStringUTF8(reference)) {
      return KR_ERR_INVALID_ARGUMENT;
    }
    if (input->match == 0) {
      reference = base::ToLowerASCII(reference);
      const auto host =
          reference.starts_with("*.") ? reference.substr(2) : reference;
      const GURL url("https://" + host + "/");
      if (!url.is_valid() || url.host() != host ||
          reference.find_first_of("/?#:@ ") != std::string::npos) {
        return KR_ERR_INVALID_ARGUMENT;
      }
    } else if (input->match == 2) {
      const GURL url(reference.contains("://") ? reference
                                               : "https://" + reference);
      if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
        return KR_ERR_INVALID_ARGUMENT;
      }
      reference = url.spec();
    } else if (input->match == 3) {
      re2::RE2::Options options;
      options.set_log_errors(false);
      re2::RE2 expression(reference, options);
      if (!expression.ok()) {
        return KR_ERR_INVALID_ARGUMENT;
      }
    }
    base::DictValue row;
    const auto* previous = input->id ? Find(false, input->id) : nullptr;
    row.Set("title",
            previous && !previous->title.empty() ? previous->title : reference);
    row.Set("pattern", reference);
    row.Set("workspace", base::NumberToString(input->workspace));
    row.Set("match", static_cast<int>(input->match));
    row.Set("enabled", input->enabled);
    row.Set("new_tabs_only", input->new_tabs_only);
    return Save(false, input->id, input->revision, std::move(row))
               ? KR_OK
               : KR_ERR_INVALID_ARGUMENT;
  }
  const Rule* BoostToEdit(const GURL& url) const {
    const Rule* selected = nullptr;
    for (auto it = boosts_.rbegin(); it != boosts_.rend(); ++it) {
      if (!Matches(*it, url, false)) {
        continue;
      }
      const bool active = it->enabled && !it->css.empty();
      const bool selected_active =
          selected && selected->enabled && !selected->css.empty();
      if (!selected || (active && !selected_active) ||
          (active == selected_active &&
           it->edit_order > selected->edit_order)) {
        selected = &*it;
      }
    }
    return selected;
  }
  SiteCustomization::BoostState BoostForSite(const GURL& url) const {
    SiteCustomization::BoostState state;
    if (Private()) {
      return state;
    }
    for (const auto& rule : boosts_) {
      if (!rule.css.empty() && Matches(rule, url, false)) {
        state.available = true;
        state.active |= rule.enabled;
      }
    }
    return state;
  }
  void OpenBoost() {
    auto* tab = owner_->browser_->GetActiveTabInterface();
    if (!tab || Private()) {
      return;
    }
    const auto* rule = BoostToEdit(tab->GetContents()->GetLastCommittedURL());
    LibraryCommand(KR_LIBRARY_BOOSTS, rule ? kBoostIdPrefix | rule->id : 0,
                   rule ? KR_LIBRARY_OPEN : KR_LIBRARY_CREATE);
  }
  void ShowBoost(uint64_t id, const GURL& initial) {
    auto* tab = owner_->browser_->GetActiveTabInterface();
    const auto* api = owner_->session_.api();
    const auto* rule = id ? Find(true, id) : nullptr;
    if (!tab || Private() || (id && !rule) || !api ||
        !KR_HAS_SLOT(api, KrShellUI, show_boost_editor) ||
        !api->show_boost_editor) {
      return;
    }
    const auto url = initial.SchemeIsHTTPOrHTTPS()
                         ? initial
                         : tab->GetContents()->GetLastCommittedURL();
    if (!url.SchemeIsHTTPOrHTTPS()) {
      return;
    }
    BoostValue value;
    value.data.id = id;
    value.data.revision = revision_;
    value.data.tab = BrowserAdapter::Identity(*tab);
    value.name = rule ? rule->title : "My Boost";
    value.pattern = rule ? rule->pattern : url.host();
    if (const auto* row = BoostRecord(id)) {
      value.css = row->FindString("custom_css") ? String(*row, "custom_css")
                                                : String(*row, "css");
      value.compiled = String(*row, "css");
      value.zapped = String(*row, "zapped");
      value.focused = String(*row, "focused");
      value.data.background = static_cast<KrColor>(Number(*row, "background"));
      value.data.foreground = static_cast<KrColor>(Number(*row, "foreground"));
      value.data.accent = static_cast<KrColor>(Number(*row, "accent"));
      value.data.font = row->FindInt("font").value_or(0);
      value.data.size = row->FindInt("size").value_or(100);
      value.data.letter_case = row->FindInt("letter_case").value_or(0);
      value.data.appearance = row->FindInt("appearance").value_or(0);
      value.data.match = row->FindInt("match").value_or(0);
      value.data.enabled = row->FindBool("enabled").value_or(true);
      value.data.contrast =
          static_cast<float>(row->FindDouble("contrast").value_or(.75));
      value.data.brightness =
          static_cast<float>(row->FindDouble("brightness").value_or(.5));
      value.data.saturation =
          static_cast<float>(row->FindDouble("saturation").value_or(.5));
      value.data.palette_hue = static_cast<float>(
          row->FindDouble("palette_hue").value_or(.365583333));
      value.data.palette_distance =
          static_cast<float>(row->FindDouble("palette_distance").value_or(.91));
      value.data.secondary_rotation = static_cast<float>(
          row->FindDouble("secondary_rotation").value_or(.152777778));
      value.data.invert_colors = row->FindBool("invert_colors").value_or(false);
      value.data.color_enabled =
          row->FindBool("color_enabled")
              .value_or(value.data.background || value.data.foreground);
      value.data.auto_theme = row->FindBool("auto_theme").value_or(false);
      value.data.palette_set = row->FindBool("palette_set").value_or(false);
    }
    const auto draft = value.View();
    api->show_boost_editor(api->self, &draft);
  }
  void RefreshPreviewPage(KrTabId id) {
    auto* tab = owner_->FindTab(id);
    if (tab) {
      tab->GetContents()->ForEachRenderFrameHost(
          [&](content::RenderFrameHost* frame) {
            ApplyStyles(*frame, frame->GetLastCommittedURL());
          });
    }
  }
  void QueuePreview() {
    if (preview_scheduled_) {
      return;
    }
    preview_scheduled_ = true;
    std::ignore = owner_->session_.Defer(base::BindOnce(
        [](base::WeakPtr<Impl> self) {
          if (!self) {
            return;
          }
          self->preview_scheduled_ = false;
          if (self->preview_) {
            self->RefreshPreviewPage(self->preview_->data.tab);
          }
        },
        weak_.GetWeakPtr()));
  }
  KrResult EditBoost(uint32_t kind, const KrBoost* input) {
    if (kind == KR_BOOST_CANCEL) {
      if (!input || !KR_HAS_FIELD(input, KrBoost, enabled)) {
        return KR_ERR_INVALID_ARGUMENT;
      }
      if (preview_ && input->serial == preview_->data.serial &&
          input->tab == preview_->data.tab) {
        const auto id = preview_->data.tab;
        ++picker_generation_;
        if (picker_remote_) {
          picker_remote_->CancelKryptonBoostPicker();
          picker_remote_.reset();
        }
        preview_.reset();
        RefreshPreviewPage(id);
      }
      return KR_OK;
    }

    auto value = CopyBoost(input);
    if (!value || kind > KR_BOOST_SHARE || Private() ||
        value->data.revision != revision_ ||
        !owner_->FindTab(value->data.tab)) {
      return KR_ERR_INVALID_ARGUMENT;
    }
    const auto id = value->data.tab;
    if (kind == KR_BOOST_PREVIEW) {
      preview_ = std::move(*value);
      QueuePreview();
      return KR_OK;
    }
    if (kind == KR_BOOST_SHARE) {
      const auto json = base::WriteJsonWithOptions(
          value->Record(), base::JSONWriter::OPTIONS_PRETTY_PRINT);
      if (!json) {
        return KR_ERR_PLATFORM;
      }
      ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
      writer.WriteText(base::UTF8ToUTF16(*json));
      owner_->Notify("Boost copied");
      return KR_OK;
    }
    if (kind == KR_BOOST_ZAP || kind == KR_BOOST_FOCUS) {
      preview_ = std::move(*value);
      PickElement(kind == KR_BOOST_FOCUS);
      return KR_OK;
    }
    ++picker_generation_;
    if (picker_remote_) {
      picker_remote_->CancelKryptonBoostPicker();
      picker_remote_.reset();
    }
    preview_.reset();
    if (kind == KR_BOOST_COMMIT) {
      if (!Save(true, value->data.id, value->data.revision, value->Record())) {
        return KR_ERR_INVALID_ARGUMENT;
      }
    } else if (kind == KR_BOOST_REMOVE && value->data.id) {
      Mutate(true, value->data.id, true);
    }
    RefreshPreviewPage(id);
    return KR_OK;
  }
  void PickElement(bool focus) {
    if (!preview_) {
      return;
    }
    auto* tab = owner_->FindTab(preview_->data.tab);
    if (!tab) {
      return;
    }
    if (picker_remote_) {
      picker_remote_->CancelKryptonBoostPicker();
      picker_remote_.reset();
    }
    const auto generation = ++picker_generation_;
    auto* frame = tab->GetContents()->GetPrimaryMainFrame();
    const auto url = frame->GetLastCommittedURL();
    frame->GetRemoteAssociatedInterfaces()->GetInterface(&picker_remote_);
    tab->GetContents()->Focus();
    picker_remote_->PickKryptonBoostElement(base::BindOnce(
        [](base::WeakPtr<Impl> self, uint64_t generation, GURL url, bool focus,
           const std::string& selector) {
          if (!self || generation != self->picker_generation_ ||
              !self->preview_) {
            return;
          }
          auto* tab = self->owner_->FindTab(self->preview_->data.tab);
          if (!tab) {
            self->preview_.reset();
            return;
          }
          if (tab->GetContents()->GetLastCommittedURL() == url &&
              !selector.empty() && selector.size() <= 4096 &&
              selector.find_first_of("{}") == std::string::npos) {
            if (focus) {
              self->preview_->focused = selector;
            } else if (self->preview_->zapped.size() + selector.size() + 2 <=
                       16384) {
              if (!self->preview_->zapped.empty()) {
                self->preview_->zapped += ",\n";
              }
              self->preview_->zapped += selector;
            }
          }
          const auto* api = self->owner_->session_.api();
          const auto value = self->preview_->View();
          if (api && KR_HAS_SLOT(api, KrShellUI, show_boost_editor) &&
              api->show_boost_editor) {
            api->show_boost_editor(api->self, &value);
          }
        },
        weak_.GetWeakPtr(), generation, url, focus));
  }

  class Menu final : public ui::SimpleMenuModel,
                     public ui::SimpleMenuModel::Delegate {
   public:
    Menu(base::WeakPtr<Impl> owner,
         bool boost,
         uint64_t item,
         bool site_only = false)
        : ui::SimpleMenuModel(this),
          owner_(owner),
          boost_(boost),
          revision_(owner->revision_),
          site_only_(site_only) {
      next_ = boost ? 0x6d610000 : 0x6d600000;
      if (auto* tab = owner->owner_->browser_->GetActiveTabInterface()) {
        initial_ = tab->GetContents()->GetLastCommittedURL();
        tab_ = tab->GetWeakPtr();
      }
      if (!site_only) {
        AddItem(Add(Action::kCreate, 0), boost
                                             ? u"New boost for this site\u2026"
                                             : u"New routing rule\u2026");
      }
      const auto& rules = boost ? owner->boosts_ : owner->routes_;
      for (const auto& rule : rules) {
        if (site_only &&
            (rule.css.empty() || !Matches(rule, initial_, false))) {
          continue;
        }
        if (item && rule.id != item) {
          continue;
        }
        auto menu = std::make_unique<ui::SimpleMenuModel>(this);
        menu->AddCheckItem(Add(Action::kToggle, rule.id), u"Enabled");
        menu->AddItem(Add(Action::kEdit, rule.id), u"Edit\u2026");
        if (site_only) {
          menu->SetIcon(
              menu->GetItemCount() - 1,
              ui::ImageModel::FromVectorIcon(vector_icons::kEditIcon));
        }
        if (!site_only) {
          menu->AddItem(Add(Action::kRemove, rule.id), u"Remove");
        }
        const int row = next_++;
        rows_.emplace(row, rule.id);
        AddSubMenu(row,
                   MenuLabel(rule.title.empty() ? rule.pattern : rule.title),
                   menu.get());
        submenus_.push_back(std::move(menu));
      }
    }
    bool IsCommandIdEnabled(int command) const override {
      if (!owner_ || owner_->Private() || revision_ != owner_->revision_) {
        return false;
      }
      if (site_only_ &&
          (!tab_ ||
           owner_->owner_->browser_->GetActiveTabInterface() != tab_.get() ||
           tab_->GetContents()->GetLastCommittedURL() != initial_)) {
        return false;
      }
      const auto found = actions_.find(command);
      return found == actions_.end() || !found->second.second ||
             owner_->Find(boost_, found->second.second);
    }
    bool IsCommandIdChecked(int command) const override {
      const auto found = actions_.find(command);
      if (!IsCommandIdEnabled(command) || found == actions_.end()) {
        return false;
      }
      const auto* rule = owner_->Find(boost_, found->second.second);
      return found->second.first == Action::kToggle && rule && rule->enabled;
    }
    std::u16string GetSecondaryLabelAt(size_t index) const override {
      const auto found = rows_.find(GetCommandIdAt(index));
      const auto* rule = owner_ && found != rows_.end()
                             ? owner_->Find(boost_, found->second)
                             : nullptr;
      return rule ? (rule->enabled ? u"Enabled" : u"Disabled")
                  : std::u16string();
    }
    void ExecuteCommand(int command, int) override {
      const auto found = actions_.find(command);
      if (!IsCommandIdEnabled(command) || found == actions_.end()) {
        return;
      }
      const auto [action, id] = found->second;
      if (boost_ && (action == Action::kCreate || action == Action::kEdit)) {
        owner_->DismissLibrary();
      }
      std::ignore = owner_->owner_->session_.Defer(base::BindOnce(
          [](base::WeakPtr<Impl> self, bool boost, Action action, uint64_t id,
             uint64_t revision, GURL initial,
             base::WeakPtr<tabs::TabInterface> tab, bool site_only) {
            if (!self || revision != self->revision_) {
              return;
            }
            if (site_only &&
                (!tab ||
                 self->owner_->browser_->GetActiveTabInterface() != tab.get() ||
                 tab->GetContents()->GetLastCommittedURL() != initial)) {
              return;
            }
            if (action == Action::kCreate || action == Action::kEdit) {
              self->Edit(boost, id, initial);
            } else {
              self->Mutate(boost, id, action == Action::kRemove);
            }
          },
          owner_, boost_, action, id, revision_, initial_, tab_, site_only_));
    }

   private:
    enum class Action { kCreate, kEdit, kToggle, kRemove };
    int Add(Action action, uint64_t id) {
      const int command = next_++;
      actions_.emplace(command, std::pair(action, id));
      return command;
    }
    base::WeakPtr<Impl> owner_;
    bool boost_;
    uint64_t revision_;
    bool site_only_ = false;
    base::WeakPtr<tabs::TabInterface> tab_;
    GURL initial_;
    int next_ = 0x6d600000;
    base::flat_map<int, std::pair<Action, uint64_t>> actions_;
    base::flat_map<int, uint64_t> rows_;
    std::vector<std::unique_ptr<ui::SimpleMenuModel>> submenus_;
  };

  std::string Glyph(tabs::TabInterface& tab) {
    const auto id = EssentialForTab(tab.GetContents());
    if (!id || Private()) {
      return TabIconForTab(tab.GetContents());
    }
    const auto* icons =
        Prefs()->GetDict(kSiteCustomizationPref).FindDict("icons");
    const auto* value =
        icons ? icons->FindString(base::NumberToString(id)) : nullptr;
    return value ? *value : TabIconForTab(tab.GetContents());
  }
  void EditTabIcon(tabs::TabInterface& tab) {
    const auto* api = owner_->session_.api();
    if (!api || !KR_HAS_SLOT(api, KrShellUI, show_tab_icon_editor) ||
        !api->show_tab_icon_editor) {
      return;
    }
    const auto glyph = Glyph(tab);
    api->show_tab_icon_editor(api->self, BrowserAdapter::Identity(tab),
                              Borrow(glyph));
  }
  void SetTabIcon(KrTabId id, std::string_view glyph) {
    auto* tab = owner_->FindTab(id);
    if (!tab || glyph.size() > 128 || glyph.contains('\0') ||
        !base::IsStringUTF8(glyph)) {
      return;
    }
    const auto essential = EssentialForTab(tab->GetContents());
    if (essential && !Private()) {
      SetTabIconForTab(tab->GetContents(), {});
      if (auto* service = SessionServiceFactory::GetForProfile(
              owner_->browser_->GetProfile())) {
        service->AddTabExtraData(
            owner_->browser_->GetSessionID(),
            sessions::SessionTabHelper::IdForTab(tab->GetContents()),
            kTabIconSessionKey, "");
      }
      auto data = Prefs()->GetDict(kSiteCustomizationPref).Clone();
      auto* icons = data.FindDict("icons");
      if (!icons) {
        icons = data.Set("icons", base::DictValue())->GetIfDict();
      }
      if (glyph.empty()) {
        icons->Remove(base::NumberToString(essential));
      } else {
        icons->Set(base::NumberToString(essential), std::string(glyph));
      }
      Prefs()->SetDict(kSiteCustomizationPref, std::move(data));
    } else {
      SetTabIconForTab(tab->GetContents(), glyph);
      if (!Private()) {
        if (auto* service = SessionServiceFactory::GetForProfile(
                owner_->browser_->GetProfile())) {
          service->AddTabExtraData(
              owner_->browser_->GetSessionID(),
              sessions::SessionTabHelper::IdForTab(tab->GetContents()),
              kTabIconSessionKey, std::string(glyph));
        }
      }
      owner_->PublishTab(*tab);
    }
  }
  ui::ImageModel Icon(tabs::TabInterface& tab, const ui::ImageModel& fallback) {
    const auto glyph = Glyph(tab);
    if (glyph.empty() || glyph.size() > 128 || !base::IsStringUTF8(glyph)) {
      return fallback;
    }
    const auto color =
        owner_->view_->GetColorProvider()->GetColor(ui::kColorIcon);
    const auto key = std::pair(glyph, color);
    if (const auto found = icons_.find(key); found != icons_.end()) {
      return found->second;
    }
    const auto text = base::UTF8ToUTF16(glyph);
    const bool icon =
        text.size() == 1 && text.front() >= 0xE000 && text.front() <= 0xF8FF;
    const gfx::FontList font(
        std::vector<std::string>{icon ? "Segoe Fluent Icons"
                                      : "Segoe UI Emoji"},
        gfx::Font::NORMAL, 24, gfx::Font::Weight::NORMAL);
    gfx::Canvas canvas(gfx::Size(32, 32), 1.f, false);
    canvas.DrawStringRectWithFlags(text, font, color, gfx::Rect(32, 32),
                                   gfx::Canvas::TEXT_ALIGN_CENTER);
    auto image = ui::ImageModel::FromImageSkia(
        gfx::ImageSkia::CreateFrom1xBitmap(canvas.GetBitmap()));
    icons_.emplace(key, image);
    return image;
  }
  base::WeakPtr<Impl> GetWeakPtr() { return weak_.GetWeakPtr(); }

 private:
  raw_ref<BrowserAdapter> owner_;
  PrefChangeRegistrar prefs_;
  std::vector<Rule> routes_, boosts_;
  std::vector<KrLibraryItemId> published_;
  base::flat_map<std::pair<std::string, SkColor>, ui::ImageModel> icons_;
  base::WeakPtr<views::Widget> editor_;
  std::optional<BoostValue> preview_;
  bool preview_scheduled_ = false;
  uint64_t picker_generation_ = 0;
  mojo::AssociatedRemote<chrome::mojom::ChromeRenderFrame> picker_remote_;
  uint64_t revision_ = 0;
  base::WeakPtrFactory<Impl> weak_{this};
};

SiteCustomization::SiteCustomization(BrowserAdapter& owner)
    : impl_(std::make_unique<Impl>(owner)) {}
SiteCustomization::~SiteCustomization() = default;
std::unique_ptr<ui::MenuModel> SiteCustomization::SiteBoostsMenu() {
  if (impl_->Private()) {
    return nullptr;
  }
  return std::make_unique<Impl::Menu>(impl_->GetWeakPtr(), true, 0, true);
}
SiteCustomization::BoostState SiteCustomization::BoostForSite(
    const GURL& url) const {
  return impl_->BoostForSite(url);
}
void SiteCustomization::OpenBoost() {
  impl_->OpenBoost();
}
void SiteCustomization::Initialize() {
  impl_->Changed();
}
std::string SiteCustomization::Glyph(tabs::TabInterface& tab) {
  return impl_->Glyph(tab);
}

std::unique_ptr<ui::MenuModel> SiteCustomization::RoutingMenu(
    KrLibraryItemId item) {
  return std::make_unique<Impl::Menu>(impl_->GetWeakPtr(), false,
                                      item & kRuleIdMask);
}
std::unique_ptr<ui::MenuModel> SiteCustomization::BoostMenu(
    KrLibraryItemId item) {
  return std::make_unique<Impl::Menu>(impl_->GetWeakPtr(), true,
                                      item & kRuleIdMask);
}
void SiteCustomization::NavigationReady(content::NavigationHandle& nav) {
  impl_->ApplyStyles(*nav.GetRenderFrameHost(), nav.GetURL());
}
void SiteCustomization::NavigationFinished(tabs::TabInterface& tab,
                                           content::NavigationHandle& nav) {
  impl_->NavigationFinished(tab, nav);
}
void SiteCustomization::ApplyStyles(content::RenderFrameHost& frame) {
  impl_->ApplyStyles(frame, frame.GetLastCommittedURL());
}
void SiteCustomization::PublishBoosts() {
  impl_->PublishBoosts();
}
void SiteCustomization::LibraryCommand(uint32_t section,
                                       KrLibraryItemId item,
                                       uint32_t command) {
  impl_->LibraryCommand(section, item, command);
}
void SiteCustomization::EditTabIcon(tabs::TabInterface& tab) {
  impl_->EditTabIcon(tab);
}
KrResult SiteCustomization::EditBoost(uint32_t kind, const KrBoost* value) {
  return impl_->EditBoost(kind, value);
}
KrResult SiteCustomization::SaveRoutingRule(const KrRoutingRule* value) {
  return impl_->SaveRoutingRule(value);
}
void SiteCustomization::SetTabIcon(KrTabId tab, std::string_view glyph) {
  impl_->SetTabIcon(tab, glyph);
}
ui::ImageModel SiteCustomization::Icon(tabs::TabInterface& tab,
                                       const ui::ImageModel& fallback) {
  return impl_->Icon(tab, fallback);
}
}  // namespace krypton_ui
