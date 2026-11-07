// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/module.h"

#include <windows.h>

#include <utility>

#include "base/base_paths.h"
#include "base/check.h"
#include "base/files/file_path.h"
#include "base/path_service.h"
#include "base/strings/stringprintf.h"
#include "base/trace_event/trace_event.h"

namespace krypton_ui {

Module::Module() = default;

Module& Module::Get() {
  static base::NoDestructor<Module> module;
  return *module;
}

base::expected<void, std::string> Module::Initialize() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  TRACE_EVENT0("browser", "KryptonUI.Initialize");
  if (shutdown_) {
    return base::unexpected("KryptonUI has already shut down.");
  }
  if (!attempted_) {
    attempted_ = true;
    base::FilePath directory;
    // DIR_MODULE is the directory of chrome.dll in an installed versioned
    // binary directory, and of the host executable in a static developer build.
    if (!base::PathService::Get(base::DIR_MODULE, &directory)) {
      initialization_error_ = "Cannot locate Krypton's binary directory.";
    } else {
      const auto path = directory.Append(FILE_PATH_LITERAL("krypton_ui.dll"));
      module_ = ::LoadLibraryExW(
          path.value().c_str(), nullptr,
          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
      if (!module_) {
        initialization_error_ = base::StringPrintf(
            "Cannot load KryptonUI (Windows error %lu). Repair the "
            "installation "
            "and verify the Windows App Runtime prerequisite.",
            ::GetLastError());
      } else {
        exports_ = {
            reinterpret_cast<KrUiIsSupportedFn>(
                ::GetProcAddress(module_, "kr_ui_is_supported")),
            reinterpret_cast<KrUiCreateFn>(
                ::GetProcAddress(module_, "kr_ui_create")),
            reinterpret_cast<KrUiPlatformNameFn>(
                ::GetProcAddress(module_, "kr_ui_platform_name")),
            reinterpret_cast<KrUiShutdownFn>(
                ::GetProcAddress(module_, "kr_ui_shutdown")),
        };
        initialization_error_ = ValidateExports(exports_);
      }
    }
  }
  if (!initialization_error_.empty()) {
    return base::unexpected(initialization_error_);
  }
  return base::ok();
}

base::expected<KrShellUI, std::string> Module::Create(
    HWND window,
    const KrBrowser& browser,
    const KrTheme& initial_theme) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  TRACE_EVENT0("browser", "KryptonUI.Create");
  if (auto result = Initialize(); !result.has_value()) {
    return base::unexpected(std::move(result.error()));
  }
  if (!::IsWindow(window) ||
      ::GetWindowThreadProcessId(window, nullptr) != ::GetCurrentThreadId()) {
    return base::unexpected(
        "KryptonUI requires the window's owning UI thread.");
  }
  const KrCreateParams params{sizeof(KrCreateParams), KR_UI_ABI_VERSION, window,
                              &browser, &initial_theme};
  KrShellUI shell{};
  shell.struct_size = sizeof(shell);
  const auto result = exports_.create(&params, &shell);
  if (result != KR_OK) {
    return base::unexpected(
        base::StringPrintf("KryptonUI could not create this window (error %u).",
                           static_cast<unsigned>(result)));
  }
  if (const auto error = ValidateShell(shell); !error.empty()) {
    if (KR_HAS_SLOT(&shell, KrShellUI, destroy) && shell.destroy &&
        shell.self) {
      shell.destroy(shell.self);
    }
    return base::unexpected(std::string(error));
  }
  ++shell_count_;
  return shell;
}

void Module::Destroy(KrShellUI& shell) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  TRACE_EVENT0("browser", "KryptonUI.Destroy");
  if (!shell.self) {
    return;
  }
  CHECK_GT(shell_count_, 0u);
  // Clear the host's instance before calling out, including during reentrancy.
  const auto instance = std::exchange(shell, KrShellUI{});
  instance.destroy(instance.self);
  --shell_count_;
}

base::expected<void, std::string> Module::Shutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  TRACE_EVENT0("browser", "KryptonUI.Shutdown");
  if (shutdown_) {
    return base::ok();
  }
  if (shell_count_ != 0) {
    return base::unexpected("KryptonUI still has live browser windows.");
  }
  shutdown_ = true;
  if (module_ && initialization_error_.empty() &&
      exports_.shutdown() != KR_OK) {
    return base::unexpected("KryptonUI could not shut down its UI runtime.");
  }
  return base::ok();
}

}  // namespace krypton_ui
