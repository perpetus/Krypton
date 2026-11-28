#include "chrome/browser/krypton_agent/page_reader.h"

#include <algorithm>
#include <array>
#include <utility>

#include "base/i18n/time_formatting.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/raw_ref.h"
#include "base/rand_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/krypton_agent/privacy_filter.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/mojom/content_extraction/ai_page_content.mojom.h"

namespace krypton_agent {
namespace {

namespace proto = optimization_guide::proto;

bool IsHidden(const proto::ContentAttributes& attributes) {
  if (attributes.attribute_type() == proto::CONTENT_ATTRIBUTE_ROOT) {
    return false;
  }
  const auto& bounds = attributes.geometry().visible_bounding_box();
  return bounds.width() <= 0 || bounds.height() <= 0 ||
         std::ranges::find(attributes.annotated_roles(),
                           proto::ANNOTATED_ROLE_CONTENT_HIDDEN) !=
             attributes.annotated_roles().end() ||
         std::ranges::find(
             attributes.interaction_info().interaction_disabled_reasons(),
             proto::INTERACTION_DISABLED_REASON_ARIA_HIDDEN) !=
             attributes.interaction_info().interaction_disabled_reasons().end();
}

bool IsSensitive(const proto::ContentAttributes& attributes) {
  return attributes.redaction_decision() !=
             proto::REDACTION_DECISION_NO_REDACTION_NECESSARY ||
         attributes.form_control_data().form_control_type() ==
             proto::FORM_CONTROL_TYPE_INPUT_PASSWORD;
}

HandoffReason ClassifyHandoff(const proto::ContentAttributes& attributes) {
  const auto& control = attributes.form_control_data();
  switch (attributes.redaction_decision()) {
    case proto::REDACTION_DECISION_REDACTED_IS_OTP:
    case proto::REDACTION_DECISION_UNREDACTED_EMPTY_OTP_FIELD:
      return HandoffReason::kVerify;
    case proto::REDACTION_DECISION_UNREDACTED_EMPTY_PAYMENT_FIELD:
    case proto::REDACTION_DECISION_REDACTED_IS_SENSITIVE_PAYMENT_FIELD:
      return HandoffReason::kEnterInformation;
    default:
      if (IsSensitive(attributes)) {
        return HandoffReason::kSignIn;
      }
  }
  for (const auto field : control.coarse_autofill_field_type()) {
    if (field == proto::COARSE_AUTOFILL_FIELD_TYPE_OTP) {
      return HandoffReason::kVerify;
    }
    if (field == proto::COARSE_AUTOFILL_FIELD_TYPE_ADDRESS ||
        field == proto::COARSE_AUTOFILL_FIELD_TYPE_CREDIT_CARD) {
      return HandoffReason::kEnterInformation;
    }
  }
  const std::string label =
      base::ToLowerASCII(attributes.label().substr(0, 256));
  const std::string text =
      base::ToLowerASCII(attributes.text_data().text_content().substr(0, 256));
  for (const auto& value : {std::string_view(label), std::string_view(text)}) {
    if (value.find("captcha") != std::string_view::npos ||
        value.find("verify you are human") != std::string_view::npos ||
        value.find("verify that you are human") != std::string_view::npos) {
      return HandoffReason::kVerify;
    }
    if (value.find("sign in to continue") != std::string_view::npos ||
        value.find("log in to continue") != std::string_view::npos) {
      return HandoffReason::kSignIn;
    }
    if (attributes.has_form_control_data() &&
        (value.find("identity verification") != std::string_view::npos ||
         value.find("government id") != std::string_view::npos ||
         value.find("passport number") != std::string_view::npos ||
         value.find("social security number") != std::string_view::npos)) {
      return HandoffReason::kEnterInformation;
    }
  }
  return HandoffReason::kNone;
}

bool IsResearchBoilerplate(const proto::ContentAttributes& attributes) {
  switch (attributes.aria_role()) {
    case proto::AX_ROLE_BANNER:
    case proto::AX_ROLE_COMPLEMENTARY:
    case proto::AX_ROLE_CONTENTINFO:
    case proto::AX_ROLE_NAVIGATION:
      return true;
    default:
      return attributes.is_ad_related();
  }
}

void CollectLinkTitle(const proto::ContentNode& node,
                      std::string& text,
                      bool& heading,
                      size_t depth = 0) {
  const auto& attributes = node.content_attributes();
  if (depth > 12 || text.size() >= 512 || IsHidden(attributes) ||
      IsSensitive(attributes) || IsResearchBoilerplate(attributes)) {
    return;
  }
  heading |= attributes.attribute_type() == proto::CONTENT_ATTRIBUTE_HEADING;
  const auto& part = attributes.text_data().text_content();
  if (!part.empty()) {
    if (!text.empty()) {
      text.push_back(' ');
    }
    text.append(part.substr(0, 512 - text.size()));
  }
  for (const auto& child : node.children_nodes()) {
    CollectLinkTitle(child, text, heading, depth + 1);
  }
}

class OutlineBuilder {
 public:
  OutlineBuilder(PageOutline& outline,
                 PrivacyFilter& filter,
                 const url::Origin& origin,
                 PageReadMode mode)
      : outline_(outline), filter_(filter), origin_(origin), mode_(mode) {}

  void Visit(const proto::ContentNode& node,
             bool in_form,
             bool user_content,
             size_t depth,
             bool in_heading = false,
             bool in_code = false) {
    if (depth > 80 || ++visited_ > 20000) {
      outline_->truncated = true;
      return;
    }
    const auto& attributes = node.content_attributes();
    const auto kind = attributes.attribute_type();
    const bool hidden = IsHidden(attributes);
    if (!hidden && outline_->handoff == HandoffReason::kNone) {
      outline_->handoff = ClassifyHandoff(attributes);
    }
    if (IsSensitive(attributes)) {
      outline_->sensitive_page = true;
      return;
    }
    if (attributes.has_iframe_data()) {
      ++outline_->hidden;
      return;
    }
    if (mode_ == PageReadMode::kResearch && IsResearchBoilerplate(attributes)) {
      return;
    }
    in_heading |= kind == proto::CONTENT_ATTRIBUTE_HEADING;
    in_code |= attributes.aria_role() == proto::AX_ROLE_CODE ||
               attributes.aria_role() == proto::AX_ROLE_PREDEPRECATED;
    const auto& reasons =
        attributes.interaction_info().interaction_disabled_reasons();
    if (std::ranges::find(reasons,
                          proto::INTERACTION_DISABLED_REASON_ARIA_HIDDEN) ==
        reasons.end()) {
      in_form |= kind == proto::CONTENT_ATTRIBUTE_FORM;
      user_content |= attributes.user_generated_content() ||
                      attributes.aria_role() == proto::AX_ROLE_COMMENT ||
                      attributes.aria_role() == proto::AX_ROLE_FEED;
      if (kind == proto::CONTENT_ATTRIBUTE_HEADING && !hidden) {
        const auto heading = base::ToLowerASCII(
            attributes.text_data().text_content().substr(0, 120));
        user_content |= heading == "comments" || heading == "replies" ||
                        heading == "reviews" || heading == "discussion";
      }
      if (hidden) {
        ++outline_->hidden;
      } else {
        outline_->user_content |= user_content;
        Add(node, in_form, user_content, in_heading, in_code);
      }
      for (const auto& child : node.children_nodes()) {
        if (visited_ >= 20000) {
          break;
        }
        Visit(child, in_form, user_content, depth + 1, in_heading, in_code);
      }
      if (mode_ == PageReadMode::kResearch &&
          kind == proto::CONTENT_ATTRIBUTE_TABLE && !hidden) {
        outline_->rows.emplace_back(R"({"role":"table_end"})");
      }
    } else {
      ++outline_->hidden;
    }
  }

 private:
  void Add(const proto::ContentNode& node,
           bool in_form,
           bool user_content,
           bool in_heading,
           bool in_code) {
    const auto& attributes = node.content_attributes();
    const auto& control = attributes.form_control_data();
    const auto& interaction = attributes.interaction_info();
    const int node_id = attributes.common_ancestor_dom_node_id();
    const auto& label = attributes.label();
    const auto& text = attributes.text_data().text_content();
    const bool include_actions = mode_ == PageReadMode::kActionable;
    const bool research = mode_ == PageReadMode::kResearch;
    const bool research_link = research && attributes.has_anchor_data();
    const auto kind = attributes.attribute_type();
    const bool research_table_marker =
        research && (kind == proto::CONTENT_ATTRIBUTE_TABLE ||
                     kind == proto::CONTENT_ATTRIBUTE_TABLE_ROW ||
                     kind == proto::CONTENT_ATTRIBUTE_TABLE_CELL);
    if (!include_actions && attributes.has_form_control_data()) {
      return;
    }
    const bool actionable =
        include_actions &&
        (interaction.is_clickable() || attributes.has_form_control_data() ||
         attributes.has_anchor_data() || interaction.has_scroller_info());
    if (node_id > 0 && actionable && outline_->targets.size() < 4096) {
      outline_->targets.try_emplace(
          node_id,
          PageTarget{
              .document = outline_->document,
              .label = label.substr(0, 1024),
              .link = GURL(attributes.anchor_data().url()),
              .control_type = control.form_control_type(),
              .checked = control.has_is_checked()
                             ? std::optional(control.is_checked())
                             : std::nullopt,
              .disabled = interaction.is_disabled(),
              .in_form = in_form,
              .sensitive_field = !control.coarse_autofill_field_type().empty(),
              .actionable = actionable});
    }
    if (label.empty() && text.empty() && !actionable && !research_link &&
        !research_table_marker) {
      return;
    }
    if (!research &&
        (outline_->rows.size() >= 2048 || output_bytes_ >= 384 * 1024)) {
      outline_->truncated = true;
      return;
    }
    base::DictValue row;
    row.Set("role",
            proto::ContentAttributeType_Name(attributes.attribute_type()));
    if (research && in_code) {
      row.Set("code", true);
    }
    if (include_actions && node_id > 0) {
      row.Set("dom_node_id", node_id);
    }
    row.Set("actionable", actionable);
    if (include_actions) {
      row.Set("visible", true);
      if (interaction.has_is_disabled()) {
        row.Set("disabled", interaction.is_disabled());
      }
      if (control.has_is_checked()) {
        row.Set("checked", control.is_checked());
      }
    }
    AddText(row, "label", label, user_content);
    AddText(row, "text", text, user_content);
    if (attributes.has_form_control_data()) {
      row.Set("input_kind",
              proto::FormControlType_Name(control.form_control_type()));
      AddText(row, "placeholder", control.placeholder(), user_content);
      if (!control.field_value().empty()) {
        row.Set("value", filter_->Field(control.field_value(), *origin_));
      }
    }
    if (research_link) {
      std::string title;
      CollectLinkTitle(node, title, in_heading);
      if (title.empty()) {
        title = label.substr(0, 512);
      }
      outline_->instruction_like |=
          PrivacyFilter::LooksLikeInstruction(title, user_content);
      row.Set("research_link", static_cast<int>(outline_->links.size()));
      outline_->links.push_back({GURL(attributes.anchor_data().url()),
                                 filter_->Text(title, *origin_), in_heading});
    } else if (attributes.has_anchor_data()) {
      row.Set("url",
              filter_->Url(GURL(attributes.anchor_data().url()), *origin_));
    }
    auto json = base::WriteJson(row);
    if (json) {
      // Row text is JSON data. Strip tag spellings before adding bridge tags.
      base::ReplaceSubstringsAfterOffset(&*json, 0, "<user_content>", "");
      base::ReplaceSubstringsAfterOffset(&*json, 0, "</user_content>", "");
      if (user_content) {
        *json = base::StrCat({"<user_content>", *json, "</user_content>"});
      }
      output_bytes_ += json->size();
      if (include_actions && node_id > 0) {
        outline_->element_rows.try_emplace(node_id, outline_->rows.size());
      }
      outline_->rows.push_back(std::move(*json));
    }
  }

  void AddText(base::DictValue& row,
               std::string_view key,
               std::string_view text,
               bool user_content) {
    if (text.empty()) {
      return;
    }
    outline_->instruction_like |=
        PrivacyFilter::LooksLikeInstruction(text, user_content);
    outline_->contains_redactions |= text.size() > 1536;
    row.Set(key, filter_->Text(text.substr(0, 1536), *origin_));
  }

  const raw_ref<PageOutline> outline_;
  const raw_ref<PrivacyFilter> filter_;
  const raw_ref<const url::Origin> origin_;
  const PageReadMode mode_;
  size_t visited_ = 0;
  size_t output_bytes_ = 0;
};

}  // namespace

PageOutline::PageOutline() = default;
PageOutline::PageOutline(PageOutline&&) = default;
PageOutline& PageOutline::operator=(PageOutline&&) = default;
PageOutline::~PageOutline() = default;

bool PageOutline::ContainsVisible(const base::Value& condition) const {
  if (condition.is_int()) {
    return element_rows.contains(condition.GetInt());
  }
  for (std::string_view row : rows) {
    constexpr std::string_view opening = "<user_content>";
    constexpr std::string_view closing = "</user_content>";
    if (row.starts_with(opening) && row.ends_with(closing)) {
      row.remove_prefix(opening.size());
      row.remove_suffix(closing.size());
    }
    auto fields = base::JSONReader::ReadDict(row, base::JSON_PARSE_RFC);
    if (!fields) {
      continue;
    }
    for (const auto* key : {"label", "text"}) {
      const auto* text = fields->FindString(key);
      if (text && text->contains(condition.GetString())) {
        return true;
      }
    }
  }
  return false;
}

void ReadPage(content::WebContents& contents,
              optimization_guide::OnAIPageContentDone done) {
  auto options = optimization_guide::ActionableAIPageContentOptions(true);
  options->visible_content_only = true;
  options->include_same_site_only = true;
  options->include_passwords_for_redaction = true;
  options->include_sensitive_payments_for_redaction = true;
  options->include_otps_for_redaction = true;
  optimization_guide::GetAIPageContent(&contents, std::move(options),
                                       std::move(done));
}

PageOutline ExtractOutline(const proto::AnnotatedPageContent& content,
                           PrivacyFilter& filter,
                           const url::Origin& origin,
                           bool categorized_user_content,
                           PageReadMode mode) {
  PageOutline outline;
  outline.document =
      content.main_frame_data().document_identifier().serialized_token();
  outline.user_content = categorized_user_content;
  const auto redactions = filter.redaction_count();
  OutlineBuilder(outline, filter, origin, mode)
      .Visit(content.root_node(), false, categorized_user_content, 0);
  outline.contains_redactions |= redactions != filter.redaction_count();
  return outline;
}

std::string WrapContent(std::string text,
                        const url::Origin& origin,
                        std::string_view tab,
                        std::string_view source_id) {
  std::array<uint8_t, 16> random;
  base::RandBytes(random);
  const std::string boundary = base::HexEncode(random);
  base::ReplaceSubstringsAfterOffset(&text, 0, boundary, "");
  base::ReplaceSubstringsAfterOffset(&text, 0, "untrusted_web_content", "");
  const std::string source_attribute =
      source_id.empty() ? "" : base::StrCat({" source=\"", source_id, "\""});
  const std::string tab_attribute =
      tab.empty() ? "" : base::StrCat({" tab=\"", tab, "\""});
  return base::StrCat(
      {"<untrusted_web_content boundary=\"", boundary, "\" origin=\"",
       origin.Serialize(), "\"", tab_attribute, " read=\"",
       base::TimeFormatAsIso8601(base::Time::Now()), "\"", source_attribute,
       ">\n", text, "\n</untrusted_web_content boundary=\"", boundary, "\">"});
}

}  // namespace krypton_agent
