/*!
 * @file hid_null.cpp
 * (AI-assisted)
 * Link stubs for DisplayManager / InputManager on platforms without SDL (3DS).
 * The kernel only calls these through Display::GetMainDisplay(), which is always null there,
 * so none of these run; they exist so the kernel links unchanged. Input on the 3DS goes through
 * game/sce/libpad.cpp -> platform/3ds/port/ctr_port.c instead.
 */

#include "game/system/hid/display_manager.h"
#include "game/system/hid/input_manager.h"

// ---------------- DisplayManager ----------------

std::string DisplayManager::get_connected_display_name(int) {
  return "3DS";
}
int DisplayManager::get_active_display_refresh_rate() {
  return 60;
}
int DisplayManager::get_screen_width() {
  return 400;
}
int DisplayManager::get_screen_height() {
  return 240;
}
int DisplayManager::get_num_resolutions(bool) {
  return 1;
}
Resolution DisplayManager::get_resolution(int, bool) {
  return {400, 240, 400.f / 240.f};
}
bool DisplayManager::is_supported_resolution(int width, int height) {
  return width == 400 && height == 240;
}
void DisplayManager::enqueue_set_window_size(int, int) {}
void DisplayManager::enqueue_set_window_display_mode(game_settings::DisplaySettings::DisplayMode,
                                                     const int,
                                                     const int) {}
void DisplayManager::enqueue_set_display_id(int) {}

// ---------------- InputManager ----------------

void InputManager::enqueue_ignore_background_controller_events(const bool) {}
void InputManager::enqueue_update_rumble(const int, const u8, const u8) {}
void InputManager::enqueue_set_controller_led(const int, const u8, const u8, const u8) {}
void InputManager::enqueue_update_mouse_options(const bool, const bool, const bool) {}
void InputManager::enqueue_set_auto_hide_mouse(const bool) {}
void InputManager::enqueue_controller_clear_trigger_effect(
    const int,
    const dualsense_effects::TriggerEffectOption) {}
void InputManager::enqueue_controller_send_trigger_effect_feedback(
    const int,
    const dualsense_effects::TriggerEffectOption,
    const u8,
    const u8) {}
void InputManager::enqueue_controller_send_trigger_effect_vibrate(
    const int,
    const dualsense_effects::TriggerEffectOption,
    const u8,
    const u8,
    const u8) {}
void InputManager::enqueue_controller_send_trigger_effect_weapon(
    const int,
    const dualsense_effects::TriggerEffectOption,
    const u8,
    const u8,
    const u8) {}
void InputManager::enqueue_controller_send_trigger_rumble(const int,
                                                          const u16,
                                                          const u16,
                                                          const u32) {}
void InputManager::enqueue_set_trigger_effects_enabled(const bool) {}
std::string InputManager::get_controller_name(const int) {
  return "3DS";
}
std::string InputManager::get_current_bind(const int,
                                           const InputDeviceType,
                                           const bool,
                                           const int,
                                           const bool) {
  return "";
}
int InputManager::get_controller_index(const int) {
  return 0;
}
void InputManager::set_controller_for_port(const int, const int) {}
bool InputManager::controller_has_led(const int) {
  return false;
}
bool InputManager::controller_has_rumble(const int) {
  return false;
}
bool InputManager::controller_has_pressure_sensitivity_support(const int) {
  return false;
}
bool InputManager::controller_has_trigger_effect_support(const int) {
  return false;
}
void InputManager::enable_keyboard(const bool) {}
void InputManager::set_wait_for_bind(const InputDeviceType, const bool, const bool, const int) {}
void InputManager::set_camera_sens(const float, const float) {}
void InputManager::reset_input_bindings_to_defaults(const int, const InputDeviceType) {}

// ---------------- sdl_util (names used by input_bindings.cpp) ----------------

#include "game/system/hid/sdl_util.h"

namespace sdl_util {
std::string get_mouse_button_name(const int, InputModifiers) {
  return "";
}
std::string get_keyboard_button_name(const int, InputModifiers) {
  return "";
}
std::string get_controller_button_name(const int) {
  return "";
}
std::string get_controller_axis_name(const int) {
  return "";
}
}  // namespace sdl_util
