// Platform layer: window, GL context or Vulkan surface, and the GLFW input backend (GLFW stays
// behind an opaque pointer, so no other module includes it).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "litholattice/input/input_backend.h"

namespace litholattice {

/// Which graphics API the window is created for: OpenGL gets a GL 3.3 core context; Vulkan gets
/// no context (the renderer creates a surface through createVulkanSurface()).
enum class GraphicsApi { OpenGL33, Vulkan };

/// Windowed (decorated, sized), Maximized (decorated, maximized), Borderless (undecorated
/// window covering the monitor at its desktop mode), Fullscreen (exclusive, a chosen video mode).
enum class DisplayMode { Windowed, Maximized, Borderless, Fullscreen };
const char* displayModeName(DisplayMode m);
/// "windowed" | "maximized" | "borderless" | "fullscreen"; false if unknown.
bool parseDisplayMode(const std::string& s, DisplayMode& out);

struct DisplayDesc {
  DisplayMode mode = DisplayMode::Windowed;
  int monitor = 0;              ///< index into the connected monitors (0 = primary)
  int width = 0, height = 0;    ///< Windowed: client size; Fullscreen: video mode. 0 = default (1280x720 / desktop mode)
  int refreshHz = 0;            ///< Fullscreen only; 0 = the highest the mode offers
};

struct WindowDesc {
  int width = 1280, height = 720;
  std::string title = "LithoLattice";
  bool vsync = false;
  GraphicsApi api = GraphicsApi::OpenGL33;
  DisplayDesc display;
};

/// One window-icon image: straight-alpha RGBA8, `width * height * 4` bytes (not owned).
struct IconPixels { int width = 0, height = 0; const unsigned char* rgba = nullptr; };

class Window {
 public:
  /// Construction token: only Window's own factory can make one, so it can use make_unique / make_shared.
  class Passkey {
    friend class Window;
    Passkey() = default;
  };
  explicit Window(Passkey);  // defined in window.cpp (Impl is complete there)
  /// Title-bar / taskbar icon; the OS picks the best size. Ignored where unsupported (Wayland, macOS).
  void setIcon(const IconPixels* images, int count);
  /// OpenGL: the GL 3.3 core context is current on return. Vulkan: fails (nullptr) when GLFW
  /// finds no Vulkan loader. Returns nullptr on failure, with a log line.
  static std::unique_ptr<Window> create(const WindowDesc& d);
  ~Window();
  bool shouldClose() const;
  /// Input for this window. Its poll() also pumps the window's OS events, once per frame.
  IInputBackend& input();
  /// Loads SDL-format gamepad mappings (gamecontrollerdb.txt) if the file exists.
  bool loadGamepadMappings(const std::string& path);
  /// Presents the GL back buffer; no-op for a Vulkan window (the renderer presents).
  void swap();
  GraphicsApi api() const { return api_; }
  void setTitle(const std::string& t);
  void setCursorCaptured(bool captured);
  bool cursorCaptured() const { return captured_; }
  bool focused() const;           ///< the window has keyboard focus
  void requestClose();            ///< ask the main loop to end (Quit to desktop; works in fullscreen)
  void framebufferSize(int& w, int& h) const;
  /// Switches display mode / monitor / video mode. The framebuffer size changes with it: the
  /// Vulkan renderer recreates its swapchain on the next frame, GL just takes the new viewport.
  void setDisplay(const DisplayDesc& d);
  const DisplayDesc& display() const { return display_; }
  /// Alt+Enter, polled once per frame: toggles Windowed <-> the last non-windowed mode
  /// (Borderless if started windowed). Returns true when it switched.
  bool pollDisplayToggle();
  /// System clipboard as UTF-8 text (empty if it holds no text).
  std::string clipboardText() const;
  void setClipboardText(const std::string& text);
  static void* glProcAddress(const char* name);  // for render backends

  /// Vulkan instance extensions the window system needs for a surface (VK_KHR_surface + the
  /// platform one). Empty if Vulkan is unavailable.
  std::vector<std::string> vulkanInstanceExtensions() const;
  /// Creates a VkSurfaceKHR for this window on `instance` (a VkInstance). Returns the surface
  /// handle, or 0 on failure. The caller owns and destroys the surface.
  uint64_t createVulkanSurface(void* instance) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool captured_ = false;
  bool vsync_ = false;
  bool toggleHeld_ = false;
  DisplayDesc display_;
  DisplayMode lastFull_ = DisplayMode::Borderless;
  int winX_ = 100, winY_ = 100, winW_ = 1280, winH_ = 720;  // restored when going back to Windowed
  GraphicsApi api_ = GraphicsApi::OpenGL33;
};

}  // namespace litholattice
