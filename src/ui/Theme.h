#pragma once
#include <QColor>
#include <QString>
#include <QStringList>

// Zunimi Studio's look, the same as zunimilabs.online: blue-black with one of four accent colours (ui/accent), red
// for live, amber for recording. Theme::apply() sets it for the whole app and can be called again after a change.
namespace Theme {
inline QColor Bg{0x0A, 0x07, 0x16};     // purple-black glass (Zunimi Stream+)
inline QColor Panel{0x14, 0x0E, 0x2A};
inline QColor Raised{0x1E, 0x16, 0x3A};
inline QColor Line{0x2E, 0x23, 0x52};
inline QColor Text{0xEC, 0xE9, 0xFB};
inline QColor Mute{0xA9, 0xA3, 0xC7};
inline QColor Accent{0x7C, 0x5C, 0xFF};  // the accent's first colour
inline QColor Accent2{0xC0, 0x84, 0xFC}; // its second, for gradients
inline QColor OnAccent{0xFF, 0xFF, 0xFF}; // text on an accent button
inline const QColor Live{0xFF, 0x3B, 0x5C};
inline const QColor Rec{0xFF, 0x9F, 0x1C};
inline const QColor Good{0x34, 0xD3, 0x99};
inline const QColor Warn{0xFB, 0xBF, 0x24};

QStringList accents();                      // violet, sunset, ocean, mint
QString accentName(const QString &accent);  // shown in Settings
void apply();                               // reads ui/accent, sets the colours, palette and style sheet
QString styleSheet();
} // namespace Theme
