#pragma once

#include <cstddef>
#include <cstdint>

struct wl_display;
struct wl_seat;
struct zwp_virtual_keyboard_manager_v1;
struct zwp_virtual_keyboard_v1;
struct xkb_context;
struct xkb_keymap;

enum class VirtualPasteShortcut : std::uint8_t {
  CtrlV = 0,
  CtrlShiftV = 1,
  ShiftInsert = 2,
};

class VirtualKeyboardService {
public:
  VirtualKeyboardService();
  ~VirtualKeyboardService();

  VirtualKeyboardService(const VirtualKeyboardService&) = delete;
  VirtualKeyboardService& operator=(const VirtualKeyboardService&) = delete;

  // `display` is flushed after a paste chord so the keys leave before the launcher's own close
  // round-trip; it may be null, in which case the main loop's next flush sends them.
  bool bind(zwp_virtual_keyboard_manager_v1* manager, wl_seat* seat, wl_display* display = nullptr);
  void cleanup();

  [[nodiscard]] bool isAvailable() const noexcept;
  [[nodiscard]] bool sendPasteShortcut(VirtualPasteShortcut shortcut);
  // Presses Backspace `count` times (snippet expansion erasing the typed keyword).
  [[nodiscard]] bool typeBackspaces(std::size_t count);

private:
  [[nodiscard]] bool ensureKeyboard();
  [[nodiscard]] bool ensureKeymap();
  void pressChord(std::uint32_t key, bool ctrlPressed, bool shiftPressed);
  void sendKey(std::uint32_t key, bool pressed);
  void updateModifiers(bool ctrlPressed, bool shiftPressed);

  zwp_virtual_keyboard_manager_v1* m_manager = nullptr;
  wl_seat* m_seat = nullptr;
  wl_display* m_display = nullptr;
  zwp_virtual_keyboard_v1* m_keyboard = nullptr;
  xkb_context* m_xkbContext = nullptr;
  xkb_keymap* m_xkbKeymap = nullptr;
  std::uint32_t m_ctrlMask = 0;
  std::uint32_t m_shiftMask = 0;
  bool m_keymapUploaded = false;
};
