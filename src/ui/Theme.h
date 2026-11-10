#pragma once
#include <QColor>
#include <QString>
#include <QStringList>

// Zunimi Studio's look, the same as zunimilabs.online: blue-black with one of four accent colours (ui/accent), red
// for live, amber for recording. Theme::apply() sets it for the whole app and can be called again after a change.
namespace Theme {
inline QColor Bg{0x05, 0x06, 0x0A};
inline QColor Panel{0x0B, 0x0D, 0x16};
inline QColor Raised{0x15, 0x18, 0x25};
inline QColor Line{0x22, 0x26, 0x36};
inline QColor Text{0xF1, 0xF3, 0xF9};
inline QColor Mute{0x9B, 0xA1, 0xB5};
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
