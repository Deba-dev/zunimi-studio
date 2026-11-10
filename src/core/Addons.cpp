#include "core/Addons.h"

#include "core/Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

QString Addons::coreDir()
{
	return QDir(QCoreApplication::applicationDirPath() + "/../../core").absolutePath();
}

bool Addons::browserInstalled()
{
	const QString dir = coreDir() + "/obs-browser";
	return QFileInfo::exists(dir + "/obs-browser.dll") && QFileInfo::exists(dir + "/libcef.dll") &&
	       QFileInfo::exists(dir + "/obs-browser-page.exe");
}

bool Addons::browserWanted()
{
	if (!browserInstalled())
		return false;
	Settings &st = Settings::get();
	if (st.value("modules/browser", false).toBool())
		return true;
	// an imported collection (or one made before) that already shows web pages switches it on by itself
	const QString name = st.value("scenes/collection", "Main").toString();
	QFile f(Settings::dataDir() + "/collections/" + name + ".json");
	if (f.open(QIODevice::ReadOnly) && f.readAll().contains("\"browser_source\"")) {
		setBrowserEnabled(true);
		return true;
	}
	return false;
}

void Addons::setBrowserEnabled(bool on)
{
	Settings::get().set("modules/browser", on);
}
