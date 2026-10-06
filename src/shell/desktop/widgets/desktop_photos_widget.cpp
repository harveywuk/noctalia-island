#include "shell/desktop/widgets/desktop_photos_widget.h"

#include "core/process/process.h"
#include "i18n/i18n.h"
#include "ui/builders.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <filesystem>

DesktopPhotosWidget::DesktopPhotosWidget(
    std::string file, std::string folder, int intervalMinutes, desktop_cards::Size size
)
    : m_file(std::move(file)), m_folder(std::move(folder)), m_intervalMinutes(std::clamp(intervalMinutes, 1, 1440)),
      m_size(size) {}

void DesktopPhotosWidget::create() {
  auto node = ui::node({});
  node->addChild(ui::image({.out = &m_image, .fit = ImageFit::Cover}));
  node->addChild(
      ui::label({.out = &m_caption, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 1})
  );
  node->addChild(
      ui::label(
          {.out = &m_empty,
           .text = i18n::tr("desktop-widgets.cards.photos-empty"),
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxLines = 4}
      )
  );
  node->addChild(
      ui::button({.out = &m_previous, .glyph = "chevron-left", .variant = ButtonVariant::Ghost, .onClick = [this] {
                    step(-1);
                  }})
  );
  node->addChild(
      ui::button({.out = &m_next, .glyph = "chevron-right", .variant = ButtonVariant::Ghost, .onClick = [this] {
                    step(1);
                  }})
  );
  node->addChild(ui::button({.out = &m_open, .glyph = "photo", .variant = ButtonVariant::Ghost, .onClick = [this] {
                               if (!m_loaded.empty())
                                 (void)process::runAsync(std::vector<std::string>{"xdg-open", m_loaded});
                             }}));
  setRoot(std::move(node));
}

void DesktopPhotosWidget::step(int direction) {
  if (m_files.size() < 2)
    return;
  m_index = direction < 0 ? (m_index + m_files.size() - 1) % m_files.size() : (m_index + 1) % m_files.size();
  requestUpdate();
}

void DesktopPhotosWidget::sync(Renderer& renderer) {
  const auto now = std::chrono::steady_clock::now();
  const bool rescan =
      m_lastScan == std::chrono::steady_clock::time_point{} || now - m_lastScan >= std::chrono::minutes(1);
  if (rescan) {
    m_lastScan = now;
    std::vector<std::string> files;
    std::error_code ec;
    if (!m_file.empty()) {
      const std::filesystem::path path = FileUtils::expandXdgBaseDir(m_file);
      if (std::filesystem::is_regular_file(path, ec))
        files.push_back(path.string());
    } else if (!m_folder.empty()) {
      const auto folder = FileUtils::expandXdgBaseDir(m_folder);
      std::filesystem::directory_iterator it(folder, ec), end;
      std::size_t visited = 0;
      for (; !ec && it != end && files.size() < 500 && visited < 4096; it.increment(ec), ++visited) {
        if (!it->is_regular_file(ec))
          continue;
        const auto extension = StringUtils::toLower(it->path().extension().string());
        if (extension == ".jpg"
            || extension == ".jpeg"
            || extension == ".png"
            || extension == ".webp"
            || extension == ".avif"
            || extension == ".jxl")
          files.push_back(it->path().string());
      }
      std::ranges::sort(files);
    }
    if (m_files != files) {
      m_files = std::move(files);
      m_index = m_files.empty() ? 0 : std::min(m_index, m_files.size() - 1);
    }
    if (m_files.size() > 1 && !m_timer.active())
      m_timer.startRepeating(std::chrono::minutes(m_intervalMinutes), [this] { step(1); });
    if (m_files.size() < 2)
      m_timer.stop();
  }
  const std::string path = m_files.empty() ? std::string() : m_files[m_index];
  auto modified = m_modified;
  if (rescan || path != m_loaded) {
    std::error_code ec;
    modified = path.empty() ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(path, ec);
    if (ec)
      modified = {};
  }
  if (path != m_loaded || modified != m_modified) {
    m_loaded = path;
    m_modified = modified;
    m_image->clear(renderer);
    if (!path.empty())
      (void)m_image->setSourceFile(renderer, path, static_cast<int>(432 * contentScale()), true);
    if (!isLayingOut())
      requestLayout();
  }
}

void DesktopPhotosWidget::doUpdate(Renderer& renderer) { sync(renderer); }

void DesktopPhotosWidget::doLayout(Renderer& renderer) {
  sync(renderer);
  const auto card =
      desktop_cards::resolve(m_size, contentScale(), boxInnerWidth(), boxInnerHeight(), backgroundPadding());
  const float scale = card.scale;
  m_image->setSize(card.width, std::max(1.0F, card.height - 34 * scale));
  m_image->setRadius(Style::scaledRadiusLg(scale));
  m_empty->setVisible(!m_image->hasImage());
  m_empty->setFontSize(Style::fontSizeBody * scale);
  m_empty->setMinWidth(card.width);
  m_empty->setMaxWidth(card.width);
  m_empty->measure(renderer);
  m_empty->setPosition(0, std::max(0.0F, (m_image->height() - m_empty->height()) * 0.5F));
  m_caption->setFontFamily(m_fontFamily);
  m_empty->setFontFamily(m_fontFamily);
  m_caption->setText(
      m_loaded.empty() ? i18n::tr("desktop-widgets.editor.types.photos")
                       : std::filesystem::path(m_loaded).stem().string()
  );
  m_caption->setFontSize(Style::fontSizeCaption * scale);
  m_caption->setMaxWidth(std::max(1.0F, card.width - 90 * scale));
  m_caption->measure(renderer);
  m_caption->setPosition(Style::rtl() ? card.width - m_caption->width() : 0, card.height - 24 * scale);
  std::size_t i = 0;
  for (auto* button : {m_previous, m_open, m_next}) {
    button->setMinWidth(28 * scale);
    button->setMinHeight(28 * scale);
    button->setPadding(2 * scale, 2 * scale);
    button->layout(renderer);
    const float x = card.width - static_cast<float>(3 - i) * 28 * scale;
    button->setPosition(Style::rtl() ? card.width - x - button->width() : x, card.height - 28 * scale);
    button->updateInputArea();
    ++i;
  }
  m_previous->setEnabled(m_files.size() > 1);
  m_next->setEnabled(m_files.size() > 1);
  m_open->setEnabled(m_image->hasImage());
  root()->setSize(card.width, card.height);
}
