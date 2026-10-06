#include "shell/desktop/widgets/desktop_collection_widget.h"

#include "config/config_service.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "launcher/notes_provider.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "shell/desktop/desktop_widget_setup.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>

namespace {
  bool webUrl(std::string_view url) { return url.starts_with("https://") || url.starts_with("http://"); }
} // namespace

DesktopCollectionWidget::DesktopCollectionWidget(
    std::string kind, Settings settings, DesktopWidgetRuntimeServices services
)
    : m_kind(std::move(kind)), m_settings(std::move(settings)), m_services(services),
      m_size(desktop_cards::sizeFromSetting(setting("card_size", "medium"))) {
  if (m_kind == "notes" || m_kind == "journal") {
    const auto configured = setting("file_path");
    if (!configured.empty())
      m_file = FileUtils::expandXdgBaseDir(configured);
    else if (m_kind == "notes")
      m_file = NotesProvider(nullptr, services.scriptDeps.configService).file();
    else
      m_file = std::filesystem::path(FileUtils::dataDir()) / "Journal.md";
  }
}

DesktopCollectionWidget::~DesktopCollectionWidget() {
  if (m_watch && m_services.scriptDeps.fileWatcher)
    m_services.scriptDeps.fileWatcher->unwatch(m_watch);
}

std::string DesktopCollectionWidget::setting(const char* key, std::string fallback) const {
  if (auto it = m_settings.find(key); it != m_settings.end())
    if (const auto* value = std::get_if<std::string>(&it->second))
      return *value;
  return fallback;
}

void DesktopCollectionWidget::create() {
  auto node = ui::node({});
  node->addChild(
      ui::label(
          {.out = &m_heading,
           .fontWeight = FontWeight::Bold,
           .color = colorSpecFromRole(ColorRole::Primary),
           .maxLines = 1}
      )
  );
  node->addChild(
      ui::label({.out = &m_subtitle, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 1})
  );
  node->addChild(ui::label({.out = &m_empty, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 5}));
  for (std::size_t i = 0; i < m_rows.size(); ++i) {
    auto& row = m_rows[i];
    node->addChild(ui::button({.out = &row.action, .variant = ButtonVariant::Ghost, .onClick = [this, i] {
                                 activate(i);
                               }}));
    node->addChild(
        ui::label({.out = &row.detail, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 1})
    );
    node->addChild(ui::checkbox({.out = &row.check, .onChange = [this, i](bool checked) { toggle(i, checked); }}));
  }
  node->addChild(
      ui::button({.out = &m_previous, .glyph = "chevron-left", .variant = ButtonVariant::Ghost, .onClick = [this] {
                    if (m_page > 0) {
                      --m_page;
                      requestLayout();
                    }
                  }})
  );
  node->addChild(
      ui::button({.out = &m_next, .glyph = "chevron-right", .variant = ButtonVariant::Ghost, .onClick = [this] {
                    if ((m_page + 1) * m_pageSize < m_items.size()) {
                      ++m_page;
                      requestLayout();
                    }
                  }})
  );
  node->addChild(
      ui::label({.out = &m_pageLabel, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 1})
  );
  node->addChild(ui::button({.out = &m_open, .glyph = "edit", .variant = ButtonVariant::Ghost, .onClick = [this] {
                               if (!m_file.empty()) {
                                 auto& panels = PanelManager::instance();
                                 panels.closePanelById("floating-notes");
                                 const auto context = (m_kind == "journal" ? "journal:" : "file:") + m_file.string();
                                 panels.openPanel("floating-notes", PanelOpenRequest{.context = context});
                               }
                             }}));
  node->addChild(
      ui::button(
          {.out = &m_configure,
           .text = i18n::tr("desktop-widgets.setup.configure"),
           .variant = ButtonVariant::Secondary,
           .onClick = [this]() { requestConfigure(); }}
      )
  );
  setRoot(std::move(node));
  if (m_kind == "news" || m_kind == "podcasts" || m_kind == "stocks" || m_kind == "home" || m_kind == "find_my") {
    m_remote =
        std::make_unique<DesktopRemoteSource>(m_kind, m_settings, m_services.httpClient, [this] { requestLayout(); });
    m_remote->start();
  }
  if (!m_file.empty() && m_services.scriptDeps.fileWatcher)
    m_watch = m_services.scriptDeps.fileWatcher->watch(
        m_file,
        [this] {
          // Read the completed save, not the transient empty file after truncation.
          m_fileLoaded = false;
          requestUpdate();
        },
        FileWatcher::WatchTrigger::WriteCompleted
    );
  refresh();
}

bool DesktopCollectionWidget::refresh() {
  std::vector<Item> items;
  if (m_remote) {
    items = m_remote->items();
  } else if (m_kind == "video_library") {
    const auto now = std::chrono::steady_clock::now();
    if (m_lastDirectoryRead != std::chrono::steady_clock::time_point{}
        && now - m_lastDirectoryRead < std::chrono::minutes(1))
      return false;
    m_lastDirectoryRead = now;
    const auto folder = setting("folder_path");
    if (!folder.empty()) {
      std::error_code ec;
      std::filesystem::directory_iterator it(FileUtils::expandXdgBaseDir(folder), ec), end;
      std::size_t visited = 0;
      for (; !ec && it != end && items.size() < 128 && visited < 4096; it.increment(ec), ++visited) {
        if (!it->is_regular_file(ec))
          continue;
        const auto extension = StringUtils::toLower(it->path().extension().string());
        if (extension == ".mp4"
            || extension == ".mkv"
            || extension == ".webm"
            || extension == ".mov"
            || extension == ".avi")
          items.push_back({it->path().stem().string(), extension.substr(1), it->path().string()});
      }
      std::ranges::sort(items, {}, &Item::title);
    }
  } else if (m_kind == "notes" || m_kind == "journal") {
    if (!m_file.empty()) {
      std::error_code ec;
      const auto timestamp = std::filesystem::last_write_time(m_file, ec);
      if (!ec && m_fileLoaded && timestamp == m_fileTime)
        return false;
      m_fileLoaded = !ec;
      m_fileTime = timestamp;
      const auto bytes = std::filesystem::file_size(m_file, ec);
      if (ec || bytes > 1024 * 1024) {
        const bool changed = !m_items.empty();
        m_items.clear();
        return changed;
      }
      for (const auto& note : NotesProvider::read(m_file)) {
        items.push_back({note.text, note.stamp, m_file.string()});
        if (items.size() >= 128)
          break;
      }
      // Floating Notes also accepts ordinary text. Show its non-empty lines when
      // the file does not contain the launcher's timestamped bullet entries.
      if (items.empty() && m_kind == "notes") {
        std::ifstream input(m_file);
        std::string line;
        while (items.size() < 128 && std::getline(input, line)) {
          line = StringUtils::trim(line);
          if (!line.empty())
            items.push_back({line, {}, m_file.string()});
        }
      }
    }
  } else if (m_kind == "reminders") {
    nlohmann::json done = nlohmann::json::object();
    if (auto* config = m_services.scriptDeps.configService) {
      auto stored = config->stateString("desktop_cards", "reminders");
      if (stored) {
        auto parsed = nlohmann::json::parse(*stored, nullptr, false);
        if (parsed.is_object())
          done = std::move(parsed);
      }
    }
    const auto list = setting("list_name", "Reminders");
    if (auto it = m_settings.find("items"); it != m_settings.end()) {
      if (const auto* tasks = std::get_if<std::vector<std::string>>(&it->second)) {
        std::set<std::string> seen;
        for (const auto& text : *tasks) {
          if (text.empty() || !seen.insert(text).second)
            continue;
          const bool checked = done.contains(list)
              && done[list].is_object()
              && done[list].contains(text)
              && done[list][text].is_boolean()
              && done[list][text].get<bool>();
          items.push_back({text, {}, {}, checked});
        }
      }
    }
  } else if (m_kind == "tips") {
    items.push_back(
        {i18n::tr("desktop-widgets.cards.tip-gallery"), i18n::tr("desktop-widgets.cards.tip-gallery-detail"),
         "https://docs.noctalia.dev/noctalia/desktop/widgets/"}
    );
    items.push_back(
        {i18n::tr("desktop-widgets.cards.tip-shortcuts"), i18n::tr("desktop-widgets.cards.tip-shortcuts-detail"),
         "https://docs.noctalia.dev/noctalia/"}
    );
    items.push_back(
        {i18n::tr("desktop-widgets.cards.tip-theme"), i18n::tr("desktop-widgets.cards.tip-theme-detail"),
         "https://docs.noctalia.dev/noctalia/"}
    );
  } else if (auto it = m_settings.find("entries"); it != m_settings.end()) {
    if (const auto* entries = std::get_if<WidgetSettingStringMap>(&it->second)) {
      for (const auto& [title, action] : *entries) {
        if (title.empty() || action.empty())
          continue;
        if (m_kind == "reading_list" && !webUrl(action))
          continue;
        if (m_kind == "contacts" && !action.starts_with("mailto:") && !action.starts_with("tel:"))
          continue;
        items.push_back({title, m_kind == "shortcuts" ? std::string() : action, action});
      }
      std::ranges::sort(items, {}, &Item::title);
    }
  }
  if (m_items == items)
    return false;
  m_items = std::move(items);
  return true;
}

void DesktopCollectionWidget::activate(std::size_t row) {
  const auto index = m_page * m_pageSize + row;
  if (index >= m_items.size())
    return;
  const auto& item = m_items[index];
  if (m_remote) {
    m_remote->activate(item);
    return;
  }
  if (m_kind == "reminders") {
    toggle(row, !item.checked);
    return;
  }
  if (m_kind == "shortcuts")
    (void)process::runAsync(item.action);
  else if (!item.action.empty())
    (void)process::runAsync(std::vector<std::string>{"xdg-open", item.action});
}

void DesktopCollectionWidget::toggle(std::size_t row, bool checked) {
  const auto index = m_page * m_pageSize + row;
  auto* config = m_services.scriptDeps.configService;
  if (m_kind != "reminders" || index >= m_items.size() || !config)
    return;
  auto stored = config->stateString("desktop_cards", "reminders");
  auto data = stored ? nlohmann::json::parse(*stored, nullptr, false) : nlohmann::json::object();
  if (!data.is_object()) {
    m_saveError = true;
    requestLayout();
    return;
  }
  const auto list = setting("list_name", "Reminders");
  if (!data.contains(list))
    data[list] = nlohmann::json::object();
  if (!data[list].is_object()) {
    m_saveError = true;
    requestLayout();
    return;
  }
  data[list][m_items[index].title] = checked;
  m_saveError = !config->setStateString("desktop_cards", "reminders", data.dump());
  if (!m_saveError)
    m_items[index].checked = checked;
  requestLayout();
}

void DesktopCollectionWidget::doUpdate(Renderer&) {
  if (refresh() && !isLayingOut())
    requestLayout();
}

void DesktopCollectionWidget::doLayout(Renderer& renderer) {
  refresh();
  const auto card =
      desktop_cards::resolve(m_size, contentScale(), boxInnerWidth(), boxInnerHeight(), backgroundPadding());
  const float scale = card.scale;
  const bool small = card.size == desktop_cards::Size::Small;
  const bool notes = m_kind == "notes" || m_kind == "journal";
  const float rowHeight = (notes ? 55.0F : 42.0F) * scale;
  m_pageSize = std::clamp<std::size_t>(
      static_cast<std::size_t>(std::max(1.0F, (card.height - 78 * scale) / rowHeight)), 1, m_rows.size()
  );
  m_page = std::min(m_page, m_items.empty() ? std::size_t{0} : (m_items.size() - 1) / m_pageSize);
  auto place = [&](Node* node, float x, float y) {
    node->setPosition(Style::rtl() ? card.width - x - node->width() : x, y);
  };
  auto label = [&](Label* node, std::string text, float x, float y, float width, float font) {
    node->setText(std::move(text));
    node->setFontSize(font * scale);
    node->setMinWidth(width);
    node->setMaxWidth(width);
    node->measure(renderer);
    place(node, x, y);
  };
  label(
      m_heading,
      m_kind == "reminders" ? setting("list_name", "Reminders") : desktop_settings::desktopWidgetTypeLabel(m_kind), 0,
      0, card.width - (notes ? 32 * scale : 0), Style::fontSizeBody
  );
  const auto incomplete = std::ranges::count_if(m_items, [](const auto& item) { return !item.checked; });
  const auto subtitle = m_remote ? m_remote->status()
      : m_saveError              ? i18n::tr("desktop-widgets.cards.save-failed")
      : m_kind == "reminders" ? i18n::tr("desktop-widgets.cards.remaining", "count", std::to_string(incomplete))
                              : i18n::tr("desktop-widgets.cards.item-count", "count", std::to_string(m_items.size()));
  label(m_subtitle, subtitle, 0, 23 * scale, card.width, Style::fontSizeCaption);
  m_subtitle->setVisible(!m_remote || !m_items.empty());
  m_empty->setVisible(m_items.empty());
  label(
      m_empty,
      m_remote ? m_remote->status()
               : i18n::tr(
                     m_kind == "journal" ? "desktop-widgets.cards.journal-empty"
                         : notes         ? "desktop-widgets.cards.notes-empty"
                                         : "desktop-widgets.cards.collection-empty"
                 ),
      0, 66 * scale, card.width, Style::fontSizeBody
  );
  for (std::size_t i = 0; i < m_rows.size(); ++i) {
    auto& row = m_rows[i];
    const auto index = m_page * m_pageSize + i;
    const bool visible = i < m_pageSize && index < m_items.size();
    row.action->setVisible(visible);
    row.detail->setVisible(visible && !small && !m_items[index].detail.empty());
    row.check->setVisible(visible && m_kind == "reminders");
    if (!visible)
      continue;
    const auto& item = m_items[index];
    const float y = 49 * scale + static_cast<float>(i) * rowHeight;
    const float x = m_kind == "reminders" ? 30 * scale : 0;
    row.action->setText(item.title);
    row.action->label()->setFontFamily(m_fontFamily);
    row.action->setEnabled(item.enabled);
    row.action->setSelected(m_kind == "home" && item.checked);
    row.action->setContentAlign(ButtonContentAlign::Start);
    row.action->setFontSize(Style::fontSizeBody * scale);
    row.action->setMinWidth(card.width - x);
    row.action->setMaxWidth(card.width - x);
    row.action->setMinHeight(30 * scale);
    row.action->setPadding(0, 2 * scale);
    row.action->setOpacity(m_kind == "reminders" && item.checked ? 0.5F : 1.0F);
    row.action->layout(renderer);
    place(row.action, x, y);
    row.action->updateInputArea();
    label(row.detail, item.detail, x, y + 28 * scale, card.width - x, Style::fontSizeMini);
    row.check->setChecked(item.checked);
    row.check->setEnabled(m_services.scriptDeps.configService != nullptr);
    row.check->setScale(scale);
    row.check->layout(renderer);
    place(row.check, 0, y + 4 * scale);
  }
  const bool paginated = m_items.size() > m_pageSize;
  m_previous->setVisible(paginated);
  m_next->setVisible(paginated);
  m_pageLabel->setVisible(paginated);
  m_previous->setEnabled(m_page > 0);
  m_next->setEnabled((m_page + 1) * m_pageSize < m_items.size());
  for (auto* button : {m_previous, m_next, m_open}) {
    button->setMinWidth(28 * scale);
    button->setMinHeight(28 * scale);
    button->setPadding(2 * scale, 2 * scale);
    button->layout(renderer);
    button->updateInputArea();
  }
  place(m_previous, 0, card.height - 28 * scale);
  place(m_next, card.width - m_next->width(), card.height - 28 * scale);
  m_pageLabel->setTextAlign(TextAlign::Center);
  label(
      m_pageLabel,
      std::format("{} / {}", m_page + 1, std::max<std::size_t>(1, (m_items.size() + m_pageSize - 1) / m_pageSize)),
      32 * scale, card.height - 23 * scale, card.width - 64 * scale, Style::fontSizeMini
  );
  m_open->setVisible(notes);
  m_open->setEnabled(!m_file.empty());
  place(m_open, card.width - m_open->width(), -4 * scale);
  m_configure->setVisible(m_items.empty() && desktop_setup::guided(m_kind));
  m_configure->setEnabled(canConfigure());
  m_configure->setFontSize(Style::fontSizeCaption * scale);
  m_configure->layout(renderer);
  place(m_configure, 0, card.height - m_configure->height());
  m_configure->updateInputArea();
  root()->setSize(card.width, card.height);
}

void DesktopCollectionWidget::onFontFamilyChanged(const std::string& family, Renderer&) {
  for (auto* label : {m_heading, m_subtitle, m_empty, m_pageLabel})
    if (label)
      label->setFontFamily(family);
  for (auto& row : m_rows) {
    if (row.detail)
      row.detail->setFontFamily(family);
  }
}
