// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_MODULE_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_MODULE_H_

#include <cstddef>
#include <string>

#include "base/no_destructor.h"
#include "base/sequence_checker.h"
#include "base/types/expected.h"
#include "base/win/windows_types.h"
#include "chrome/browser/ui/krypton_ui/runtime_contract.h"

namespace krypton_ui {

// Process-lifetime loader. XAML owns process-global objects whose code must
// remain mapped even after the last browser window closes.
class Module {
 public:
  static Module& Get();
  Module(const Module&) = delete;
  Module& operator=(const Module&) = delete;

  [[nodiscard]] base::expected<void, std::string> Initialize();
  [[nodiscard]] base::expected<KrShellUI, std::string>
  Create(HWND window, const KrBrowser& browser, const KrTheme& initial_theme);
  void Destroy(KrShellUI& shell);

  // Terminal; call once at UI-thread shutdown, never on an ordinary close.
  [[nodiscard]] base::expected<void, std::string> Shutdown();

 private:
  friend class base::NoDestructor<Module>;
  friend class ModuleTest;
  Module();
  ~Module() = default;

  // Intentionally not ScopedNativeLibrary: this module cannot be unloaded.
  HMODULE module_ = nullptr;
  RuntimeExports exports_;
  std::string initialization_error_;
  size_t shell_count_ = 0;
  bool attempted_ = false;
  bool shutdown_ = false;
  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_MODULE_H_
