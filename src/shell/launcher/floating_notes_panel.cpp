#include "shell/launcher/floating_notes_panel.h"

#include "config/config_service.h"
#include "core/input/key_symbols.h"
#include "i18n/i18n.h"
#include "launcher/launcher_util.h"
#include "launcher/notes_provider.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <chrono>
#include <fstream>
#include <sstream>

namespace {

  constexpr std::string_view kPanelId = "floating-notes";
  constexpr auto kSaveDelay = std::chrono::milliseconds(600);

} // namespace

FloatingNotesPanel::FloatingNotesPanel(ConfigService* config) : m_config(config) {}

FloatingNotesPanel::~FloatingNotesPanel() = default;

std::filesystem::path FloatingNotesPanel::file() const { return NotesProvider(nullptr, m_config).file(); }

std::string FloatingNotesPanel::panelScreenPosition() const {
  if (m_config != nullptr && !m_config->config().shell.launcher.floatingNotesPosition.empty()) {
    return m_config->config().shell.launcher.floatingNotesPosition;
  }
  return "bottom_right";
}

InputArea* FloatingNotesPanel::initialFocusArea() const {
  return m_editor != nullptr ? m_editor->inputArea() : nullptr;
}

void FloatingNotesPanel::create() {
  const float scale = contentScale();
  auto container = ui::column({
      .out = &m_container,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });

  auto header = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .paddingH = Style::spaceXs * scale,
  });
  header->addChild(
      ui::glyph({
          .glyph = "note",
          .glyphSize = Style::baseGlyphSize * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  header->addChild(
      ui::label({
          .text = i18n::tr("launcher.floating-notes.title"),
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Medium,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .flexGrow = 1.0F,
      })
  );
  header->addChild(
      ui::button({
          .glyph = "external-link",
          .glyphSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("launcher.floating-notes.open-file"),
          .onClick = [this]() {
            save();
            (void)launcher_util::openUri(launcher_util::fileUri(file()));
          },
      })
  );
  header->addChild(
      ui::button({
          .glyph = "x",
          .glyphSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("launcher.floating-notes.close"),
          .onClick = []() { PanelManager::instance().closePanelById(kPanelId); },
      })
  );
  container->addChild(std::move(header));

  container->addChild(
      ui::input({
          .out = &m_editor,
          .placeholder = i18n::tr("launcher.floating-notes.placeholder"),
          .fontSize = Style::fontSizeBody * scale,
          .horizontalPadding = Style::spaceSm * scale,
          .lineEditing = true,
          .frameVisible = false,
          .surfaceOpacity = panelCardOpacity(),
          .flexGrow = 1.0F,
          .onChange =
              [this](const std::string& text) {
                m_dirty = text != m_loaded;
                if (m_dirty) {
                  setStatus(i18n::tr("launcher.floating-notes.editing"));
                  scheduleSave();
                }
              },
          .onKeyEvent =
              [this](std::uint32_t sym, std::uint32_t modifiers) {
                // Esc closes the window; the text is saved first.
                if (sym == XKB_KEY_Escape && modifiers == 0) {
                  PanelManager::instance().closePanelById(kPanelId);
                  return true;
                }
                return false;
              },
          .configure = [](Input& input) { input.setMultiline(true); },
      })
  );

  container->addChild(
      ui::label({
          .out = &m_status,
          .fontSize = Style::fontSizeMini * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .ellipsize = TextEllipsize::Middle,
      })
  );

  setRoot(std::move(container));
}

void FloatingNotesPanel::onOpen(std::string_view /*context*/) { load(); }

void FloatingNotesPanel::onClose() {
  m_saveTimer.stop();
  save();
  m_container = nullptr;
  m_editor = nullptr;
  m_status = nullptr;
  m_loaded.clear();
  m_dirty = false;
}

void FloatingNotesPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_container == nullptr) {
    return;
  }
  m_container->setSize(width, height);
  m_container->layout(renderer);
}

void FloatingNotesPanel::load() {
  if (m_editor == nullptr) {
    return;
  }
  std::ifstream in(file());
  std::stringstream buffer;
  if (in.is_open()) {
    buffer << in.rdbuf();
  }
  m_loaded = buffer.str();
  m_dirty = false;
  m_editor->setValue(m_loaded);
  setStatus(i18n::tr("launcher.floating-notes.saved-to", "file", file().filename().string()));
}

void FloatingNotesPanel::scheduleSave() {
  m_saveTimer.start(kSaveDelay, [this]() { save(); });
}

void FloatingNotesPanel::save() {
  if (m_editor == nullptr || !m_dirty) {
    return;
  }
  const std::filesystem::path path = file();
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::trunc);
  if (!out.is_open()) {
    setStatus(i18n::tr("launcher.floating-notes.save-failed"));
    return;
  }
  out << m_editor->value();
  if (!m_editor->value().empty() && m_editor->value().back() != '\n') {
    out << '\n';
  }
  m_loaded = m_editor->value();
  m_dirty = false;
  setStatus(i18n::tr("launcher.floating-notes.saved-to", "file", path.filename().string()));
}

void FloatingNotesPanel::setStatus(const std::string& text) {
  if (m_status != nullptr) {
    m_status->setText(text);
  }
}
