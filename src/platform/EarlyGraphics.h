#pragma once

#include <QtGlobal>
#include <functional>

class QQuickWindow;

// Windows: the main window comes on screen before its first frame can be drawn.
// - Its Direct3D 11 device is made on a thread of its own while the app starts. Making one loads the GPU
//   driver (~150–200 ms), and Qt makes it only when the window is first exposed, so the first frame waited
//   that long. The window adopts this one instead (QQuickWindow::setGraphicsDevice); if its graphics are
//   ever released (a GPU reset), it makes its own again, as usual.
// - Until that first frame, the window is filled with its background colour through GDI: otherwise it
//   stays see-through, showing what's behind it.
// - While the device is still being made, `meanwhile` runs: work the app would do after the first frame anyway.
// Elsewhere these do nothing.
namespace EarlyGraphics {
#ifdef Q_OS_WIN
void start();                          // first thing in main(): starts making the device
void prepare(QQuickWindow *window, const std::function<void()> &meanwhile);   // once the window is shown
#else
inline void start() {}
inline void prepare(QQuickWindow *, const std::function<void()> &) {}
#endif
} // namespace EarlyGraphics
