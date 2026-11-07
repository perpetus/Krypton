// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/runtime_contract.h"

#include <cstddef>
#include <functional>
#include <type_traits>

#include "chrome/browser/ui/krypton_ui/buildflags.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(ENABLE_KRYPTON_UI)
#include <windows.h>

#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "chrome/browser/ui/krypton_ui/module.h"
#include "chrome/browser/ui/krypton_ui/test_task_runner.h"
#include "chrome/browser/ui/krypton_ui/window_session.h"
#endif

namespace krypton_ui {
namespace {

template <typename Function>
struct Stub;

template <typename Result, typename... Args>
struct Stub<Result (*)(Args...)> {
  static Result Call(Args...) {
    if constexpr (!std::is_void_v<Result>) {
      return {};
    }
  }
};

KrShellUI CompleteShell() {
  KrShellUI shell{};
  shell.struct_size = sizeof(shell);
  static int instance;
  shell.self = &instance;
#define KR_STUB_SLOT(slot) shell.slot = &Stub<decltype(shell.slot)>::Call
  KR_STUB_SLOT(destroy);
  KR_STUB_SLOT(set_window_state);
  KR_STUB_SLOT(set_theme);
  KR_STUB_SLOT(set_layout_prefs);
  KR_STUB_SLOT(put_image);
  KR_STUB_SLOT(drop_image);
  KR_STUB_SLOT(set_spaces);
  KR_STUB_SLOT(tab_inserted);
  KR_STUB_SLOT(tab_updated);
  KR_STUB_SLOT(tab_moved);
  KR_STUB_SLOT(tab_removed);
  KR_STUB_SLOT(set_active_tab);
  KR_STUB_SLOT(set_nav_state);
  KR_STUB_SLOT(set_actions);
  KR_STUB_SLOT(show_menu);
  KR_STUB_SLOT(focus_address);
  KR_STUB_SLOT(set_ui_text);
  KR_STUB_SLOT(set_text_direction);
  KR_STUB_SLOT(request_context_menu);
  KR_STUB_SLOT(set_tab_preview);
  KR_STUB_SLOT(set_workspace_theme);
  KR_STUB_SLOT(show_theme_editor);
  KR_STUB_SLOT(show_menu_panel);
  KR_STUB_SLOT(set_load_progress);
  KR_STUB_SLOT(set_appearance_prefs);
  KR_STUB_SLOT(show_settings);
  KR_STUB_SLOT(set_address_text);
  KR_STUB_SLOT(dismiss_address);
  KR_STUB_SLOT(set_suggestion_snapshot);
  KR_STUB_SLOT(dismiss_menu);
  KR_STUB_SLOT(show_workspace_editor);
  KR_STUB_SLOT(show_menu_panel_with_tiles);
  KR_STUB_SLOT(show_task_manager);
  KR_STUB_SLOT(upsert_task);
  KR_STUB_SLOT(remove_task);
  KR_STUB_SLOT(show_site_permissions);
  KR_STUB_SLOT(upsert_site_permission);
  KR_STUB_SLOT(remove_site_permission);
  KR_STUB_SLOT(show_permission_prompt);
  KR_STUB_SLOT(dismiss_permission_prompt);
  KR_STUB_SLOT(set_address_edit);
  KR_STUB_SLOT(focus_native);
  KR_STUB_SLOT(upsert_privacy_feature);
  KR_STUB_SLOT(remove_privacy_feature);
  KR_STUB_SLOT(upsert_download);
  KR_STUB_SLOT(remove_download);
  KR_STUB_SLOT(upsert_media_session);
  KR_STUB_SLOT(remove_media_session);
  KR_STUB_SLOT(set_library_section);
  KR_STUB_SLOT(upsert_library_item);
  KR_STUB_SLOT(remove_library_item);
  KR_STUB_SLOT(show_library);
#undef KR_STUB_SLOT
  return shell;
}

TEST(KryptonUiRuntimeContractTest, RejectsMissingExports) {
  RuntimeExports exports;
  EXPECT_FALSE(ValidateExports(exports).empty());
  exports.is_supported = &Stub<KrUiIsSupportedFn>::Call;
  exports.create = &Stub<KrUiCreateFn>::Call;
  exports.platform_name = &Stub<KrUiPlatformNameFn>::Call;
  EXPECT_FALSE(ValidateExports(exports).empty());
  exports.shutdown = &Stub<KrUiShutdownFn>::Call;
  exports.is_supported = [](uint32_t version) {
    return version == KR_UI_ABI_VERSION;
  };
  EXPECT_TRUE(ValidateExports(exports).empty());
}

TEST(KryptonUiRuntimeContractTest, RejectsUnsupportedAbiOrRuntime) {
  const RuntimeExports exports{
      [](uint32_t version) {
        EXPECT_EQ(version, KR_UI_ABI_VERSION);
        return false;
      },
      &Stub<KrUiCreateFn>::Call,
      &Stub<KrUiPlatformNameFn>::Call,
      &Stub<KrUiShutdownFn>::Call,
  };
  EXPECT_FALSE(ValidateExports(exports).empty());
}

TEST(KryptonUiRuntimeContractTest, RejectsInvalidInstanceAndMissingDestroy) {
  auto shell = CompleteShell();
  shell.self = nullptr;
  EXPECT_FALSE(ValidateShell(shell).empty());
  shell = CompleteShell();
  shell.destroy = nullptr;
  EXPECT_FALSE(ValidateShell(shell).empty());
}

TEST(KryptonUiRuntimeContractTest, RequiresSnapshotAndWorkspaceExtensions) {
  auto shell = CompleteShell();
  EXPECT_TRUE(ValidateShell(shell).empty());
  shell.struct_size = offsetof(KrShellUI, set_suggestion_snapshot);
  EXPECT_FALSE(ValidateShell(shell).empty());
  shell = CompleteShell();
  shell.show_workspace_editor = nullptr;
  EXPECT_FALSE(ValidateShell(shell).empty());
}

TEST(KryptonUiRuntimeContractTest, AcceptsFutureTrailingSlots) {
  auto shell = CompleteShell();
  shell.struct_size += sizeof(void*);
  EXPECT_TRUE(ValidateShell(shell).empty());
}

TEST(KryptonUiRuntimeContractTest, RequiresSourceMenuPresentation) {
  auto shell = CompleteShell();
  shell.show_menu = nullptr;
  EXPECT_NE(ValidateShell(shell).find("show_menu"), std::string_view::npos);
}

TEST(KryptonUiRuntimeContractTest, RequiresNativeBrowserToolExtensions) {
  auto shell = CompleteShell();
  shell.struct_size = offsetof(KrShellUI, show_menu_panel_with_tiles);
  EXPECT_FALSE(ValidateShell(shell).empty());

  shell = CompleteShell();
  shell.show_task_manager = nullptr;
  EXPECT_FALSE(ValidateShell(shell).empty());

  shell = CompleteShell();
  shell.upsert_site_permission = nullptr;
  EXPECT_FALSE(ValidateShell(shell).empty());

  shell = CompleteShell();
  shell.dismiss_permission_prompt = nullptr;
  EXPECT_FALSE(ValidateShell(shell).empty());
}

TEST(KryptonUiRuntimeContractTest, RequiresModelDrivenNativeAddressEditing) {
  auto shell = CompleteShell();
  shell.struct_size = offsetof(KrShellUI, set_address_edit);
  EXPECT_FALSE(ValidateShell(shell).empty());
  shell = CompleteShell();
  shell.set_address_edit = nullptr;
  EXPECT_FALSE(ValidateShell(shell).empty());
}

TEST(KryptonUiRuntimeContractTest, RequiresSemanticNativeFocus) {
  auto shell = CompleteShell();
  shell.struct_size = offsetof(KrShellUI, focus_native);
  EXPECT_NE(ValidateShell(shell).find("focus_native"), std::string_view::npos);
  shell = CompleteShell();
  shell.focus_native = nullptr;
  EXPECT_NE(ValidateShell(shell).find("focus_native"), std::string_view::npos);
}

}  // namespace

#if BUILDFLAG(ENABLE_KRYPTON_UI)
// A fake ABI module and hidden Win32 controls exercise ownership without
// loading XAML, creating rendering surfaces, or using the GPU.
class ModuleTest : public ::testing::Test {
 protected:
  struct Window {
    Window() {
      handle = ::CreateWindowExW(0, L"STATIC", L"KryptonUI contract test",
                                 WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                 ::GetModuleHandleW(nullptr), nullptr);
    }
    ~Window() {
      if (handle) {
        ::DestroyWindow(handle);
      }
    }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    HWND handle = nullptr;
  };

  void SetUp() override {
    ASSERT_TRUE(first_.handle);
    ASSERT_TRUE(second_.handle);
    current_ = this;
    module_.attempted_ = true;
    module_.module_ = ::GetModuleHandleW(nullptr);
    module_.exports_.create = &Create;
    module_.exports_.shutdown = &Shutdown;
  }
  void TearDown() override {
    EXPECT_EQ(created_, destroyed_);
    current_ = nullptr;
  }

  void FailInitialization() {
    module_.initialization_error_ = "Test initialization failure";
  }
  static KrResult KR_UI_CALL Create(const KrCreateParams*, KrShellUI* output) {
    if (current_->create_error_) {
      return KR_ERR_PLATFORM;
    }
    *output = CompleteShell();
    output->self = current_;
    output->destroy = [](void* self) {
      ++static_cast<ModuleTest*>(self)->destroyed_;
    };
    output->set_window_state = [](void* self, const KrWindowState* state) {
      auto& fixture = *static_cast<ModuleTest*>(self);
      ++fixture.state_calls_;
      if (fixture.on_state_) {
        fixture.on_state_.Run(*state);
      }
    };
    if (current_->incomplete_shell_) {
      output->set_suggestion_snapshot = nullptr;
    }
    ++current_->created_;
    return KR_OK;
  }
  static KrResult KR_UI_CALL Shutdown() {
    ++current_->shutdown_calls_;
    return KR_OK;
  }

  static inline ModuleTest* current_ = nullptr;
  Module module_;
  Window first_, second_;
  const KrBrowser browser_{sizeof(KrBrowser)};
  const KrTheme theme_{sizeof(KrTheme)};
  int created_ = 0;
  int destroyed_ = 0;
  int shutdown_calls_ = 0;
  bool create_error_ = false;
  bool incomplete_shell_ = false;
  unsigned state_calls_ = 0;
  base::RepeatingCallback<void(const KrWindowState&)> on_state_;
};

TEST_F(ModuleTest, PropagatesInitializationAndCreationErrors) {
  create_error_ = true;
  EXPECT_FALSE(module_.Create(first_.handle, browser_, theme_).has_value());
  EXPECT_EQ(created_, 0);
  FailInitialization();
  EXPECT_FALSE(module_.Initialize().has_value());
  EXPECT_FALSE(module_.Create(first_.handle, browser_, theme_).has_value());
}

TEST_F(ModuleTest, RejectsInvalidWindowsBeforeCallingModule) {
  EXPECT_FALSE(module_.Create(nullptr, browser_, theme_).has_value());
  EXPECT_EQ(created_, 0);
}

TEST_F(ModuleTest, CleansUpAnIncompleteReturnedContract) {
  incomplete_shell_ = true;
  EXPECT_FALSE(module_.Create(first_.handle, browser_, theme_).has_value());
  EXPECT_EQ(created_, 1);
  EXPECT_EQ(destroyed_, 1);
  EXPECT_TRUE(module_.Shutdown().has_value());
}

TEST_F(ModuleTest, RefusesShutdownUntilEveryWindowIsDestroyed) {
  auto first = module_.Create(first_.handle, browser_, theme_);
  auto second = module_.Create(second_.handle, browser_, theme_);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_FALSE(module_.Shutdown().has_value());
  module_.Destroy(*first);
  EXPECT_FALSE(module_.Shutdown().has_value());
  module_.Destroy(*second);
  EXPECT_TRUE(module_.Shutdown().has_value());
  EXPECT_EQ(shutdown_calls_, 1);
}

TEST_F(ModuleTest, LastWindowCloseAllowsReopeningUntilTerminalShutdown) {
  auto first = module_.Create(first_.handle, browser_, theme_);
  ASSERT_TRUE(first.has_value());
  module_.Destroy(*first);
  module_.Destroy(*first);
  EXPECT_EQ(destroyed_, 1);
  EXPECT_EQ(shutdown_calls_, 0);
  auto reopened = module_.Create(second_.handle, browser_, theme_);
  ASSERT_TRUE(reopened.has_value());
  module_.Destroy(*reopened);
  EXPECT_TRUE(module_.Shutdown().has_value());
  EXPECT_TRUE(module_.Shutdown().has_value());
  EXPECT_EQ(shutdown_calls_, 1);
  EXPECT_FALSE(module_.Create(first_.handle, browser_, theme_).has_value());
}

TEST_F(ModuleTest, WindowSessionCancelsCommandsBeforeDestroyingShell) {
  auto runner = base::MakeRefCounted<test::TaskRunner>();
  WindowSession session(module_, runner);
  ASSERT_TRUE(session.Initialize(first_.handle, browser_, theme_).has_value());
  int calls = 0;
  auto weak = session.GetWeakPtr();
  EXPECT_TRUE(weak);
  EXPECT_TRUE(session.Defer(
      base::BindOnce([](int& calls) { ++calls; }, std::ref(calls))));
  session.Close();
  EXPECT_FALSE(weak);
  EXPECT_EQ(destroyed_, 1);
  EXPECT_TRUE(::IsWindow(first_.handle));
  EXPECT_EQ(session.api(), nullptr);
  EXPECT_FALSE(session.Defer(
      base::BindOnce([](int& calls) { ++calls; }, std::ref(calls))));
  EXPECT_TRUE(runner->RunOne());
  EXPECT_EQ(calls, 0);
  session.Close();
  EXPECT_EQ(destroyed_, 1);
  EXPECT_FALSE(session.Initialize(first_.handle, browser_, theme_).has_value());
}

TEST_F(ModuleTest, FailedSessionCannotBeRetriedAsAnotherShell) {
  auto runner = base::MakeRefCounted<test::TaskRunner>();
  WindowSession session(module_, runner);
  create_error_ = true;
  EXPECT_FALSE(session.Initialize(first_.handle, browser_, theme_).has_value());
  create_error_ = false;
  EXPECT_FALSE(session.Initialize(first_.handle, browser_, theme_).has_value());
  EXPECT_FALSE(session.UpdateWindowState(0).has_value());
  EXPECT_EQ(created_, 0);
}

TEST_F(ModuleTest, SessionReportsSynchronousGeometryAndSkipsUnchangedState) {
  auto runner = base::MakeRefCounted<test::TaskRunner>();
  WindowSession session(module_, runner);
  ASSERT_TRUE(session.Initialize(first_.handle, browser_, theme_).has_value());
  on_state_ = base::BindRepeating(
      [](WindowSession& session, const KrWindowState& state) {
        EXPECT_GT(state.scale_factor, 0);
        EXPECT_TRUE(session.geometry().SetContent({31.5f, 20, 100, 50}, 3));
      },
      std::ref(session));
  auto changed = session.UpdateWindowState(KR_WINDOW_ACTIVE);
  ASSERT_TRUE(changed.has_value());
  EXPECT_TRUE(*changed);
  EXPECT_FLOAT_EQ(session.geometry().content().x, 31.5f);
  EXPECT_FLOAT_EQ(session.geometry().corner_radius(), 3);
  changed = session.UpdateWindowState(KR_WINDOW_ACTIVE);
  ASSERT_TRUE(changed.has_value());
  EXPECT_FALSE(*changed);
  EXPECT_EQ(state_calls_, 1u);
}

TEST_F(ModuleTest, ReentrantResizeKeepsTheNewestWindowState) {
  auto runner = base::MakeRefCounted<test::TaskRunner>();
  WindowSession session(module_, runner);
  ASSERT_TRUE(session.Initialize(first_.handle, browser_, theme_).has_value());
  ASSERT_TRUE(::SetWindowPos(first_.handle, nullptr, 0, 0, 600, 400,
                             SWP_NOACTIVATE | SWP_NOZORDER));
  bool reentered = false;
  on_state_ = base::BindRepeating(
      [](WindowSession& session, HWND window, bool& reentered,
         const KrWindowState&) {
        if (reentered) {
          return;
        }
        reentered = true;
        EXPECT_TRUE(::SetWindowPos(window, nullptr, 0, 0, 700, 500,
                                   SWP_NOACTIVATE | SWP_NOZORDER));
        auto result = session.UpdateWindowState(0);
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
      },
      std::ref(session), first_.handle, std::ref(reentered));
  ASSERT_TRUE(session.UpdateWindowState(0).has_value());
  EXPECT_TRUE(reentered);
  EXPECT_EQ(state_calls_, 2u);
  const auto unchanged = session.UpdateWindowState(0);
  ASSERT_TRUE(unchanged.has_value());
  EXPECT_FALSE(*unchanged);
  EXPECT_EQ(state_calls_, 2u);
}

TEST_F(ModuleTest, SessionCanCloseFromItsSynchronousLayoutCallback) {
  auto runner = base::MakeRefCounted<test::TaskRunner>();
  WindowSession session(module_, runner);
  ASSERT_TRUE(session.Initialize(first_.handle, browser_, theme_).has_value());
  auto weak = session.GetWeakPtr();
  on_state_ = base::BindRepeating(
      [](WindowSession& session, const KrWindowState&) { session.Close(); },
      std::ref(session));
  EXPECT_TRUE(session.UpdateWindowState(0).has_value());
  EXPECT_FALSE(weak);
  EXPECT_EQ(session.api(), nullptr);
  EXPECT_EQ(destroyed_, 1);
  EXPECT_EQ(state_calls_, 1u);
}

TEST_F(ModuleTest, SessionDestructionCancelsOutstandingCommands) {
  auto runner = base::MakeRefCounted<test::TaskRunner>();
  int calls = 0;
  {
    WindowSession session(module_, runner);
    ASSERT_TRUE(
        session.Initialize(first_.handle, browser_, theme_).has_value());
    ASSERT_TRUE(session.Defer(
        base::BindOnce([](int& calls) { ++calls; }, std::ref(calls))));
  }
  EXPECT_EQ(destroyed_, 1);
  EXPECT_TRUE(runner->RunOne());
  EXPECT_EQ(calls, 0);
}
#endif  // BUILDFLAG(ENABLE_KRYPTON_UI)

}  // namespace krypton_ui
