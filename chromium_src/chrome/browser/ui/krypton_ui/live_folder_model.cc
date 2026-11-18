// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/live_folder_model.h"

#include <algorithm>
#include <set>
#include <string_view>
#include "base/json/json_reader.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "crypto/hash.h"
#include "net/base/url_util.h"
#include "services/data_decoder/public/cpp/safe_xml_parser.h"

namespace krypton_ui {
namespace {
constexpr size_t kMaxSourceLength = 2048;
constexpr size_t kMaxTitleLength = 1024;

bool SafeUrl(const GURL& url) {
  return url.is_valid() && url.SchemeIsHTTPOrHTTPS() && !url.has_username() &&
         !url.has_password() && url.spec().size() <= kMaxSourceLength;
}

// A bounded text/attribute scanner, not an HTML interpreter. No scripts,
// subresources or page DOM are created for a provider response. As in HTML,
// a quote opens a value only directly after '='; stray quotes, such as those
// in GitHub's `<!-- '"` -->` form guards, must not consume the whole page.
size_t TagEnd(std::string_view html, size_t start) {
  for (size_t i = start; i < html.size(); ++i) {
    if (html[i] == '>')
      return i;
    if (html[i] != '=')
      continue;
    size_t value = i + 1;
    while (value < html.size() && base::IsAsciiWhitespace(html[value]))
      ++value;
    if (value < html.size() && (html[value] == '"' || html[value] == '\'')) {
      i = html.find(html[value], value + 1);
      if (i == std::string_view::npos)
        return i;
    } else {
      i = value - 1;
    }
  }
  return std::string_view::npos;
}

std::string Text(std::string_view html) {
  std::string text;
  text.reserve(std::min(html.size(), kMaxTitleLength));
  for (size_t i = 0; i < html.size() && text.size() < kMaxTitleLength;) {
    if (html[i] == '<') {
      const auto end = TagEnd(html, i + 1);
      if (end == std::string_view::npos)
        break;
      i = end + 1;
    } else {
      text.push_back(html[i++]);
    }
  }
  return base::UTF16ToUTF8(base::CollapseWhitespace(
      base::UnescapeForHTML(base::UTF8ToUTF16(text)), true));
}

std::string Attribute(std::string_view tag, std::string_view key) {
  size_t i = tag.find_first_of(" \t\r\n");
  while (i != std::string_view::npos && i < tag.size()) {
    while (i < tag.size() && base::IsAsciiWhitespace(tag[i]))
      ++i;
    const auto begin = i;
    while (i < tag.size() && !base::IsAsciiWhitespace(tag[i]) &&
           tag[i] != '=' && tag[i] != '/')
      ++i;
    const auto name = tag.substr(begin, i - begin);
    while (i < tag.size() && base::IsAsciiWhitespace(tag[i]))
      ++i;
    if (i == tag.size())
      break;
    if (tag[i] != '=') {
      ++i;
      continue;
    }
    ++i;
    while (i < tag.size() && base::IsAsciiWhitespace(tag[i]))
      ++i;
    if (i == tag.size())
      break;
    const char quote = tag[i] == '\'' || tag[i] == '"' ? tag[i++] : 0;
    const auto value_begin = i;
    while (i < tag.size() &&
           (quote ? tag[i] != quote : !base::IsAsciiWhitespace(tag[i])))
      ++i;
    if (name == key) {
      return base::UTF16ToUTF8(base::UnescapeForHTML(
          base::UTF8ToUTF16(tag.substr(value_begin, i - value_begin))));
    }
    if (quote && i < tag.size())
      ++i;
  }
  return {};
}

struct HtmlTag {
  std::string_view name;
  std::string_view text;  // Between '<' and '>'.
  std::string_view raw;   // Script/style content, already skipped.
};

// Returns the next start tag at or after `at` and moves past it. Comments,
// end tags, declarations and literal '<' text are skipped; an unterminated
// construct ends the scan rather than guessing.
std::optional<HtmlTag> NextTag(std::string_view html, size_t& at) {
  while ((at = html.find('<', at)) != std::string_view::npos) {
    const auto rest = html.substr(at + 1);
    if (rest.starts_with("!--")) {
      const auto end = html.find("-->", at + 2);
      if (end == std::string_view::npos)
        break;
      at = end + 3;
      continue;
    }
    const bool start = !rest.empty() && base::IsAsciiAlpha(rest.front());
    if (!start &&
        (rest.empty() ||
         (rest.front() != '/' && rest.front() != '!' && rest.front() != '?'))) {
      ++at;
      continue;
    }
    const auto end = start ? TagEnd(html, at + 1) : html.find('>', at + 1);
    if (end == std::string_view::npos)
      break;
    HtmlTag tag;
    tag.text = html.substr(at + 1, end - at - 1);
    tag.name = tag.text.substr(0, tag.text.find_first_of(" \t\r\n\f/"));
    at = end + 1;
    if (!start)
      continue;
    const bool script = base::EqualsCaseInsensitiveASCII(tag.name, "script");
    if (script || base::EqualsCaseInsensitiveASCII(tag.name, "style")) {
      const auto close = html.find(script ? "</script" : "</style", at);
      if (close == std::string_view::npos)
        break;
      tag.raw = html.substr(at, close - at);
      at = close;
    }
    return tag;
  }
  at = std::string_view::npos;
  return std::nullopt;
}

// GitHub renders an empty user-login value for a signed-out request. Only the
// document head is inspected; cookies are never read or copied.
bool GithubSignedOut(std::string_view html) {
  size_t at = 0;
  while (const auto tag = NextTag(html, at)) {
    if (base::EqualsCaseInsensitiveASCII(tag->name, "body"))
      return false;
    if (base::EqualsCaseInsensitiveASCII(tag->name, "meta") &&
        Attribute(tag->text, "name") == "user-login")
      return Attribute(tag->text, "content").empty();
  }
  return false;
}

std::string LocalName(const base::Value& node) {
  std::string name;
  data_decoder::GetXmlElementTagName(node, &name);
  if (const auto colon = name.rfind(':'); colon != std::string::npos)
    name.erase(0, colon + 1);
  return name;
}

const base::Value* Child(const base::Value& node, std::string_view name) {
  if (const auto* children = data_decoder::GetXmlElementChildren(node))
    for (const auto& child : *children)
      if (LocalName(child) == name)
        return &child;
  return nullptr;
}

std::string NodeText(const base::Value* node, unsigned depth = 0) {
  if (!node || depth > 8)
    return {};
  std::string result;
  if (data_decoder::GetXmlElementText(*node, &result))
    return result.substr(0, kMaxSourceLength);
  if (const auto* children = data_decoder::GetXmlElementChildren(*node)) {
    for (const auto& child : *children) {
      result += NodeText(&child, depth + 1);
      if (result.size() >= kMaxSourceLength) {
        result.resize(kMaxSourceLength);
        break;
      }
    }
  }
  return result;
}

void AddItem(LiveFolderItems& items,
             std::set<std::string>& seen,
             const LiveFolderConfig& config,
             std::string_view id,
             std::string title,
             const GURL& url,
             bool markup = true) {
  if (items.size() >= config.max_items || !SafeUrl(url) || id.empty() ||
      id.size() > kMaxSourceLength || !base::IsStringUTF8(title))
    return;
  // Persistent bounded identifiers, including the source, isolate dismissals
  // after configuration changes. The digest is identity, not authentication.
  const auto digest = crypto::hash::Sha256(base::NumberToString(config.kind) +
                                           ":" + config.source + "\n" +
                                           std::string(id) + "\n" + url.spec());
  const auto key = base::HexEncode(digest);
  if (!seen.insert(key).second)
    return;
  if (markup) {
    title = Text(title);
  } else {
    // GitHub JSON titles are plain text: "<" is part of the title.
    title = base::UTF16ToUTF8(
        base::CollapseWhitespace(base::UTF8ToUTF16(title), true));
    title = std::string(base::TruncateUTF8ToByteSize(title, kMaxTitleLength));
  }
  if (title.empty())
    title = std::string(url.host());
  items.push_back({key, std::move(title), url.spec()});
}

bool GithubItemUrl(const GURL& url, uint32_t kind) {
  if (!SafeUrl(url) || !url.SchemeIs("https") || url.host() != "github.com")
    return false;
  const std::string_view part =
      kind == KR_LIVE_FOLDER_GITHUB_PULLS ? "/pull/" : "/issues/";
  const auto at = url.path().rfind(part);
  if (at == std::string_view::npos || at == 0)
    return false;
  const auto number = url.path().substr(at + part.size());
  return !number.empty() && std::ranges::all_of(number, [](char c) {
    return base::IsAsciiDigit(c);
  });
}

// GitHub's React routes use one payload shape in JSON responses and in a
// page's embedded data: payload.<name>ContentRoute.results[]. An unknown or
// ambiguous route keeps the cache instead of being read as an empty list.
const base::ListValue* GithubRows(const base::DictValue& root, uint32_t kind) {
  const auto* payload = root.FindDict("payload");
  if (!payload)
    return nullptr;
  if (kind == KR_LIVE_FOLDER_GITHUB_PULLS) {
    if (const auto* route =
            payload->FindDict("pullsDashboardSurfaceContentRoute")) {
      if (const auto* rows = route->FindList("results")) {
        if (rows->empty() &&
            route->FindIntByDottedPath("pageInfo.totalCount").value_or(0) > 0)
          return nullptr;
        return rows;
      }
    }
  }
  const base::ListValue* found = nullptr;
  for (const auto [name, value] : *payload) {
    const auto* route = value.GetIfDict();
    const auto* rows = route && name.ends_with("ContentRoute")
                           ? route->FindList("results")
                           : nullptr;
    if (!rows)
      continue;
    // An empty first page with a positive total is incomplete data.
    if (found ||
        (rows->empty() &&
         route->FindIntByDottedPath("pageInfo.totalCount").value_or(0) > 0))
      return nullptr;
    found = rows;
  }
  return found;
}

std::optional<LiveFolderItems> GithubJson(std::string_view body,
                                          const LiveFolderConfig& config) {
  auto value = base::JSONReader::Read(body, base::JSON_PARSE_RFC);
  if (!value || !value->is_dict())
    return std::nullopt;
  const auto* rows = GithubRows(value->GetDict(), config.kind);
  if (!rows)
    return std::nullopt;
  LiveFolderItems items;
  std::set<std::string> seen;
  for (const auto& value_row : *rows) {
    const auto* row = value_row.GetIfDict();
    if (!row)
      return std::nullopt;
    // Issue routes wrap each item; the pull request dashboard is flat.
    if (const auto* item = row->FindDict(
            config.kind == KR_LIVE_FOLDER_GITHUB_PULLS ? "pullRequest"
                                                       : "issue"))
      row = item;
    const auto* link = row->FindString("permalink");
    if (!link)
      link = row->FindString("url");
    const auto* title = row->FindString("title");
    if (!link || !title)
      return std::nullopt;
    const GURL url = GURL("https://github.com").Resolve(*link);
    if (!GithubItemUrl(url, config.kind))
      return std::nullopt;
    AddItem(items, seen, config, url.path(), *title, url, /*markup=*/false);
  }
  if (!rows->empty() && items.empty())
    return std::nullopt;
  return items;
}
}  // namespace

bool ValidLiveFolderConfig(const LiveFolderConfig& config) {
  if (config.kind == KR_LIVE_FOLDER_NONE)
    return config.source.empty();
  if (config.kind > KR_LIVE_FOLDER_GITHUB_ISSUES || config.source.empty() ||
      config.source.size() > kMaxSourceLength ||
      !base::IsStringUTF8(config.source) ||
      config.source.find_first_of("\r\n\0", 0, 3) != std::string::npos ||
      config.interval_minutes < 15 || config.interval_minutes > 480 ||
      !config.max_items || config.max_items > kMaxLiveItems)
    return false;
  return config.kind != KR_LIVE_FOLDER_RSS || SafeUrl(GURL(config.source));
}

GURL LiveFolderRequestUrl(const LiveFolderConfig& config) {
  if (config.kind == KR_LIVE_FOLDER_RSS)
    return GURL(config.source);
  return net::AppendQueryParameter(
      GURL(config.kind == KR_LIVE_FOLDER_GITHUB_PULLS
               ? "https://github.com/pulls"
               : "https://github.com/issues"),
      "q",
      std::string(config.kind == KR_LIVE_FOLDER_GITHUB_PULLS ? "is:pr "
                                                             : "is:issue ") +
          "is:open archived:false sort:updated-desc " + config.source);
}

LiveFolderConfig ReadLiveFolderConfig(const base::DictValue& row) {
  LiveFolderConfig config;
  if (const auto* live = row.FindDict("live")) {
    config.kind = live->FindInt("kind").value_or(0);
    if (const auto* source = live->FindString("source"))
      config.source = *source;
    config.interval_minutes = live->FindInt("interval_minutes").value_or(30);
    config.max_items = live->FindInt("max_items").value_or(20);
  }
  return ValidLiveFolderConfig(config) ? config : LiveFolderConfig();
}

void WriteLiveFolderConfig(base::DictValue& row,
                           const LiveFolderConfig& config) {
  if (!config.enabled())
    return;
  base::DictValue live;
  live.Set("kind", static_cast<int>(config.kind));
  live.Set("source", config.source);
  live.Set("interval_minutes", static_cast<int>(config.interval_minutes));
  live.Set("max_items", static_cast<int>(config.max_items));
  row.Set("live", std::move(live));
}

LiveFolderResult ParseLiveFeed(base::Value tree,
                               GURL base_url,
                               LiveFolderConfig config) {
  const auto name = LocalName(tree);
  const bool atom = name == "feed";
  const auto* root = name == "rss" ? Child(tree, "channel") : &tree;
  if (!root || (!atom && name != "rss" && name != "RDF"))
    return base::unexpected("The address did not return an RSS or Atom feed.");
  if (const auto xml_base =
          data_decoder::GetXmlElementAttribute(tree, "xml:base");
      !xml_base.empty())
    base_url = base_url.Resolve(xml_base);
  LiveFolderItems items;
  std::set<std::string> seen;
  size_t entries = 0;
  if (const auto* children = data_decoder::GetXmlElementChildren(*root)) {
    for (const auto& entry : *children) {
      if (LocalName(entry) != (atom ? "entry" : "item"))
        continue;
      ++entries;
      std::string link;
      if (atom) {
        if (const auto* parts = data_decoder::GetXmlElementChildren(entry)) {
          for (const auto& part : *parts) {
            if (LocalName(part) != "link")
              continue;
            const auto rel = data_decoder::GetXmlElementAttribute(part, "rel");
            if (rel.empty() || rel == "alternate") {
              link = data_decoder::GetXmlElementAttribute(part, "href");
              if (!link.empty())
                break;
            }
          }
        }
      } else {
        link = NodeText(Child(entry, "link"));
      }
      base::TrimWhitespaceASCII(link, base::TRIM_ALL, &link);
      if (link.empty())
        continue;
      const auto url = base_url.Resolve(link);
      auto id = NodeText(Child(entry, atom ? "id" : "guid"));
      AddItem(items, seen, config, id.empty() ? url.spec() : id,
              NodeText(Child(entry, "title")), url);
    }
  }
  if (entries && items.empty())
    return base::unexpected(
        "The feed contained no readable links. Cached items are kept.");
  return items;
}

LiveFolderResult ParseLiveGithub(std::string body, LiveFolderConfig config) {
  if (auto items = GithubJson(body, config))
    return std::move(*items);
  const std::string_view html(body);
  if (GithubSignedOut(html))
    return base::unexpected(kLiveFolderGithubSignedOut);
  LiveFolderItems items;
  std::set<std::string> seen;
  bool recognized = false;
  size_t at = 0;
  while (const auto tag = NextTag(html, at)) {
    // React's embedded route data uses the same checked schema as JSON
    // responses and does not depend on server-rendered list markup.
    if (base::EqualsCaseInsensitiveASCII(tag->name, "script")) {
      if (Attribute(tag->text, "type") == "application/json") {
        if (auto parsed = GithubJson(tag->raw, config))
          return std::move(*parsed);
      }
      continue;
    }
    if (!base::EqualsCaseInsensitiveASCII(tag->name, "a"))
      continue;
    const auto id = Attribute(tag->text, "id");
    if (!id.starts_with("issue_") &&
        Attribute(tag->text, "data-testid") != "issue-listitem-title-link")
      continue;
    const auto close = html.find("</a>", at);
    if (close == std::string_view::npos)
      return base::unexpected("GitHub returned an incomplete response.");
    const auto url =
        GURL("https://github.com").Resolve(Attribute(tag->text, "href"));
    if (!GithubItemUrl(url, config.kind))
      return base::unexpected("GitHub returned an unexpected item.");
    recognized = true;
    AddItem(items, seen, config, url.path(),
            std::string(html.substr(at, close - at)), url);
    at = close + 4;
  }
  // Never interpret a login page, changed markup or server error as an empty
  // result. Unknown empty HTML is deliberately conservative; JSON [] is clear.
  if (!recognized)
    return base::unexpected(
        "Could not read GitHub's list. Cached items are kept.");
  return items;
}

LiveFolderResult ParseLiveGithubError(int status, std::string body) {
  // Signed-out dashboards answer 404: JSON states the authentication failure
  // and HTML carries an empty user-login. Other errors keep the cache.
  bool signed_out = GithubSignedOut(body);
  if (!signed_out && body.starts_with('{')) {
    if (const auto value = base::JSONReader::Read(body, base::JSON_PARSE_RFC);
        value && value->is_dict()) {
      const auto* error = value->GetDict().FindString("error");
      signed_out = error && error->find("authenticate") != std::string::npos;
    }
  }
  if (signed_out)
    return base::unexpected(kLiveFolderGithubSignedOut);
  return base::unexpected("GitHub returned HTTP " +
                          base::NumberToString(status) +
                          ". Cached items are kept.");
}
}  // namespace krypton_ui
