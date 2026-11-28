#include "chrome/browser/krypton_agent/confirmation_gate.h"

#include <algorithm>
#include <array>
#include <utility>

#include "base/logging.h"
#include "base/strings/string_util.h"
#include "chrome/browser/krypton_agent/page_reader.h"
#include "chrome/common/actor_webui.mojom.h"

namespace krypton_agent {
namespace {
namespace proto = optimization_guide::proto;
namespace mojom = actor::webui::mojom;

bool HasConsequentialLabel(std::string_view label) {
  static constexpr std::array<std::string_view, 18> kVerbs{
      "buy",     "order",  "pay",       "send",    "post",   "delete",
      "publish", "accept", "confirm",   "submit",  "upload", "download",
      "sign in", "log in", "authorize", "approve", "grant",  "remove"};
  std::string lower = base::ToLowerASCII(label);
  return std::ranges::any_of(
      kVerbs, [&lower](auto verb) { return lower.contains(verb); });
}
}  // namespace

bool ConfirmationGate::IsWebNavigation(const GURL& url) {
  return url.is_valid() && url.SchemeIsHTTPOrHTTPS() && !url.has_username() &&
         !url.has_password();
}

bool ConfirmationGate::NeedsConfirmation(std::string_view tool,
                                         const base::DictValue& arguments,
                                         const PageOutline& page,
                                         const PageTarget* target) const {
  if (tool == "read_page") {
    return false;
  }
  if (tool == "capture_page") {
    return true;
  }
  if (flagged_ || page.sensitive_page || page.truncated) {
    return true;
  }
  if (tool == "type") {
    return target &&
           (target->sensitive_field || HasConsequentialLabel(target->label) ||
            arguments.FindBool("follow_by_enter").value_or(false) ||
            (target->control_type != proto::FORM_CONTROL_TYPE_INPUT_SEARCH &&
             target->control_type != proto::FORM_CONTROL_TYPE_INPUT_TEXT &&
             target->control_type != proto::FORM_CONTROL_TYPE_TEXT_AREA));
  }
  if (tool == "select_option") {
    return target &&
           (target->sensitive_field || HasConsequentialLabel(target->label));
  }
  if (tool == "drag_and_release" || arguments.FindInt("click_count") == 2) {
    return true;
  }
  if ((tool == "click" || tool == "set_checked") && target) {
    if (target->in_form || target->sensitive_field ||
        HasConsequentialLabel(target->label)) {
      return true;
    }
    switch (target->control_type) {
      case proto::FORM_CONTROL_TYPE_BUTTON_BUTTON:
      case proto::FORM_CONTROL_TYPE_INPUT_BUTTON:
      case proto::FORM_CONTROL_TYPE_INPUT_CHECKBOX:
      case proto::FORM_CONTROL_TYPE_INPUT_RADIO:
        return false;
      default:
        // Link target and download attributes aren't available in the outline.
        // Custom controls can also hide submission or window-open behavior.
        return true;
    }
  }
  return false;
}

void ConfirmationGate::LogDenial(std::string_view action) {
  LOG(WARNING) << "Krypton Agent denied an action requiring confirmation: "
               << action;
}

void ConfirmationGate::OnTabAddedToTask(actor::TaskId, const tabs::TabHandle&) {
}
void ConfirmationGate::OnTaskTabsVisibilityChanged(actor::TaskId, bool) {}

void ConfirmationGate::RequestToShowCredentialSelectionDialog(
    actor::TaskId,
    const base::flat_map<std::string, gfx::Image>&,
    const std::vector<actor_login::Credential>&,
    CredentialSelectedCallback callback) {
  LogDenial("credential selection");
  std::move(callback).Run(nullptr);
}

void ConfirmationGate::RequestToShowUserConfirmationDialog(
    actor::TaskId,
    const url::Origin&,
    bool,
    UserConfirmationDialogCallback callback) {
  LogDenial("framework confirmation");
  std::move(callback).Run(mojom::UserConfirmationDialogResponse::New(
      mojom::ConfirmationRequestResult::NewPermissionGranted(false)));
}

void ConfirmationGate::RequestToConfirmNavigation(
    actor::TaskId,
    const url::Origin&,
    NavigationConfirmationCallback callback) {
  LogDenial("navigation");
  std::move(callback).Run(mojom::NavigationConfirmationResponse::New(
      mojom::ConfirmationRequestResult::NewPermissionGranted(false)));
}

void ConfirmationGate::RequestToShowAutofillSuggestionsDialog(
    actor::TaskId,
    std::vector<autofill::ActorFormFillingRequest>,
    base::WeakPtr<actor::AutofillSelectionDialogEventHandler>,
    AutofillSuggestionSelectedCallback callback) {
  LogDenial("autofill");
  std::move(callback).Run(nullptr);
}

void ConfirmationGate::RequestToShowGmailOtpOptInDialog(
    actor::TaskId,
    GmailOtpOptInCallback callback) {
  LogDenial("one-time code opt-in");
  std::move(callback).Run(mojom::GmailOtpOptInResult::NewResponse(
      mojom::GmailOtpOptInResponse::New(false)));
}

void ConfirmationGate::RequestToShowGmailOtpConfirmationDialog(
    actor::TaskId,
    const std::string&,
    GmailOtpConfirmationCallback callback) {
  LogDenial("one-time code");
  std::move(callback).Run(mojom::GmailOtpConfirmationResult::NewResponse(
      mojom::GmailOtpConfirmationResponse::New(false)));
}

}  // namespace krypton_agent
