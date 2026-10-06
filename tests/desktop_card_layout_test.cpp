#include "config/config_service.h"
#include "core/files/file_watcher.h"
#include "net/http_client.h"
#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "shell/desktop/widgets/desktop_batteries_widget.h"
#include "shell/desktop/widgets/desktop_calendar_widget.h"
#include "shell/desktop/widgets/desktop_clock_widget.h"
#include "shell/desktop/widgets/desktop_collection_widget.h"
#include "shell/desktop/widgets/desktop_media_player_widget.h"
#include "shell/desktop/widgets/desktop_photos_widget.h"
#include "shell/desktop/widgets/desktop_stack_widget.h"
#include "shell/desktop/widgets/desktop_status_card_widget.h"
#include "shell/desktop/widgets/desktop_weather_widget.h"
#include "system/weather_service.h"
#include "ui/controls/label.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string_view>
#include <vector>

namespace {
  class StubRenderer final : public Renderer {
  public:
    // Fixed-advance shaping, so a wrap budget maps to an exact character count and the
    // resulting line count is predictable.
    TextMetrics measureText(
        std::string_view text, float fontSize, FontWeight, float maxWidth, int maxLines, TextAlign, std::string_view,
        TextEllipsize, bool
    ) override {
      constexpr float kAdvance = 10.0F;
      const float natural = static_cast<float>(text.size()) * kAdvance;
      float width = natural;
      int lineCount = text.empty() ? 0 : 1;
      if (maxWidth > 0.0F && natural > maxWidth) {
        const float perLine = std::floor(maxWidth / kAdvance) * kAdvance;
        lineCount = perLine > 0.0F ? static_cast<int>(std::ceil(natural / perLine)) : 1;
        if (maxLines > 0) {
          lineCount = std::min(lineCount, maxLines);
        }
        width = maxWidth;
      }
      return TextMetrics{
          .width = width,
          .right = width,
          .bottom = fontSize * static_cast<float>(lineCount),
          .lineCount = lineCount,
      };
    }

    TextMetrics measureFont(float fontSize, FontWeight) override { return TextMetrics{.bottom = fontSize}; }

    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, FontWeight
    ) override {}

    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, FontWeight
    ) override {}

    TextMetrics measureGlyph(char32_t, float) override {
      return TextMetrics{
          .width = 18.0F,
          .left = 1.5F,
          .right = 19.5F,
          .top = -15.0F,
          .bottom = 3.0F,
          .inkTop = -15.0F,
          .inkBottom = 3.0F,
          .inkLeft = 1.5F,
          .inkRight = 19.5F,
      };
    }

    TextureManager& textureManager() override { std::abort(); }
    [[nodiscard]] float renderScale() const noexcept override { return 1.0F; }
  };

  void expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "desktop_card_layout_test: {}", message);
      std::exit(1);
    }
  }

  bool near(float a, float b) { return std::abs(a - b) < 0.01F; }

  void checkWidget(DesktopWidget& widget, Renderer& renderer, float width, float height) {
    widget.setBackgroundStyle(colorSpecFromRole(ColorRole::Surface), Style::radiusXl, Style::cardPadding);
    widget.create();
    widget.update(renderer);
    widget.layout(renderer);
    expect(
        near(widget.intrinsicWidth(), width) && near(widget.intrinsicHeight(), height),
        "preset has expected outer footprint"
    );
    expect(!widget.needsFrameTick() && !widget.wantsSecondTicks(), "cards do not animate or poll every second");
    widget.setBox(500.0F, 460.0F);
    widget.layout(renderer);
    expect(
        near(widget.root()->width(), 472.0F) && near(widget.root()->height(), 432.0F),
        "resized content fills the padded tile"
    );
    expect(near(widget.contentScale(), 1.0F), "resizing retains the shell text scale");
    widget.setBox(208.0F, 208.0F);
    widget.layout(renderer);
    expect(near(widget.root()->width(), 180.0F), "shrinking reflows back to a compact card");
    widget.setBox(0.0F, 0.0F);
    widget.setContentScale(1.5F);
    widget.layout(renderer);
    expect(near(widget.intrinsicWidth(), width * 1.5F), "presets follow the shell content scale");
    int layouts = 0;
    widget.setLayoutCallback([&]() { ++layouts; });
    widget.update(renderer);
    widget.update(renderer);
    expect(layouts == 0, "unchanged card data does not schedule layouts");
  }
} // namespace

int main() {
  char fixtureTemplate[] = "/tmp/noctalia-card-test-XXXXXX";
  const char* fixturePath = mkdtemp(fixtureTemplate);
  expect(fixturePath != nullptr, "isolated fixture directory exists");
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{fixturePath};
  for (const char* key : {"NOCTALIA_CONFIG_HOME", "NOCTALIA_STATE_HOME", "NOCTALIA_DATA_HOME", "XDG_CACHE_HOME"}) {
    setenv(key, fixturePath, 1);
  }
  const Color original = rgba(0.9F, 0.2F, 0.4F, 0.6F);
  const auto muted = withSaturation(original, 0.18F);
  const auto luminance = [](Color c) { return c.r * 0.2126F + c.g * 0.7152F + c.b * 0.0722F; };
  expect(
      near(luminance(original), luminance(muted)) && muted.a == original.a,
      "desktop blending preserves brightness and transparency"
  );
  expect(withSaturation(original, 1.0F) == original, "full colour is unchanged");
  StubRenderer renderer;
  for (const auto size : {desktop_cards::Size::Small, desktop_cards::Size::Medium, desktop_cards::Size::Large}) {
    const float width = size == desktop_cards::Size::Small ? 208.0F : 432.0F;
    const float height = size == desktop_cards::Size::Large ? 432.0F : 208.0F;
    DesktopWeatherWidget weather(nullptr, {.cardSize = size});
    checkWidget(weather, renderer, width, height);
    DesktopCalendarWidget calendar(nullptr, nullptr, {.cardSize = size});
    checkWidget(calendar, renderer, width, height);
    DesktopClockWidget digital({.format = "{:%H:%M}", .cardSize = size, .showSeconds = false});
    checkWidget(digital, renderer, width, height);
    DesktopClockWidget analog(
        {.style = DesktopClockWidget::Style::Analog, .format = "{:%H:%M}", .cardSize = size, .showSeconds = false}
    );
    checkWidget(analog, renderer, width, height);
    DesktopMediaPlayerWidget media(nullptr, nullptr, {.cardSize = size});
    checkWidget(media, renderer, width, height);
    DesktopBatteriesWidget batteries({}, size, {});
    checkWidget(batteries, renderer, width, height);
    DesktopStatusCardWidget screenTime({}, size);
    checkWidget(screenTime, renderer, width, height);
    DesktopPhotosWidget photos({}, {}, 5, size);
    checkWidget(photos, renderer, width, height);
    const std::vector<DesktopWidgetState> stackedCards{
        {.id = "clock", .type = "clock"}, {.id = "tips", .type = "tips"}
    };
    const std::string stackSize = size == desktop_cards::Size::Small ? "small"
        : size == desktop_cards::Size::Large                         ? "large"
                                                                     : "medium";
    DesktopStackWidget stack("fixture", stackedCards, {{"card_size", stackSize}}, {});
    checkWidget(stack, renderer, width, height);
    stack.showPage(1);
    stack.layout(renderer);
    expect(stack.page() == 1, "stack switches to the requested member");
    stack.showPage(99);
    expect(stack.page() == 1, "invalid stack page is ignored");
    const std::string cardSize = size == desktop_cards::Size::Small ? "small"
        : size == desktop_cards::Size::Large                        ? "large"
                                                                    : "medium";
    for (const char* kind :
         {"notes", "journal", "reminders", "shortcuts", "contacts", "reading_list", "tips", "news", "podcasts",
          "stocks", "home", "find_my", "video_library"}) {
      DesktopCollectionWidget::Settings settings{
          {"card_size", cardSize}, {"file_path", std::string(fixturePath) + "/missing-notes.md"}
      };
      DesktopCollectionWidget empty(kind, settings, {});
      checkWidget(empty, renderer, width, height);
      settings["items"] = std::vector<std::string>{"First task", "Second task", "First task"};
      settings["entries"] = WidgetSettingStringMap{
          {"Website", "https://example.test"},
          {"Contact", "mailto:fixture@example.test"},
          {"Never run automatically", "exit 42"}
      };
      DesktopCollectionWidget populated(kind, settings, {});
      checkWidget(populated, renderer, width, height);
    }
  }
  {
    AnimationManager animations;
    DesktopStackWidget rotating(
        "rotating", {{.id = "first", .type = "tips"}, {.id = "second", .type = "tips"}},
        {{"card_size", std::string("small")}, {"auto_rotate", true}}, {}
    );
    rotating.setAnimationManager(&animations);
    rotating.create();
    rotating.layout(renderer);
    expect(rotating.wantsSecondTicks(), "automatic rotation uses the existing second tick");
    rotating.setPinned(true);
    expect(rotating.pinned() && !rotating.wantsSecondTicks(), "pinning holds the page and stops rotation ticks");
    rotating.showPage(1);
    rotating.layout(renderer);
    expect(
        !rotating.pinned() && rotating.page() == 1 && animations.hasActive(),
        "manual navigation unpins and fades the incoming page"
    );
    MotionService::instance().setEnabled(false);
    rotating.showPage(0);
    rotating.layout(renderer);
    expect(rotating.root()->children().front()->opacity() == 1.0F, "reduced motion shows the new page fully opaque");
    rotating.setEditorPreview(true);
    expect(!rotating.wantsSecondTicks(), "editor previews never rotate");
    MotionService::instance().setEnabled(true);
  }
  {
    DesktopStackWidget smart(
        "smart", {{.id = "clock", .type = "clock"}, {.id = "calendar", .type = "calendar"}}, {{"smart_rotate", true}},
        {}
    );
    smart.create();
    smart.update(renderer);
    smart.layout(renderer);
    expect(smart.wantsSecondTicks(), "Smart Rotate observes context even with timed rotation disabled");
    smart.setPinned(true);
    expect(!smart.wantsSecondTicks(), "pinned stack does not poll for smart suggestions");
    smart.setPinned(false);
    expect(smart.wantsSecondTicks(), "unpin resumes smart context checks");
    smart.setEditorPreview(true);
    expect(!smart.wantsSecondTicks(), "Smart Rotate stays idle in editor previews");
    DesktopStackWidget unrelated(
        "unrelated", {{.id = "clock", .type = "clock"}, {.id = "tips", .type = "tips"}}, {{"smart_rotate", true}}, {}
    );
    unrelated.create();
    expect(!unrelated.wantsSecondTicks(), "stacks without eligible cards do not poll for context");
  }
  {
    const auto path = std::filesystem::path(fixturePath) / "watched-note.md";
    std::ofstream(path) << "Initial note\n";
    FileWatcher watcher;
    DesktopWidgetRuntimeServices services;
    services.scriptDeps.fileWatcher = &watcher;
    DesktopCollectionWidget notes("notes", {{"file_path", path.string()}}, services);
    notes.create();
    notes.layout(renderer);
    int updates = 0;
    notes.setUpdateCallback([&] { ++updates; });
    std::ofstream saving(path, std::ios::trunc);
    watcher.dispatch();
    expect(updates == 0, "truncating an open note does not refresh its card");
    saving << "Updated note\n";
    saving.flush();
    watcher.dispatch();
    expect(updates == 0, "an unfinished note write does not consume its refresh notification");
    saving.close();
    watcher.dispatch();
    expect(updates == 1, "closing the saved note triggers a refresh");
    notes.update(renderer);
    notes.layout(renderer);
    const auto containsText = [](const auto& self, const Node& node) -> bool {
      if (const auto* label = dynamic_cast<const Label*>(&node); label && label->text() == "Updated note")
        return true;
      return std::ranges::any_of(node.children(), [&](const auto& child) { return self(self, *child); });
    };
    expect(containsText(containsText, *notes.root()), "the card displays the completed note save");
  }
  DesktopWeatherWidget classic(nullptr, {});
  classic.create();
  classic.layout(renderer);
  expect(
      near(classic.intrinsicWidth(), 180.0F) && near(classic.intrinsicHeight(), 72.0F),
      "legacy weather keeps its layout"
  );
  classic.setBox(360.0F, 144.0F);
  classic.layout(renderer);
  expect(near(classic.contentScale(), 2.0F), "legacy weather still scales its content");
  const auto compact = desktop_cards::resolve(desktop_cards::Size::Large, 1.0F, 180.0F, 180.0F, 14.0F);
  expect(compact.size == desktop_cards::Size::Small, "narrow card hides extended content");
  const auto shortWide = desktop_cards::resolve(desktop_cards::Size::Large, 1.0F, 404.0F, 340.0F, 14.0F);
  expect(shortWide.size == desktop_cards::Size::Medium, "shortened wide cards do not clip an expanded forecast");
  const auto wide = desktop_cards::resolve(desktop_cards::Size::Small, 1.0F, 404.0F, 180.0F, 14.0F);
  expect(wide.size == desktop_cards::Size::Medium, "wide card reveals intermediate content");
  // A partially populated provider response must settle after one layout, even when the
  // chosen card has space for six days. No HTTP polling or user configuration is needed.
  ConfigService config;
  {
    expect(config.setStateString("desktop_stacks", "pages", R"({"partial":"clock"})"), "fixture page saved");
    expect(config.setStateString("desktop_stacks", "pins", R"({"partial":"clock"})"), "fixture pin saved");
    DesktopWidgetRuntimeServices services;
    services.scriptDeps.configService = &config;
    DesktopStackWidget partial(
        "partial",
        {{.id = "missing", .type = "media_player"},
         {.id = "calendar", .type = "calendar"},
         {.id = "clock", .type = "clock"}},
        {{"smart_rotate", true}}, services
    );
    partial.create();
    partial.update(renderer);
    partial.layout(renderer);
    expect(partial.page() == 1 && partial.pinned(), "unavailable services do not shift the pinned member identity");
    partial.showPage(0);
    expect(
        config.stateString("desktop_stacks", "pages") == R"({"partial":"calendar"})",
        "navigation persists the actual rendered member when a source is unavailable"
    );
    expect(!partial.pinned(), "manual navigation clears the pin after source filtering");
  }
  HttpClient http;
  WeatherService service(config, http);
  service.setLocation(WeatherCoordinates{51.5, -0.12}, "Fixture", "test");
  auto& snapshot = const_cast<WeatherSnapshot&>(service.snapshot());
  snapshot.valid = true;
  snapshot.locationName = "A long location name that must stay inside its card";
  snapshot.forecastDays.push_back({.dateIso = "2099-01-02", .temperatureMaxC = 15.0, .temperatureMinC = -2.0});
  DesktopWeatherWidget sparse(&service, {.cardSize = desktop_cards::Size::Large});
  sparse.setBackgroundStyle(colorSpecFromRole(ColorRole::Surface), Style::radiusXl, Style::cardPadding);
  sparse.create();
  sparse.layout(renderer);
  int layouts = 0;
  sparse.setLayoutCallback([&]() { ++layouts; });
  sparse.update(renderer);
  sparse.update(renderer);
  expect(layouts == 0, "short forecast does not repeatedly request layouts");
  snapshot.forecastDays.clear();
  sparse.update(renderer);
  expect(layouts == 1, "removing forecast data schedules one layout");
  sparse.layout(renderer);
  sparse.update(renderer);
  expect(layouts == 1, "empty forecast settles after hiding stale rows");
  sparse.setBox(0.0F, 0.0F);
  sparse.applySetting("card_size", std::string("classic"), {}, renderer);
  expect(
      near(sparse.intrinsicWidth(), 208.0F) && near(sparse.intrinsicHeight(), 100.0F),
      "classic can be restored after using a card"
  );
  std::println("PASS: desktop card presets, responsive resize, scaling, legacy layout and idle updates");
}
