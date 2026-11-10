#pragma once
#include <QString>
#include <QStringList>

// The words on screen in the streamer's language. The English text is the key: L("Go Live") gives the Hindi when
// Hindi is chosen, and the English when it isn't or when a line has no translation yet. Picked once at start (the
// language switch restarts the app), so lookups are plain reads.
namespace i18n {
void load(); // reads ui/language from the settings; call once, before any window is built
QString language(); // "en" or "hi"
QString obsLocale(); // what libobs and its plugins should use ("en-US", "hi-IN")
QStringList languages(); // codes, in the order shown
QString languageName(const QString &code); // in that language: "English", "हिन्दी"
QString t(const char *english);
const char *c(const char *english); // the same as a C string that stays valid (for libobs property labels)
void installQtTranslator(); // Qt's own words (Yes, No, Save, Cancel on dialog buttons) from the same table
} // namespace i18n

#define L(text) i18n::t(text)
