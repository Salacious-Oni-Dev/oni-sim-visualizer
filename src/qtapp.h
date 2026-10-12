// The one QApplication this process has, and the check that proves Qt loads at all.
//
// Qt allows exactly one QApplication per process and wants it built before any widget,
// font or pixmap exists. The command-line paths (--probe, --digest, every test) never need
// one, so it is made on demand by whichever GUI path is first, never in main(). That keeps
// a headless run of this exe headless: no platform plugin is loaded, no DPI awareness is
// set, and nothing about the arms test.sh drives changes because the binary links Qt.
//
// `platform` is Qt's own -platform argument ("windows", "offscreen", ...); empty means
// Qt's default for the OS. It is passed as argv rather than through QT_QPA_PLATFORM
// because an environment variable set in WSL does not cross into a .exe started through
// interop unless WSLENV names it, and a check that silently ran on the wrong platform
// would prove nothing.

#pragma once

#include <QApplication>
#include <QFont>
#include <QImage>
#include <QLabel>
#include <QPalette>
#include <QStyleFactory>
#include <QtGlobal>

#include <memory>
#include <string>

#include "out.h"
#include "qt/fonts.h"

namespace simviz {
namespace qtapp {

// --font large. Was font.h's, with the bitmap font it chose between; now it only picks the
// size the bundled font is drawn at (FontPixelSize).
inline bool g_large_font = false;

// `platform` empty means the real window system, with Qt's FreeType font engine forced on it.
// The default Windows engine (DirectWrite) depends on the host's ClearType and gamma settings;
// FreeType over the bundled fonts does not. The goldens are drawn on `offscreen`, and those
// were measured byte-identical under Windows and under wine; whether the windowed
// FreeType path draws the very same text pixels as `offscreen` has NOT been measured.
inline QApplication* App(const char* platform = "") {
  static std::unique_ptr<QApplication> app;
  if (app) return app.get();
  // QT_QPA_PLATFORM, when set, wins over the default -- it is how a smoke run is pointed at
  // `offscreen` so no window appears on anybody's desktop (from WSL it has to be named in
  // WSLENV to reach the exe at all).
  if ((platform == nullptr || platform[0] == '\0') && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    platform = "windows:fontengine=freetype";
  // QApplication keeps references to argc and argv for its whole life, so both are static.
  static int argc = 1;
  static std::string arg0 = "simviz", flag = "-platform", value;
  static char* argv[4] = {arg0.data(), nullptr, nullptr, nullptr};
  if (platform != nullptr && platform[0] != '\0') {
    value = platform;
    argv[1] = flag.data();
    argv[2] = value.data();
    argc = 3;
  }
  app = std::make_unique<QApplication>(argc, argv);
  // Fusion, not the host's native style: native styles follow the Windows theme, accent colour
  // and version, so the same build would draw different controls on two machines. Fusion is
  // Qt's own drawing and is the same everywhere.
  app->setStyle(QStyleFactory::create("Fusion"));
  // Dark, because the world is: a white panel beside a near-black world is the brightest
  // thing on screen and pulls the eye off the cells. The HUD's own colours (frame.h) are the
  // anchor, so panel text and the overlay read as one program.
  QPalette pal;
  pal.setColor(QPalette::Window, QColor(30, 33, 40));
  pal.setColor(QPalette::WindowText, QColor(225, 232, 240));
  pal.setColor(QPalette::Base, QColor(20, 22, 28));
  pal.setColor(QPalette::AlternateBase, QColor(36, 40, 48));
  pal.setColor(QPalette::Text, QColor(225, 232, 240));
  pal.setColor(QPalette::PlaceholderText, QColor(110, 118, 130));
  pal.setColor(QPalette::Button, QColor(44, 48, 58));
  pal.setColor(QPalette::ButtonText, QColor(225, 232, 240));
  pal.setColor(QPalette::Highlight, QColor(70, 110, 160));
  pal.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
  pal.setColor(QPalette::ToolTipBase, QColor(16, 18, 24));
  pal.setColor(QPalette::ToolTipText, QColor(225, 232, 240));
  pal.setColor(QPalette::Link, QColor(120, 170, 230));
  pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(110, 118, 130));
  pal.setColor(QPalette::Disabled, QPalette::Text, QColor(110, 118, 130));
  pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(110, 118, 130));
  app->setPalette(pal);
  qtfonts::Register();
  return app.get();
}

// The application font, sized from --ui-scale and --font large. Every widget inherits it; the
// world view's overlay asks for the mono face at the same size.
inline int FontPixelSize(float ui_scale, bool large) {
  const float base = large ? 15.0f : 12.0f;
  const float scale = ui_scale > 0.0f ? ui_scale : 1.0f;
  return static_cast<int>(base * scale + 0.5f);
}

inline void SetFonts(float ui_scale, bool large) {
  QFont f(qtfonts::Sans());
  f.setPixelSize(FontPixelSize(ui_scale, large));
  f.setHintingPreference(QFont::PreferNoHinting);
  QApplication::setFont(f);
}

inline QFont MonoFont(float ui_scale, bool large) {
  QFont f(qtfonts::Mono());
  f.setPixelSize(FontPixelSize(ui_scale, large));
  f.setHintingPreference(QFont::PreferNoHinting);
  f.setStyleHint(QFont::Monospace);
  return f;
}

// --qt-check: boot Qt on the offscreen platform, build one widget and grab it. Exits 0 and
// prints the version and platform only if all three worked, so a release zip missing a DLL
// or a plugin fails here with Qt's own message rather than as a window that never opens.
inline int Check() {
  QApplication* app = App("offscreen");
  QLabel label("oni-sim-visualizer");
  label.resize(160, 40);
  const QImage grab = label.grab().toImage();
  if (grab.isNull() || grab.width() <= 0) {
    out::Printf("qt %s: platform %s booted, but grabbing a widget returned no image\n",
                qVersion(), app->platformName().toUtf8().constData());
    return 1;
  }
  out::Printf("qt %s platform %s grab %dx%d\n", qVersion(),
              app->platformName().toUtf8().constData(), grab.width(), grab.height());
  return 0;
}

}  // namespace qtapp
}  // namespace simviz
