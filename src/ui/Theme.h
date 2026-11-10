#pragma once
#include <QColor>
#include <QString>

// Zunimi Studio's look: deep graphite with an electric cyan accent, red for live, amber for recording.
namespace Theme {
inline const QColor Bg{0x0B, 0x0D, 0x12};
inline const QColor Panel{0x12, 0x15, 0x1C};
inline const QColor Raised{0x1A, 0x1E, 0x28};
inline const QColor Line{0x26, 0x2B, 0x38};
inline const QColor Text{0xE9, 0xEC, 0xF2};
inline const QColor Mute{0x8A, 0x93, 0xA6};
inline const QColor Accent{0x22, 0xD3, 0xEE};
inline const QColor Live{0xFF, 0x3B, 0x5C};
inline const QColor Rec{0xFF, 0x9F, 0x1C};
inline const QColor Good{0x34, 0xD3, 0x99};
inline const QColor Warn{0xFB, 0xBF, 0x24};

QString styleSheet();
} // namespace Theme
