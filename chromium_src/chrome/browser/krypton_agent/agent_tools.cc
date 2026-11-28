#include "chrome/browser/krypton_agent/agent_tools.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

#include "base/no_destructor.h"
#include "base/time/time.h"
#include "chrome/browser/actor/tools/click_tool_request.h"
#include "chrome/browser/actor/tools/drag_and_release_tool_request.h"
#include "chrome/browser/actor/tools/find_and_highlight_tool_request.h"
#include "chrome/browser/actor/tools/history_tool_request.h"
#include "chrome/browser/actor/tools/media_control_tool_request.h"
#include "chrome/browser/actor/tools/move_mouse_tool_request.h"
#include "chrome/browser/actor/tools/navigate_tool_request.h"
#include "chrome/browser/actor/tools/open_url_tool_request.h"
#include "chrome/browser/actor/tools/perform_search_tool_request.h"
#include "chrome/browser/actor/tools/registry/tool_registry.h"
#include "chrome/browser/actor/tools/scroll_to_tool_request.h"
#include "chrome/browser/actor/tools/scroll_tool_request.h"
#include "chrome/browser/actor/tools/select_tool_request.h"
#include "chrome/browser/actor/tools/type_tool_request.h"
#include "chrome/browser/actor/tools/wait_tool_request.h"
#include "chrome/browser/krypton_agent/organization.h"
#include "chrome/browser/krypton_agent/page_reader.h"
#include "chrome/common/actor.mojom.h"
#include "components/actor/core/shared_types.h"

namespace krypton_agent {
namespace {

constexpr std::array<std::string_view, 14> kActions{
    "navigate",       "go_back",     "go_forward",    "reload_page",
    "click",          "type",        "select_option", "scroll",
    "scroll_to",      "scroll_page", "wait",          "find_and_highlight",
    "perform_search", "open_url"};

struct KryptonTool {
  KryptonToolId id;
  std::string_view name;
  std::string_view description;
  base::DictValue (*schema)();
};

base::DictValue ReadPageSchema() {
  return base::DictValue()
      .Set("type", "object")
      .Set("properties",
           base::DictValue().Set("continuation",
                                 base::DictValue().Set("type", "integer")))
      .Set("additionalProperties", false);
}

base::DictValue HandoffSchema() {
  base::ListValue reasons;
  for (const auto* reason :
       {"sign_in", "verify", "enter_information", "take_over"}) {
    reasons.Append(reason);
  }
  return base::DictValue()
      .Set("type", "object")
      .Set("properties", base::DictValue().Set(
                             "reason", base::DictValue()
                                           .Set("type", "string")
                                           .Set("enum", std::move(reasons))))
      .Set("required", base::ListValue().Append("reason"))
      .Set("additionalProperties", false);
}

base::DictValue HtmlProperty() {
  return base::DictValue()
      .Set("type", "string")
      .Set("minLength", 1)
      .Set("maxLength", kMaxHtmlBytes);
}

base::DictValue Dimension(int minimum, int maximum) {
  return base::DictValue()
      .Set("type", "integer")
      .Set("minimum", minimum)
      .Set("maximum", maximum);
}

base::DictValue HtmlRenderSchema() {
  return base::DictValue()
      .Set("type", "object")
      .Set("properties",
           base::DictValue()
               .Set("html", HtmlProperty())
               .Set("title", base::DictValue()
                                 .Set("type", "string")
                                 .Set("minLength", 1)
                                 .Set("maxLength", kMaxHtmlTitleBytes))
               .Set("height", Dimension(kMinHtmlHeight, kMaxHtmlHeight)))
      .Set("required", base::ListValue().Append("html").Append("title"))
      .Set("additionalProperties", false);
}

base::DictValue HtmlPreviewSchema() {
  auto bounded_string = [](int limit) {
    return base::DictValue().Set("type", "string").Set("maxLength", limit);
  };
  auto step = base::DictValue()
                  .Set("type", "object")
                  .Set("properties",
                       base::DictValue()
                           .Set("action", base::DictValue()
                                              .Set("type", "string")
                                              .Set("enum", base::ListValue()
                                                               .Append("click")
                                                               .Append("type")
                                                               .Append("press")
                                                               .Append("wait")))
                           .Set("selector", bounded_string(256))
                           .Set("text", bounded_string(4096))
                           .Set("key", bounded_string(32))
                           .Set("milliseconds", Dimension(0, 1000)))
                  .Set("required", base::ListValue().Append("action"))
                  .Set("additionalProperties", false);
  return base::DictValue()
      .Set("type", "object")
      .Set("properties",
           base::DictValue()
               .Set("html", HtmlProperty())
               .Set("width", Dimension(kMinHtmlWidth, kMaxHtmlWidth))
               .Set("appearance",
                    base::DictValue()
                        .Set("type", "string")
                        .Set("enum",
                             base::ListValue().Append("light").Append("dark")))
               .Set("steps", base::DictValue()
                                 .Set("type", "array")
                                 .Set("maxItems", 12)
                                 .Set("items", std::move(step))))
      .Set("required", base::ListValue().Append("html"))
      .Set("additionalProperties", false);
}

// Add Krypton tools here and route their IDs in the session. All requests
// reach the session through the same scope, limits, and confirmation checks.
constexpr std::string_view kHtmlGuideTopics[] = {
    "widgets", "charts", "report", "slides", "motion",
};

base::DictValue HtmlGuideSchema() {
  base::ListValue topics;
  for (std::string_view topic : kHtmlGuideTopics) {
    topics.Append(topic);
  }
  return base::DictValue()
      .Set("type", "object")
      .Set("properties",
           base::DictValue().Set("topic", base::DictValue()
                                              .Set("type", "string")
                                              .Set("enum", std::move(topics))))
      .Set("required", base::ListValue().Append("topic"))
      .Set("additionalProperties", false);
}

base::DictValue ToolSchema(base::DictValue properties,
                           base::ListValue required) {
  return base::DictValue()
      .Set("type", "object")
      .Set("properties", std::move(properties))
      .Set("required", std::move(required))
      .Set("additionalProperties", false);
}

base::DictValue NodeIdProperty() {
  return base::DictValue().Set("type", "integer").Set("minimum", 1);
}

constexpr std::pair<std::string_view, std::string_view> kHelpTopics[] = {
    {"spaces",
     "Spaces group tabs into separate work areas. Switch Spaces from "
     "the sidebar. Some Spaces use their own profile."},
    {"essentials",
     "Essentials are shortcuts in the sidebar. Use a tab's menu "
     "to add it to or remove it from Essentials."},
    {"folders",
     "Folders group related tabs in a Space. Use the sidebar to "
     "create a folder and move tabs into it."},
    {"splits",
     "Split view shows tabs side by side. Use the tab menu to "
     "enter or leave a split."},
    {"glance",
     "Glance opens a link in a temporary card. Close it to return to "
     "the page underneath, or open the link as a tab."},
    {"notes",
     "Notes keep text in Krypton. Create a note from the sidebar and "
     "keep it in the Space where you need it."},
    {"easels",
     "Easels let you arrange items on a canvas. Create an easel from "
     "the sidebar in the Space where you need it."},
    {"agent_permissions",
     "Enable AI in Privacy. Choose the browser permissions "
     "you want when starting a chat. A tab attachment "
     "shares filtered text for the current message."},
    {"settings",
     "Open Settings from the browser menu. Privacy controls AI and "
     "which sites the agent can navigate to."},
};

base::DictValue HelpSchema() {
  base::ListValue topics;
  for (const auto& [topic, guide] : kHelpTopics) {
    topics.Append(topic);
  }
  return ToolSchema(
      base::DictValue().Set("topic", base::DictValue()
                                         .Set("type", "string")
                                         .Set("enum", std::move(topics))),
      base::ListValue().Append("topic"));
}

constexpr KryptonTool kKryptonTools[]{
    {KryptonToolId::kWebSearch, "web_search",
     "Search the default engine in the background. Returns filtered results "
     "with link IDs. Pass the returned continuation alone for more results.",
     [] {
       return ToolSchema(
           base::DictValue()
               .Set("queries",
                    base::DictValue()
                        .Set("type", "array")
                        .Set("minItems", 1)
                        .Set("items", base::DictValue().Set("type", "string")))
               .Set("continuation", NodeIdProperty()),
           base::ListValue());
     }},
    {KryptonToolId::kReadUrl, "read_url",
     "Read HTTP or HTTPS addresses or link IDs in the background. Retry with "
     "use_sign_in only after a sign-in refusal. Pass the returned "
     "continuation alone for more text.",
     [] {
       return ToolSchema(
           base::DictValue()
               .Set("targets",
                    base::DictValue()
                        .Set("type", "array")
                        .Set("minItems", 1)
                        .Set("items", base::DictValue().Set("type", "string")))
               .Set("use_sign_in", base::DictValue().Set("type", "boolean"))
               .Set("continuation", NodeIdProperty()),
           base::ListValue());
     }},
    {KryptonToolId::kReadPage, "read_page",
     "Read a filtered outline of the shared tab.", &ReadPageSchema},
    {KryptonToolId::kReadContextTab, "read_context_tab",
     "Read the tab the user attached to this message. Read-only.",
     [] {
       return base::DictValue()
           .Set("type", "object")
           .Set("properties", base::DictValue())
           .Set("additionalProperties", false);
     }},
    {KryptonToolId::kRequestHandoff, "request_handoff",
     "Ask the user to take control of the shared tab.", &HandoffSchema},
    {KryptonToolId::kMoveMouse, "move_mouse",
     "Move the pointer over an observed page element.",
     [] {
       return ToolSchema(base::DictValue().Set("dom_node_id", NodeIdProperty()),
                         base::ListValue().Append("dom_node_id"));
     }},
    {KryptonToolId::kDragAndRelease, "drag_and_release",
     "Drag between two observed page elements. Requires confirmation.",
     [] {
       return ToolSchema(base::DictValue()
                             .Set("from_dom_node_id", NodeIdProperty())
                             .Set("to_dom_node_id", NodeIdProperty()),
                         base::ListValue()
                             .Append("from_dom_node_id")
                             .Append("to_dom_node_id"));
     }},
    {KryptonToolId::kSetChecked, "set_checked",
     "Set an observed checkbox to the requested state.",
     [] {
       return ToolSchema(
           base::DictValue()
               .Set("dom_node_id", NodeIdProperty())
               .Set("checked", base::DictValue().Set("type", "boolean")),
           base::ListValue().Append("dom_node_id").Append("checked"));
     }},
    {KryptonToolId::kReadElement, "read_element",
     "Read the filtered data for a visible element from your latest page "
     "reading.",
     [] {
       return ToolSchema(base::DictValue().Set("dom_node_id", NodeIdProperty()),
                         base::ListValue().Append("dom_node_id"));
     }},
    {KryptonToolId::kListWorkTabs, "krypton_list_work_tabs",
     "List the tabs this chat opened for browser tools.",
     [] { return ToolSchema({}, {}); }},
    {KryptonToolId::kCloseWorkTab, "krypton_close_work_tab",
     "Close a tab this chat opened. Use its ID from krypton_list_work_tabs.",
     [] {
       return ToolSchema(base::DictValue().Set(
                             "tab", base::DictValue().Set("type", "string")),
                         base::ListValue().Append("tab"));
     }},
    {KryptonToolId::kCapturePage, "capture_page",
     "Capture the visible page as a PNG. Requires approval for each capture.",
     [] { return ToolSchema({}, {}); }},
    {KryptonToolId::kPlayMedia, "play_media", "Play media in the page tab.",
     [] { return ToolSchema({}, {}); }},
    {KryptonToolId::kPauseMedia, "pause_media", "Pause media in the page tab.",
     [] { return ToolSchema({}, {}); }},
    {KryptonToolId::kSeekMedia, "seek_media",
     "Move media playback to the given number of seconds from the start.",
     [] {
       return ToolSchema(
           base::DictValue().Set(
               "seconds",
               base::DictValue().Set("type", "number").Set("minimum", 0)),
           base::ListValue().Append("seconds"));
     }},
    {KryptonToolId::kHelp, "krypton_help",
     "Read local help for Krypton features.", &HelpSchema},
    {KryptonToolId::kHtmlRender, "html_render",
     "Show a self-contained HTML chart, diagram or table in the transcript. "
     "Use inline HTML and the local kit. Call html_guide for the kit, then "
     "html_preview. Network access and file paths are unavailable.",
     &HtmlRenderSchema},
    {KryptonToolId::kHtmlPreview, "html_preview",
     "Preview self-contained HTML and return a PNG and content height. "
     "Use before html_render. See html_guide for the local kit.",
     &HtmlPreviewSchema},
    {KryptonToolId::kHtmlGuide, "html_guide",
     "Read Krypton's local HTML render guide for a specific topic.",
     &HtmlGuideSchema},
};

const KryptonTool* KryptonDefinition(std::string_view tool) {
  const auto* found =
      std::ranges::find(kKryptonTools, tool, &KryptonTool::name);
  return found == std::end(kKryptonTools) ? nullptr : found;
}

bool IsAction(std::string_view tool) {
  return std::ranges::find(kActions, tool) != kActions.end();
}

const actor::ToolDefinition* Definition(const actor::ToolRegistry& registry,
                                        std::string_view name) {
  auto definitions = registry.GetAllTools();
  const auto it =
      std::ranges::find(definitions, name, &actor::ToolDefinition::name);
  if (it != definitions.end()) {
    return &*it;
  }
  if (name == "perform_search") {
    static const base::NoDestructor<std::optional<actor::ToolDefinition>>
        search(actor::PerformSearchToolRequest::GetToolDefinition());
    return search->has_value() ? &search->value() : nullptr;
  }
  if (name == "open_url") {
    static const base::NoDestructor<std::optional<actor::ToolDefinition>>
        open_url(actor::OpenUrlToolRequest::GetToolDefinition());
    return open_url->has_value() ? &open_url->value() : nullptr;
  }
  if (name == "go_back") {
    static const base::NoDestructor<std::optional<actor::ToolDefinition>> back(
        actor::HistoryBackToolRequest::GetToolDefinition());
    return back->has_value() ? &back->value() : nullptr;
  }
  if (name == "go_forward") {
    static const base::NoDestructor<std::optional<actor::ToolDefinition>>
        forward(actor::HistoryForwardToolRequest::GetToolDefinition());
    return forward->has_value() ? &forward->value() : nullptr;
  }
  return nullptr;
}

base::DictValue ActionSchema(const actor::ToolDefinition& tool) {
  auto schema = tool.parameters_json_schema.Clone();
  auto* properties = schema.FindDict("properties");
  if (tool.name == "click" && properties) {
    properties->Set("click_count", base::DictValue()
                                       .Set("type", "integer")
                                       .Set("minimum", 1)
                                       .Set("maximum", 2));
  } else if (tool.name == "type" && properties) {
    properties->Set(
        "mode",
        base::DictValue()
            .Set("type", "string")
            .Set("enum",
                 base::ListValue().Append("replace").Append("prepend").Append(
                     "append")));
  } else if (tool.name == "wait" && properties) {
    properties->Set(
        "until",
        base::DictValue()
            .Set("description", "Visible text or a DOM node ID.")
            .Set("oneOf", base::ListValue()
                              .Append(base::DictValue().Set("type", "string"))
                              .Append(NodeIdProperty())));
  } else if (tool.name == "open_url" && properties) {
    schema.Set("required", base::ListValue().Append("url"));
    properties->Set("new_tab", base::DictValue()
                                   .Set("type", "boolean")
                                   .Set("description",
                                        "Open a new work tab for this chat."));
  }
  return schema;
}

bool MatchesProperty(const base::Value& value,
                     const base::DictValue& property) {
  const auto* type = property.FindString("type");
  if (!type) {
    return false;
  }
  if (*type == "integer" || *type == "number") {
    if ((*type == "integer" && !value.is_int()) ||
        (!value.is_int() && !value.is_double())) {
      return false;
    }
    const double number = value.GetDouble();
    return std::isfinite(number) &&
           number >= property.FindDouble("minimum").value_or(number) &&
           number <= property.FindDouble("maximum").value_or(number);
  }
  if (*type == "boolean") {
    return value.is_bool();
  }
  if (*type != "string" || !value.is_string() ||
      value.GetString().size() > 4096) {
    return false;
  }
  const auto* choices = property.FindList("enum");
  return !choices || std::ranges::find(*choices, value) != choices->end();
}

using Direction = actor::ScrollToolRequest::Direction;

std::optional<Direction> ParseDirection(std::string_view value) {
  if (value == "left") {
    return Direction::kLeft;
  }
  if (value == "right") {
    return Direction::kRight;
  }
  if (value == "up") {
    return Direction::kUp;
  }
  if (value == "down") {
    return Direction::kDown;
  }
  return std::nullopt;
}

std::string_view StringArg(const base::DictValue& arguments,
                           std::string_view key) {
  if (const auto* result = arguments.FindString(key)) {
    return *result;
  }
  return {};
}

}  // namespace

std::optional<KryptonToolId> FindKryptonTool(std::string_view name) {
  const auto* tool = KryptonDefinition(name);
  if (!tool) {
    return std::nullopt;
  }
  return tool->id;
}

base::ListValue ListTools(const actor::ToolRegistry& registry) {
  base::ListValue tools;
  for (const auto& tool : kKryptonTools) {
    tools.Append(base::DictValue()
                     .Set("name", tool.name)
                     .Set("description", tool.description)
                     .Set("inputSchema", tool.schema()));
  }
  auto append = [&](const actor::ToolDefinition& tool) {
    std::string_view description = tool.description;
    base::DictValue schema = ActionSchema(tool);
    if (tool.name == "find_and_highlight") {
      description = "Find matching text in the shared tab.";
    } else if (tool.name == "open_url") {
      description =
          "Open an HTTP or HTTPS address or link ID in the shared tab or a "
          "new work tab.";
    }
    tools.Append(base::DictValue()
                     .Set("name", tool.name)
                     .Set("description", description)
                     .Set("inputSchema", std::move(schema)));
  };
  for (const auto& tool : registry.GetAllTools()) {
    if (IsAction(tool.name)) {
      append(tool);
    }
  }
  for (const auto name :
       {"perform_search", "open_url", "go_back", "go_forward"}) {
    if (std::ranges::find(registry.GetAllTools(), name,
                          &actor::ToolDefinition::name) ==
        registry.GetAllTools().end()) {
      if (const auto* tool = Definition(registry, name)) {
        append(*tool);
      }
    }
  }
  Organization::List(tools);
  return tools;
}

namespace {

bool ValidPreviewSteps(const base::ListValue& steps) {
  if (steps.size() > 12) {
    return false;
  }
  int wait_time = 0;
  for (const auto& value : steps) {
    const auto* step = value.GetIfDict();
    if (!step) {
      return false;
    }
    const auto action = StringArg(*step, "action");
    if (action == "wait") {
      const int delay = step->FindInt("milliseconds").value_or(-1);
      if (step->size() != 2 || delay < 0 || delay > 1000) {
        return false;
      }
      wait_time += delay;
      continue;
    }
    const auto selector = StringArg(*step, "selector");
    if (selector.empty() || selector.size() > 256) {
      return false;
    }
    if (action == "click" && step->size() == 2) {
      continue;
    }
    if (action == "type" && step->size() == 3) {
      if (const auto* input = step->FindString("text");
          input && input->size() <= 4096) {
        continue;
      }
    }
    if (action == "press" && step->size() == 3) {
      const auto key = StringArg(*step, "key");
      if (!key.empty() && key.size() <= 32) {
        continue;
      }
    }
    return false;
  }
  return wait_time <= 3000;
}

}  // namespace

bool ValidHtmlArguments(const base::DictValue& arguments, bool preview) {
  const auto html = StringArg(arguments, "html");
  if (html.empty() || html.size() > kMaxHtmlBytes ||
      std::ranges::any_of(html, [](unsigned char c) {
        return c < 0x20 && c != '\n' && c != '\r' && c != '\t';
      })) {
    return false;
  }
  for (const auto [key, value] : arguments) {
    if (key == "html") {
      continue;
    }
    if (!preview) {
      if ((key == "title" && value.is_string() && !value.GetString().empty() &&
           value.GetString().size() <= kMaxHtmlTitleBytes) ||
          (key == "height" && value.is_int() &&
           value.GetInt() >= kMinHtmlHeight &&
           value.GetInt() <= kMaxHtmlHeight)) {
        continue;
      }
    } else if ((key == "width" && value.is_int() &&
                value.GetInt() >= kMinHtmlWidth &&
                value.GetInt() <= kMaxHtmlWidth) ||
               (key == "appearance" && value.is_string() &&
                (value.GetString() == "dark" ||
                 value.GetString() == "light")) ||
               (key == "steps" && value.is_list() &&
                ValidPreviewSteps(value.GetList()))) {
      continue;
    }
    return false;
  }
  return preview || arguments.contains("title");
}

bool ValidArguments(const actor::ToolRegistry& registry,
                    std::string_view tool,
                    const base::DictValue& arguments) {
  if (const auto* organization = Organization::Find(tool)) {
    return Organization::Valid(*organization, arguments);
  }
  if (tool == "html_render" || tool == "html_preview") {
    return ValidHtmlArguments(arguments, tool == "html_preview");
  }
  if (tool == "html_guide") {
    const auto topic = StringArg(arguments, "topic");
    return arguments.size() == 1 &&
           std::ranges::contains(kHtmlGuideTopics, topic);
  }
  if (tool == "web_search" || tool == "read_url") {
    if (arguments.contains("continuation")) {
      return arguments.size() == 1 &&
             arguments.FindInt("continuation").value_or(0) > 0;
    }
    const auto* inputs =
        arguments.FindList(tool == "web_search" ? "queries" : "targets");
    if (!inputs || inputs->empty() ||
        (arguments.size() != 1 &&
         (tool != "read_url" || arguments.size() != 2 ||
          !arguments.FindBool("use_sign_in").has_value()))) {
      return false;
    }
    return std::ranges::all_of(*inputs, [](const base::Value& value) {
      return value.is_string() && !value.GetString().empty() &&
             value.GetString().size() <= 4096;
    });
  }
  if (FindKryptonTool(tool) == KryptonToolId::kReadPage) {
    return arguments.empty() ||
           (arguments.size() == 1 &&
            arguments.FindInt("continuation").value_or(0) > 0);
  }
  if (FindKryptonTool(tool) == KryptonToolId::kReadContextTab) {
    return arguments.empty();
  }
  if (FindKryptonTool(tool) == KryptonToolId::kRequestHandoff) {
    const auto* reason = arguments.FindString("reason");
    return arguments.size() == 1 && reason &&
           (*reason == "sign_in" || *reason == "verify" ||
            *reason == "enter_information" || *reason == "take_over");
  }
  if (tool == "open_url") {
    const auto url = StringArg(arguments, "url");
    return !url.empty() && url.size() <= 4096 &&
           (arguments.size() == 1 ||
            (arguments.size() == 2 &&
             arguments.FindBool("new_tab").has_value()));
  }
  const auto* local = KryptonDefinition(tool);
  const auto* actor = IsAction(tool) ? Definition(registry, tool) : nullptr;
  if (!local && !actor) {
    return false;
  }
  const auto schema = local ? local->schema() : ActionSchema(*actor);
  const auto* properties = schema.FindDict("properties");
  for (const auto [name, value] : arguments) {
    if (tool == "wait" && name == "until") {
      if ((value.is_int() && value.GetInt() > 0) ||
          (value.is_string() && !value.GetString().empty() &&
           value.GetString().size() <= 4096)) {
        continue;
      }
      return false;
    }
    const auto* property = properties ? properties->FindDict(name) : nullptr;
    if (!property || !MatchesProperty(value, *property)) {
      return false;
    }
  }
  if (const auto* required = schema.FindList("required")) {
    for (const auto& name : *required) {
      if (!name.is_string() || !arguments.contains(name.GetString())) {
        return false;
      }
    }
  }
  return true;
}

std::string_view GetKryptonHelp(std::string_view topic) {
  for (const auto& [name, guide] : kHelpTopics) {
    if (name == topic) {
      return guide;
    }
  }
  return {};
}

std::unique_ptr<actor::ToolRequest> BuildRequest(
    std::string_view tool,
    const base::DictValue& arguments,
    actor::ActorSurfaceHandle surface,
    const PageOutline& page) {
  if (tool == "perform_search") {
    const auto terms = StringArg(arguments, "query");
    if (!terms.empty()) {
      return std::make_unique<actor::PerformSearchToolRequest>(
          surface, std::string(terms));
    }
    return {};
  }
  if (tool == "open_url") {
    return std::make_unique<actor::OpenUrlToolRequest>(
        surface, GURL(StringArg(arguments, "url")), /*open_in_new_tab=*/false);
  }
  if (tool == "navigate") {
    GURL url(StringArg(arguments, "url"));
    if (url.SchemeIsHTTPOrHTTPS()) {
      return std::make_unique<actor::NavigateToolRequest>(surface,
                                                          std::move(url));
    }
    return {};
  }
  if (tool == "reload_page") {
    return std::make_unique<actor::ReloadPageToolRequest>(surface);
  }
  if (tool == "wait") {
    const int duration = arguments.FindInt("wait_time_ms").value_or(0);
    if (duration > 0 && duration <= 5000) {
      return std::make_unique<actor::WaitToolRequest>(
          base::Milliseconds(duration), surface);
    }
    return {};
  }
  if (tool == "find_and_highlight") {
    const auto query = StringArg(arguments, "query");
    if (!query.empty()) {
      return std::make_unique<actor::FindAndHighlightToolRequest>(
          surface, std::string(query));
    }
    return {};
  }
  if (tool == "play_media") {
    return std::make_unique<actor::PlayMediaToolRequest>(surface);
  }
  if (tool == "pause_media") {
    return std::make_unique<actor::PauseMediaToolRequest>(surface);
  }
  if (tool == "seek_media") {
    if (const auto seconds = arguments.FindDouble("seconds");
        seconds && *seconds >= 0 &&
        *seconds < base::TimeDelta::Max().InSecondsF()) {
      return std::make_unique<actor::SeekMediaToolRequest>(
          surface, base::Seconds(*seconds));
    }
    return {};
  }
  if (tool == "drag_and_release") {
    const int from = arguments.FindInt("from_dom_node_id").value_or(0);
    const int to = arguments.FindInt("to_dom_node_id").value_or(0);
    if (from == to || !page.targets.contains(from) ||
        !page.targets.contains(to) || page.document.empty()) {
      return {};
    }
    return std::make_unique<actor::DragAndReleaseToolRequest>(
        surface, actor::DomNode{from, page.document},
        actor::DomNode{to, page.document});
  }
  const bool can_scroll_viewport = tool == "scroll" || tool == "scroll_page";
  const auto node_id = arguments.FindInt("dom_node_id");
  if ((!node_id || *node_id == 0) && !can_scroll_viewport) {
    return {};
  }
  const int target_id = node_id.value_or(0);
  if (target_id && !page.targets.contains(target_id)) {
    return {};
  }
  if (page.document.empty()) {
    return {};
  }
  const actor::PageTarget target = actor::DomNode{
      .node_id = target_id, .document_identifier = page.document};
  if (tool == "click" || tool == "set_checked") {
    return std::make_unique<actor::ClickToolRequest>(
        surface, target, actor::mojom::ClickType::kLeft,
        arguments.FindInt("click_count") == 2
            ? actor::mojom::ClickCount::kDouble
            : actor::mojom::ClickCount::kSingle);
  }
  if (tool == "move_mouse") {
    return std::make_unique<actor::MoveMouseToolRequest>(surface, target);
  }
  if (tool == "type") {
    using Mode = actor::TypeToolRequest::Mode;
    Mode mode = Mode::kReplace;
    const auto requested = StringArg(arguments, "mode");
    if (requested == "prepend") {
      mode = Mode::kPrepend;
    } else if (requested == "append") {
      mode = Mode::kAppend;
    }
    return std::make_unique<actor::TypeToolRequest>(
        surface, target, StringArg(arguments, "text"),
        arguments.FindBool("follow_by_enter").value_or(false), mode);
  }
  if (tool == "select_option") {
    return std::make_unique<actor::SelectToolRequest>(
        surface, target, StringArg(arguments, "value"));
  }
  if (tool == "scroll_to") {
    return std::make_unique<actor::ScrollToToolRequest>(surface, target);
  }
  const auto direction = ParseDirection(StringArg(arguments, "direction"));
  if (!direction) {
    return {};
  }
  if (tool == "scroll") {
    const double distance = arguments.FindDouble("distance").value_or(0);
    if (distance > 0 && distance <= 10000) {
      return std::make_unique<actor::ScrollToolRequest>(
          surface, target, *direction, static_cast<float>(distance));
    }
  }
  if (tool == "scroll_page") {
    const auto granularity = StringArg(arguments, "granularity");
    using Granularity = actor::ScrollPageToolRequest::Granularity;
    if (granularity == "page" || granularity == "document") {
      return std::make_unique<actor::ScrollPageToolRequest>(
          surface, target,
          granularity == "page" ? Granularity::kPage : Granularity::kDocument,
          *direction);
    }
  }
  return {};
}

}  // namespace krypton_agent
