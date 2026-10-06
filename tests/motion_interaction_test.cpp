#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "render/animation/motion_service.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/rect_node.h"
#include "shell/desktop/widgets/desktop_stack_widget.h"
#include "shell/settings/settings_modal_host.h"
#include "tests/test_check.h"
#include "ui/controls/button.h"
#include "ui/node_motion.h"

#include <chrono>
#include <cmath>
#include <thread>

namespace {
  class EmptyRenderer final : public Renderer {
    TextMetrics measureText(
        std::string_view, float, FontWeight, float, int, TextAlign, std::string_view, TextEllipsize, bool
    ) override {
      return {};
    }
    TextMetrics measureFont(float, FontWeight) override { return {}; }
    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, FontWeight
    ) override {}
    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, FontWeight
    ) override {}
    TextMetrics measureGlyph(char32_t, float) override { return {}; }
    TextureManager& textureManager() override { std::abort(); }
    float renderScale() const noexcept override { return 1; }
  };

  Color buttonColor(const Button& button) {
    for (const auto& child : button.children())
      if (const auto* background = dynamic_cast<const RectNode*>(child.get()))
        return background->style().fill;
    TEST_CHECK(false);
    return {};
  }
} // namespace

int main() {
  KeybindMatcher::setMatcher(KeybindAction::Validate, [](std::uint32_t key, std::uint32_t) {
    return key == XKB_KEY_Return;
  });
  auto& motion = MotionService::instance();
  motion.setEnabled(true);
  motion.setSpeed(1);
  AnimationManager animations;
  {
    Node card;
    card.setAnimationManager(&animations);
    card.setSize(200, 100);
    Motion::revealNode(card);
    TEST_CHECK(card.opacity() == 0 && card.scale() < 1);
    TEST_CHECK(card.width() == 200 && card.height() == 100);
    motion.setEnabled(false);
    TEST_CHECK(card.opacity() == 1 && card.scale() == 1);
    animations.tick(0);
    TEST_CHECK(!animations.hasActive());
    Motion::liftNode(card, true);
    TEST_CHECK(card.scale() == 1 && !animations.hasActive());
    motion.setEnabled(true);
    Motion::liftNode(card, true);
    motion.setEnabled(false);
    TEST_CHECK(card.scale() == 1);
    animations.tick(0);
  }
  motion.setEnabled(true);
  {
    auto node = std::make_unique<Node>();
    node->setAnimationManager(&animations);
    Motion::revealNode(*node);
    TEST_CHECK(animations.hasActive());
    node.reset();
    TEST_CHECK(!animations.hasActive());
  }
  {
    motion.setEnabled(false);
    Button button;
    button.setAnimationManager(&animations);
    const auto colors = [](Color fill) {
      return Button::ButtonStateColors{fixedColorSpec(fill), clearColorSpec(), fixedColorSpec(Color{1, 1, 1, 1})};
    };
    button.setCustomPalette(
        {.normal = colors({0, 0, 0, 1}),
         .hover = colors({1, 0, 0, 1}),
         .pressed = colors({0, 1, 0, 1}),
         .disabled = colors({0, 0, 0, 1})}
    );
    int clicks = 0;
    button.setOnClick([&]() { ++clicks; });
    animations.tick(0);
    motion.setEnabled(true);
    button.setHoveredVisual(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    animations.tick(0);
    const auto displayed = buttonColor(button);
    button.setPressedVisual(true);
    animations.tick(0);
    TEST_CHECK(std::abs(buttonColor(button).r - displayed.r) < .05F);
    motion.setEnabled(false);
    TEST_CHECK(buttonColor(button).g == 1);
    animations.tick(0);
    button.setHoveredVisual(false);
    button.setPressedVisual(false);
    button.inputArea()->dispatchKey(XKB_KEY_Return, 0, 0, true);
    TEST_CHECK(clicks == 1 && buttonColor(button).g == 1);
    button.inputArea()->dispatchKey(XKB_KEY_Return, 0, 0, false);
    TEST_CHECK(buttonColor(button).g == 0);
    animations.tick(0);
  }
  {
    motion.setEnabled(true);
    DesktopStackWidget stack("", {{.id = "one", .type = "label"}, {.id = "two", .type = "label"}}, {}, {});
    stack.setAnimationManager(&animations);
    stack.create();
    auto* first = stack.root()->children()[0].get();
    auto* second = stack.root()->children()[1].get();
    stack.showPage(1);
    TEST_CHECK(first->visible() && second->visible());
    TEST_CHECK(!first->hitTestVisible() && second->hitTestVisible());
    stack.showPage(0);
    TEST_CHECK(first->hitTestVisible() && !second->hitTestVisible());
    motion.setEnabled(false);
    animations.tick(0);
    TEST_CHECK(first->visible() && first->opacity() == 1 && !second->visible());
    stack.showPage(1);
    TEST_CHECK(!first->visible() && second->visible() && second->opacity() == 1);
  }
  TEST_CHECK(!animations.hasActive());
  {
    motion.setEnabled(true);
    EmptyRenderer renderer;
    Node scene;
    scene.setAnimationManager(&animations);
    InputDispatcher input;
    input.setSceneRoot(&scene);
    settings::SettingsModalHost host;
    host.initialize(input, [] {}, [] {});
    host.attach(scene, nullptr, renderer, 800, 600);
    int cancellations = 0;
    const auto build = [] { return std::make_unique<Node>(); };
    TEST_CHECK(host.push({.build = build, .requestClose = [&] {
                            ++cancellations;
                            host.pop();
                          }}));
    host.requestCloseTop();
    TEST_CHECK(host.isOpen());
    host.closeAll();
    TEST_CHECK(cancellations == 1 && !host.isOpen());
    animations.tick(0);

    int attempts = 0;
    const auto parent = host.push({.build = build, .requestClose = [&] { ++attempts; }});
    TEST_CHECK(parent);
    host.requestCloseTop();
    motion.setEnabled(false);
    animations.tick(0);
    TEST_CHECK(attempts == 1 && host.isTop(*parent) && host.topRoot()->opacity() == 1);
    motion.setEnabled(true);
    host.requestCloseTop();
    TEST_CHECK(host.push({.build = build}));
    motion.setEnabled(false);
    animations.tick(0);
    host.pop();
    TEST_CHECK(host.isTop(*parent) && host.topRoot()->opacity() == 1);
    host.requestCloseTop();
    animations.tick(0);
    TEST_CHECK(attempts == 2);
    host.closeAll();
  }
  TEST_CHECK(!animations.hasActive());
  motion.setEnabled(true);
}
