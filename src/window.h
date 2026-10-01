// A Win32 window that displays a software framebuffer.
//
// The whole graphics dependency of this program is this file plus -lgdi32. There is no
// swapchain, no device, no context and no third-party library: the frame is composed in
// an Image by frame.h and handed to StretchDIBits. That is what lets the identical
// frame be written to a PNG with no window at all -- see the note at the top of font.h.
//
// It is also why this is Win32 rather than GLFW or SDL, reversing the repo README's
// original line. The portability those buy is not real here: simhost::Bind does
// LoadLibraryA on a Windows PE, so offline mode is Windows-only by construction until
// there is a non-Windows sim build. A portable window layer would add a dependency and
// buy nothing today.
//
// The renderer sits behind one seam -- ComposeFrame fills an Image, Present shows it --
// so a GL backend stays a contained change if the UI ever outgrows software drawing.

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "image.h"

namespace simviz {

class Window {
 public:
  ~Window() { Close(); }

  // `fullscreen`: a borderless window covering the primary monitor, and width/height are
  // ignored. Same window class, same input; only the style and the rectangle differ.
  bool Open(const char* title, int width, int height, bool fullscreen = false) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = &Window::Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // we paint every pixel; letting GDI erase only flickers
    wc.lpszClassName = L"OniSimvizWindow";
    if (!GetClassInfoExW(inst, wc.lpszClassName, &wc) && !RegisterClassExW(&wc)) {
      if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }

    const DWORD style = fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    RECT r = {0, 0, width, height};
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (fullscreen) {
      r = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
      x = y = 0;
    } else {
      AdjustWindowRect(&r, style, FALSE);
    }

    wchar_t wtitle[256];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 256);

    hwnd_ = CreateWindowExW(0, wc.lpszClassName, wtitle, style,
                            x, y, r.right - r.left,
                            r.bottom - r.top, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;
    dc_ = GetDC(hwnd_);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    RECT cr;
    GetClientRect(hwnd_, &cr);
    client_w_ = cr.right - cr.left;
    client_h_ = cr.bottom - cr.top;
    return true;
  }

  void Close() {
    if (dc_ && hwnd_) ReleaseDC(hwnd_, dc_);
    if (hwnd_) DestroyWindow(hwnd_);
    dc_ = nullptr;
    hwnd_ = nullptr;
  }

  // Drains the message queue. Returns false once the window has been closed.
  // Edge-triggered state (key presses, wheel, drag delta) is cleared here, so it must
  // be read once per Pump.
  bool Pump() {
    for (int i = 0; i < 256; ++i) pressed_[i] = 0;
    for (int i = 0; i < 256; ++i) repeats_[i] = 0;
    typed_.clear();
    wheel_ = 0;
    drag_dx_ = drag_dy_ = 0;
    clicked_ = false;
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    return !closed_;
  }

  void Present(const Image& img) {
    if (!dc_ || img.empty()) return;
    const int w = img.width(), h = img.height();
    // GDI wants BGRX; the Image is RGB. One pass, no allocation after the first frame.
    bgra_.resize(static_cast<size_t>(w) * h * 4);
    const Rgb* src = img.data();
    uint8_t* dst = bgra_.data();
    for (int i = 0, n = w * h; i < n; ++i) {
      dst[i * 4 + 0] = src[i].b;
      dst[i * 4 + 1] = src[i].g;
      dst[i * 4 + 2] = src[i].r;
      dst[i * 4 + 3] = 255;
    }
    bgra_w_ = w;
    bgra_h_ = h;
    Blit();
  }

  // The UI scale (--ui-scale). The program renders a framebuffer of width() x height()
  // LOGICAL pixels, which Blit enlarges by the factor, and every mouse coordinate is
  // divided by it. So at 2 the HUD, the panels and the 5x8 font are all twice the size on
  // screen, and nothing above this class knows: the layout, the hit tests and --snapshot
  // work in logical pixels as before. A whole factor is drawn with sharp pixels; a
  // fractional one (1.5) is filtered, because nearest-neighbour at 1.5 gives a bitmap font
  // strokes of uneven width.
  void SetScale(float s) { scale_ = s < 1.0f ? 1.0f : (s > 4.0f ? 4.0f : s); }
  float scale() const { return scale_; }
  int client_width() const { return client_w_; }
  int client_height() const { return client_h_; }
  int width() const { return static_cast<int>(client_w_ / scale_); }
  int height() const { return static_cast<int>(client_h_ / scale_); }

  // The native handle, needed only to parent a system dialog (launch.h's file picker) so
  // the dialog is modal to this window rather than to the desktop. Nothing in this program
  // draws through it -- the framebuffer path is Present, above.
  HWND hwnd() const { return hwnd_; }
  bool resized() const { return resized_; }
  void clear_resized() { resized_ = false; }

  bool pressed(int vk) const { return press_count(vk) > 0; }

  // How many separate presses of `vk` arrived in the last Pump. This is a COUNT rather
  // than a flag because a flag loses repeats: two key-downs that reach the queue
  // between one Pump and the next collapse into one press. Measured with synthetic
  // input -- six '+' presses 600 ms apart zoomed to 6.00x as they should, four presses
  // 380 ms apart reached only 3.07x, one step short. A person cannot press fast enough
  // to hit that on a 300 fps loop, but anything driving the window from outside can,
  // and a repeatable action should not depend on the frame rate.
  int press_count(int vk) const {
    return (vk >= 0 && vk < 256) ? pressed_[vk] : 0;
  }
  bool held(int vk) const { return vk >= 0 && vk < 256 && held_[vk]; }
  int wheel() const { return wheel_; }
  int mouse_x() const { return mouse_in_ ? static_cast<int>(mouse_x_ / scale_) : -1; }
  int mouse_y() const { return mouse_in_ ? static_cast<int>(mouse_y_ / scale_) : -1; }
  int drag_dx() const { return static_cast<int>(drag_dx_ / scale_); }
  int drag_dy() const { return static_cast<int>(drag_dy_ / scale_); }

  // The button going DOWN, as an edge, with the position it went down at. Drag deltas
  // alone cannot express "grab this widget": a press with no movement produces no
  // delta, and the first delta of a real drag has already moved the pointer off the
  // thing that was grabbed.
  bool clicked() const { return clicked_; }
  int click_x() const { return click_x_ < 0 ? click_x_ : static_cast<int>(click_x_ / scale_); }
  int click_y() const { return click_y_ < 0 ? click_y_ : static_cast<int>(click_y_ / scale_); }
  bool mouse_down() const { return dragging_; }

  // Characters typed since the last Pump, in order, ASCII. See WM_CHAR below for why the
  // set is bounded and why this is not derivable from press_count.
  const std::string& typed() const { return typed_; }

  // Key-downs including auto-repeat. Use this for a caret and a delete; use press_count
  // for anything a held key should not do a hundred times.
  int key_repeats(int vk) const { return (vk >= 0 && vk < 256) ? repeats_[vk] : 0; }

 private:
  void Blit() {
    if (bgra_.empty()) return;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = bgra_w_;
    bi.bmiHeader.biHeight = -bgra_h_;  // negative: top-down, matching Image row order
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    const bool whole = scale_ == static_cast<float>(static_cast<int>(scale_));
    SetStretchBltMode(dc_, whole ? COLORONCOLOR : HALFTONE);
    if (!whole) SetBrushOrgEx(dc_, 0, 0, nullptr);  // HALFTONE requires it after the mode change
    StretchDIBits(dc_, 0, 0, static_cast<int>(bgra_w_ * scale_ + 0.5f),
                  static_cast<int>(bgra_h_ * scale_ + 0.5f), 0, 0, bgra_w_, bgra_h_, bgra_.data(),
                  &bi, DIB_RGB_COLORS, SRCCOPY);
  }

  static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Window* self = nullptr;
    if (msg == WM_NCCREATE) {
      self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
      self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
      const LRESULT handled = self->Handle(hwnd, msg, wp, lp);
      if (handled != kUnhandled) return handled;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  static constexpr LRESULT kUnhandled = 0x7FFFFFFF;

  LRESULT Handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_CLOSE:
      case WM_DESTROY:
        closed_ = true;
        return 0;

      case WM_SIZE:
        client_w_ = LOWORD(lp);
        client_h_ = HIWORD(lp);
        resized_ = true;
        return 0;

      case WM_PAINT: {
        // Repaint from the last presented frame rather than re-simulating: a window
        // drag must not advance the sim.
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Blit();
        EndPaint(hwnd, &ps);
        return 0;
      }

      case WM_ERASEBKGND:
        return 1;  // every pixel is painted by Blit; erasing first only flickers

      case WM_KEYDOWN:
      case WM_SYSKEYDOWN:
        if (wp < 256) {
          // bit 30 of lParam: the key was already down, i.e. this is auto-repeat.
          if (!(lp & (1 << 30)) && pressed_[wp] < 255) ++pressed_[wp];
          // The same key-down counted WITH auto-repeat. Text editing wants the opposite
          // answer from everything else in this file: holding backspace should delete a
          // run of characters, while holding '+' should not zoom a thousand times.
          if (repeats_[wp] < 255) ++repeats_[wp];
          held_[wp] = true;
        }
        return 0;

      case WM_KEYUP:
      case WM_SYSKEYUP:
        if (wp < 256) held_[wp] = false;
        return 0;

      case WM_MOUSEMOVE: {
        const int x = static_cast<short>(LOWORD(lp));
        const int y = static_cast<short>(HIWORD(lp));
        if (dragging_) {
          drag_dx_ += x - mouse_x_;
          drag_dy_ += y - mouse_y_;
        }
        mouse_x_ = x;
        mouse_y_ = y;
        mouse_in_ = true;
        if (!tracking_) {
          TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
          TrackMouseEvent(&tme);
          tracking_ = true;
        }
        return 0;
      }

      case WM_MOUSELEAVE:
        mouse_in_ = false;
        tracking_ = false;
        return 0;

      case WM_LBUTTONDOWN:
        dragging_ = true;
        clicked_ = true;
        click_x_ = static_cast<short>(LOWORD(lp));
        click_y_ = static_cast<short>(HIWORD(lp));
        mouse_x_ = click_x_;
        mouse_y_ = click_y_;
        mouse_in_ = true;
        SetCapture(hwnd);
        return 0;

      case WM_LBUTTONUP:
        dragging_ = false;
        ReleaseCapture();
        return 0;

      case WM_MOUSEWHEEL:
        wheel_ += GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        return 0;

      // Typed characters, which is a different question from "which key went down" and
      // cannot be answered from WM_KEYDOWN: shift, caps lock, the keyboard layout and the
      // dead-key state all sit between a key and the character it produces, and Windows
      // has already resolved all of them by the time this arrives (Pump calls
      // TranslateMessage, which is what synthesises this message).
      //
      // ASCII ONLY, and that is a deliberate limit rather than an oversight: the HUD is
      // painted with an owned 5x8 bitmap font that has no glyph above 0x7E, so accepting a
      // character it cannot draw would leave a field whose contents differ from what it
      // shows -- the one failure a text box must not have. Backspace (0x08), tab (0x09),
      // return (0x0D) and escape (0x1B) come through here too, as themselves, and repeat
      // while held because Windows repeats WM_CHAR; the reader decides what they mean.
      case WM_CHAR:
        if (wp == 0x08 || wp == 0x09 || wp == 0x0D || wp == 0x1B ||
            (wp >= 0x20 && wp < 0x7F)) {
          if (typed_.size() < 4096) typed_.push_back(static_cast<char>(wp));
        }
        return 0;

      default:
        return kUnhandled;
    }
  }

  HWND hwnd_ = nullptr;
  HDC dc_ = nullptr;
  bool closed_ = false, resized_ = false;
  int client_w_ = 0, client_h_ = 0;
  uint8_t pressed_[256] = {};
  uint8_t repeats_[256] = {};
  std::string typed_;
  bool held_[256] = {};
  int wheel_ = 0;
  int mouse_x_ = -1, mouse_y_ = -1;
  bool mouse_in_ = false, tracking_ = false, dragging_ = false;
  int drag_dx_ = 0, drag_dy_ = 0;
  bool clicked_ = false;
  int click_x_ = -1, click_y_ = -1;
  std::vector<uint8_t> bgra_;
  int bgra_w_ = 0, bgra_h_ = 0;
  float scale_ = 1.0f;
};

}  // namespace simviz
