#include "shell/desktop/widgets/desktop_remote_source.h"

#include "core/process/process.h"
#include "i18n/i18n.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <libxml/parser.h>
#include <nlohmann/json.hpp>
#include <optional>

namespace {
  std::string content(xmlNode* node) {
    xmlChar* text = xmlNodeGetContent(node);
    if (!text)
      return {};
    std::string result(reinterpret_cast<const char*>(text));
    xmlFree(text);
    return StringUtils::trim(result);
  }
  bool named(xmlNode* node, const char* name) {
    return node && node->type == XML_ELEMENT_NODE && xmlStrEqual(node->name, BAD_CAST name);
  }
  std::string property(xmlNode* node, const char* key) {
    xmlChar* value = xmlGetProp(node, BAD_CAST key);
    if (!value)
      return {};
    std::string result(reinterpret_cast<const char*>(value));
    xmlFree(value);
    return result;
  }
  std::string stringValue(const nlohmann::json& object, const char* key) {
    if (!object.is_object())
      return {};
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
  }
  bool toggleEntity(std::string_view id) {
    return id.starts_with("light.") || id.starts_with("switch.") || id.starts_with("input_boolean.");
  }
  bool responseOk(const HttpResponse& response) {
    return response.transportOk
        && response.status >= 200
        && response.status < 300
        && response.body.size() <= 2 * 1024 * 1024;
  }
} // namespace

namespace desktop_sources {
  bool validWebUrl(std::string_view url) {
    const auto prefix = url.starts_with("https://") ? 8U : (url.starts_with("http://") ? 7U : 0U);
    if (prefix == 0 || url.size() <= prefix || std::ranges::any_of(url, [](unsigned char c) {
          return c <= 32 || c == 127;
        }))
      return false;
    return url[prefix] != '/' && url[prefix] != '?' && url[prefix] != '#';
  }

  std::vector<DesktopCardItem> parseFeed(std::string_view xml, bool podcasts) {
    std::vector<DesktopCardItem> result;
    if (xml.empty() || xml.size() > 2 * 1024 * 1024)
      return result;
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(
        xmlReadMemory(
            xml.data(), static_cast<int>(xml.size()), nullptr, nullptr,
            XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING
        ),
        &xmlFreeDoc
    );
    if (!doc || doc->intSubset || doc->extSubset)
      return result;
    auto* root = xmlDocGetRootElement(doc.get());
    if (!root)
      return result;
    xmlNode* container = root;
    if (named(root, "rss")) {
      for (auto* child = root->children; child; child = child->next)
        if (named(child, "channel")) {
          container = child;
          break;
        }
    }
    for (auto* entry = container->children; entry && result.size() < 128; entry = entry->next) {
      if (!named(entry, "item") && !named(entry, "entry"))
        continue;
      DesktopCardItem item;
      std::string enclosure;
      for (auto* child = entry->children; child; child = child->next) {
        if (named(child, "title"))
          item.title = content(child);
        else if (named(child, "pubDate") || named(child, "published") || named(child, "updated"))
          item.detail = content(child);
        else if (named(child, "enclosure"))
          enclosure = property(child, "url");
        else if (named(child, "link")) {
          const auto rel = property(child, "rel");
          const auto href = property(child, "href");
          if (rel == "enclosure")
            enclosure = href;
          else if (rel.empty() || rel == "alternate")
            item.action = href.empty() ? content(child) : href;
        }
      }
      if (podcasts && validWebUrl(enclosure))
        item.action = enclosure;
      if (!item.title.empty() && validWebUrl(item.action))
        result.push_back(std::move(item));
    }
    return result;
  }

  std::vector<DesktopCardItem>
  parseHomeStates(std::string_view json, const std::vector<std::string>& entities, bool locations) {
    std::vector<DesktopCardItem> result;
    const auto data = nlohmann::json::parse(json, nullptr, false);
    if (!data.is_array())
      return result;
    for (const auto& id : entities) {
      if (locations && !id.starts_with("person.") && !id.starts_with("device_tracker."))
        continue;
      for (const auto& state : data) {
        if (stringValue(state, "entity_id") != id)
          continue;
        const auto status = stringValue(state, "state");
        const auto attributes = state.contains("attributes") && state["attributes"].is_object()
            ? state["attributes"]
            : nlohmann::json::object();
        DesktopCardItem item{stringValue(attributes, "friendly_name"), status, id};
        if (item.title.empty())
          item.title = id;
        const auto updated = stringValue(state, "last_updated");
        if (!updated.empty())
          item.detail += " · " + updated.substr(0, 16);
        item.checked = status == "on";
        item.enabled = status != "unavailable" && status != "unknown" && !status.empty();
        if (locations) {
          item.enabled = false;
          if (attributes.contains("latitude")
              && attributes["latitude"].is_number()
              && attributes.contains("longitude")
              && attributes["longitude"].is_number()) {
            const double lat = attributes["latitude"].get<double>(), lon = attributes["longitude"].get<double>();
            if (std::isfinite(lat) && std::isfinite(lon) && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180) {
              item.action = std::format(
                  "https://www.openstreetmap.org/?mlat={:.6f}&mlon={:.6f}#map=16/{:.6f}/{:.6f}", lat, lon, lat, lon
              );
              item.enabled = !status.empty() && status != "unavailable" && status != "unknown";
            }
          }
        } else
          item.enabled = item.enabled && toggleEntity(id);
        result.push_back(std::move(item));
        break;
      }
    }
    return result;
  }

  std::vector<DesktopCardItem> parseQuote(std::string_view json) {
    const auto data = nlohmann::json::parse(json, nullptr, false);
    if (!data.is_object() || !data.contains("Global Quote"))
      return {};
    const auto& quote = data["Global Quote"];
    const auto symbol = stringValue(quote, "01. symbol"), price = stringValue(quote, "05. price");
    if (symbol.empty() || price.empty())
      return {};
    return {
        {symbol + "  " + price,
         stringValue(quote, "10. change percent") + " · " + stringValue(quote, "07. latest trading day"),
         "https://www.alphavantage.co/", false, true}
    };
  }
} // namespace desktop_sources

DesktopRemoteSource::DesktopRemoteSource(
    std::string kind, Settings settings, HttpClient* http, std::function<void()> changed
)
    : m_kind(std::move(kind)), m_settings(std::move(settings)), m_http(http), m_changed(std::move(changed)) {}
DesktopRemoteSource::~DesktopRemoteSource() { m_alive.reset(); }
std::string DesktopRemoteSource::setting(const char* key) const {
  if (auto it = m_settings.find(key); it != m_settings.end())
    if (const auto* text = std::get_if<std::string>(&it->second))
      return *text;
  return {};
}
std::vector<std::string> DesktopRemoteSource::list(const char* key) const {
  if (auto it = m_settings.find(key); it != m_settings.end())
    if (const auto* value = std::get_if<std::vector<std::string>>(&it->second))
      return *value;
  return {};
}
std::string DesktopRemoteSource::token() const {
  const auto path = setting("token_file");
  if (path.empty())
    return {};
  std::ifstream input(FileUtils::expandXdgBaseDir(path), std::ios::binary);
  std::string result(4097, '\0');
  input.read(result.data(), static_cast<std::streamsize>(result.size()));
  result.resize(static_cast<std::size_t>(input.gcount()));
  if (result.size() > 4096)
    return {};
  result = StringUtils::trim(result);
  return result.find_first_of("\r\n") == std::string::npos ? result : std::string();
}
void DesktopRemoteSource::request(HttpRequest request, std::function<void(HttpResponse)> callback) {
  if (!m_http)
    return;
  std::weak_ptr<int> alive = m_alive;
  m_http->request(std::move(request), [alive, callback = std::move(callback)](HttpResponse response) {
    if (!alive.expired())
      callback(std::move(response));
  });
}
void DesktopRemoteSource::start() {
  int minutes = m_kind == "stocks" ? 360 : (m_kind == "home" || m_kind == "find_my" ? 1 : 15);
  if (auto it = m_settings.find("refresh_minutes"); it != m_settings.end())
    if (const auto* value = std::get_if<std::int64_t>(&it->second))
      minutes = static_cast<int>(std::clamp<std::int64_t>(*value, 1, 1440));
  fetch();
  m_timer.startRepeating(std::chrono::minutes(minutes), [this] { fetch(); });
}

void DesktopRemoteSource::fetch() {
  if (m_pending)
    return;
  if (!m_http) {
    m_status = i18n::tr("desktop-widgets.cards.source-unavailable");
    return;
  }
  if (m_kind == "stocks") {
    if (token().empty() || list("symbols").empty()) {
      m_status = i18n::tr("desktop-widgets.cards.configure-stocks");
      return;
    }
    m_pending = true;
    m_quotes.clear();
    fetchQuote(0);
    return;
  }
  const bool home = m_kind == "home" || m_kind == "find_my";
  std::string url = setting(home ? "server_url" : "feed_url");
  if (!desktop_sources::validWebUrl(url) || (home && (token().empty() || list("entities").empty()))) {
    m_status = i18n::tr(home ? "desktop-widgets.cards.configure-home" : "desktop-widgets.cards.configure-feed");
    return;
  }
  HttpRequest req;
  if (home) {
    while (url.ends_with('/'))
      url.pop_back();
    url += "/api/states";
    req.headers = {"Authorization: Bearer " + token()};
  } else
    req.followRedirects = true;
  req.url = url;
  m_pending = true;
  m_status = i18n::tr("desktop-widgets.cards.refreshing");
  request(std::move(req), [this, home](HttpResponse response) {
    m_pending = false;
    if (!responseOk(response)) {
      m_status = i18n::tr("desktop-widgets.cards.refresh-failed");
      for (auto& item : m_items)
        item.enabled = false;
    } else {
      m_items = home ? desktop_sources::parseHomeStates(response.body, list("entities"), m_kind == "find_my")
                     : desktop_sources::parseFeed(response.body, m_kind == "podcasts");
      m_status = m_items.empty() ? i18n::tr("desktop-widgets.cards.source-empty")
                                 : (home ? "Home Assistant" : i18n::tr("desktop-widgets.cards.feed"));
    }
    m_changed();
  });
}

void DesktopRemoteSource::fetchQuote(std::size_t index) {
  const auto symbols = list("symbols");
  if (index >= std::min<std::size_t>(3, symbols.size())) {
    m_pending = false;
    const bool complete = m_quotes.size() == std::min<std::size_t>(3, symbols.size());
    if (!m_quotes.empty())
      m_items = std::move(m_quotes);
    m_status = complete ? "Alpha Vantage" : i18n::tr("desktop-widgets.cards.refresh-failed");
    m_changed();
    return;
  }
  HttpRequest req;
  req.url = "https://www.alphavantage.co/query?function=GLOBAL_QUOTE&symbol="
      + StringUtils::urlEncode(symbols[index])
      + "&apikey="
      + StringUtils::urlEncode(token());
  request(std::move(req), [this, index](HttpResponse response) {
    if (responseOk(response)) {
      auto quotes = desktop_sources::parseQuote(response.body);
      m_quotes.insert(m_quotes.end(), quotes.begin(), quotes.end());
    }
    fetchQuote(index + 1);
  });
}

void DesktopRemoteSource::activate(const DesktopCardItem& item) {
  if (!item.enabled || m_pending)
    return;
  if (m_kind != "home") {
    if (desktop_sources::validWebUrl(item.action))
      (void)process::runAsync(std::vector<std::string>{"xdg-open", item.action});
    return;
  }
  if (!toggleEntity(item.action) || token().empty())
    return;
  std::string url = setting("server_url");
  if (!desktop_sources::validWebUrl(url))
    return;
  while (url.ends_with('/'))
    url.pop_back();
  HttpRequest req;
  req.method = "POST";
  req.url = url + "/api/services/homeassistant/toggle";
  req.headers = {"Authorization: Bearer " + token(), "Content-Type: application/json"};
  req.body = nlohmann::json{{"entity_id", item.action}}.dump();
  m_pending = true;
  request(std::move(req), [this](HttpResponse response) {
    m_pending = false;
    if (responseOk(response))
      fetch();
    else
      m_status = i18n::tr("desktop-widgets.cards.action-failed");
    m_changed();
  });
}
