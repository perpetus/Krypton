// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/runtime_contract.h"

namespace krypton_ui {

std::string_view ValidateExports(const RuntimeExports& exports) {
  if (!exports.is_supported || !exports.create || !exports.platform_name ||
      !exports.shutdown) {
    return "KryptonUI is missing a required runtime export.";
  }
  if (!exports.is_supported(KR_UI_ABI_VERSION)) {
    return "KryptonUI is incompatible or its Windows App Runtime prerequisite "
           "is unavailable. Repair the installation.";
  }
  return {};
}

std::string_view ValidateShell(const KrShellUI& shell) {
  if (!KR_HAS_SLOT(&shell, KrShellUI, self) || !shell.self) {
    return "KryptonUI returned an invalid shell instance.";
  }
#define KR_REQUIRE_SHELL_SLOT(slot)                                    \
  if (!KR_HAS_SLOT(&shell, KrShellUI, slot) || !shell.slot) {          \
    return "KryptonUI is missing the required shell slot: " #slot "."; \
  }
  KR_REQUIRE_SHELL_SLOT(destroy)
  KR_REQUIRE_SHELL_SLOT(set_window_state)
  KR_REQUIRE_SHELL_SLOT(set_theme)
  KR_REQUIRE_SHELL_SLOT(set_layout_prefs)
  KR_REQUIRE_SHELL_SLOT(put_image)
  KR_REQUIRE_SHELL_SLOT(drop_image)
  KR_REQUIRE_SHELL_SLOT(set_spaces)
  KR_REQUIRE_SHELL_SLOT(tab_inserted)
  KR_REQUIRE_SHELL_SLOT(tab_updated)
  KR_REQUIRE_SHELL_SLOT(tab_moved)
  KR_REQUIRE_SHELL_SLOT(tab_removed)
  KR_REQUIRE_SHELL_SLOT(set_active_tab)
  KR_REQUIRE_SHELL_SLOT(set_nav_state)
  KR_REQUIRE_SHELL_SLOT(set_actions)
  KR_REQUIRE_SHELL_SLOT(show_menu)
  KR_REQUIRE_SHELL_SLOT(focus_address)
  KR_REQUIRE_SHELL_SLOT(set_ui_text)
  KR_REQUIRE_SHELL_SLOT(set_text_direction)
  KR_REQUIRE_SHELL_SLOT(request_context_menu)
  KR_REQUIRE_SHELL_SLOT(set_tab_preview)
  KR_REQUIRE_SHELL_SLOT(set_workspace_theme)
  KR_REQUIRE_SHELL_SLOT(show_theme_editor)
  KR_REQUIRE_SHELL_SLOT(show_menu_panel)
  KR_REQUIRE_SHELL_SLOT(set_load_progress)
  KR_REQUIRE_SHELL_SLOT(set_appearance_prefs)
  KR_REQUIRE_SHELL_SLOT(show_settings)
  KR_REQUIRE_SHELL_SLOT(set_address_text)
  KR_REQUIRE_SHELL_SLOT(dismiss_address)
  KR_REQUIRE_SHELL_SLOT(set_suggestion_snapshot)
  KR_REQUIRE_SHELL_SLOT(dismiss_menu)
  KR_REQUIRE_SHELL_SLOT(show_workspace_editor)
  KR_REQUIRE_SHELL_SLOT(show_menu_panel_with_tiles)
  KR_REQUIRE_SHELL_SLOT(show_task_manager)
  KR_REQUIRE_SHELL_SLOT(upsert_task)
  KR_REQUIRE_SHELL_SLOT(remove_task)
  KR_REQUIRE_SHELL_SLOT(show_site_permissions)
  KR_REQUIRE_SHELL_SLOT(upsert_site_permission)
  KR_REQUIRE_SHELL_SLOT(remove_site_permission)
  KR_REQUIRE_SHELL_SLOT(show_permission_prompt)
  KR_REQUIRE_SHELL_SLOT(dismiss_permission_prompt)
  KR_REQUIRE_SHELL_SLOT(set_address_edit)
  KR_REQUIRE_SHELL_SLOT(focus_native)
#undef KR_REQUIRE_SHELL_SLOT
  return {};
}

}  // namespace krypton_ui
