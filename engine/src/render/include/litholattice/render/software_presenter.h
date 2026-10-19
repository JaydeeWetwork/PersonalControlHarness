// Software-renderer present path: gets a SoftwareRenderer framebuffer onto
// a window. The CPU renderer has no swapchain of its own (IRenderer::
// presentMode() == "framebuffer"), so the host uploads the framebuffer as a
// GL texture and draws it as a fullscreen triangle into the window's back
// buffer; Window::swap() then presents it, with vsync following the
// window's swap interval exactly as for the GL backend.
//
// The GL work lives in this library, next to the gl33 backend, because the
// GL entry-point loader and the program helper are private to it; the
// platform layer stays API-agnostic (it only lends glProcAddress + swap(),
// and a Vulkan window has no GL context at all). The presenter therefore
// requires the window's OpenGL 3.3 context (the app creates its window for
// OpenGL when the software backend is selected).
#pragma once
#include <cstdint>
#include <memory>
#include "litholattice/render/renderer.h"  // GLProcLoader

namespace litholattice {

class SoftwarePresenter {
 public:
  virtual ~SoftwarePresenter() = default;
  /// Uploads `pixels` (w * h uint32 texels, 0xAARRGGBB, top row first — the
  /// SoftwareFramebuffer layout) and draws it upright over the whole
  /// (fbW x fbH) default framebuffer. Call with a GL 3.3 core context
  /// current, after SoftwareRenderer::renderFrame() and before
  /// Window::swap(). Steady state allocates nothing on the CPU side: the
  /// texture object is reused and its storage re-specified only when the
  /// frame size changes.
  virtual void present(const uint32_t* pixels, int w, int h, int fbW, int fbH) = 0;
};

/// Creates the presenter for the calling thread's current GL 3.3 context.
/// Loads the shared GL entry points (the same gate as createGL33Renderer).
/// Returns nullptr on failure (no loader, a missing entry point, or the
/// blit program failed to build), with a log line.
std::unique_ptr<SoftwarePresenter> createSoftwarePresenter(GLProcLoader loader);

}  // namespace litholattice
