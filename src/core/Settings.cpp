#include "core/Settings.h"

#include <QDir>
#include <QStandardPaths>

#include <windows.h>
#include <dpapi.h>

namespace {
QByteArray dpapi(const QByteArray &in, bool encrypt)
{
	DATA_BLOB src{DWORD(in.size()), reinterpret_cast<BYTE *>(const_cast<char *>(in.data()))};
	DATA_BLOB out{};
	const BOOL ok = encrypt ? CryptProtectData(&src, L"Zunimi Studio", nullptr, nullptr, nullptr,
						   CRYPTPROTECT_UI_FORBIDDEN, &out)
				: CryptUnprotectData(&src, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
						     &out);
	if (!ok)
		return {};
	QByteArray result(reinterpret_cast<const char *>(out.pbData), int(out.cbData));
	LocalFree(out.pbData);
	return result;
}
} // namespace

QString Settings::dataDir()
{
	const QString custom = qEnvironmentVariable("ZUNIMI_DATA"); // tests use their own folder
	const QString dir = custom.isEmpty()
				    ? QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/ZunimiStudio"
				    : custom;
	QDir().mkpath(dir);
	return dir;
}

Settings &Settings::get()
{
	static Settings instance;
	return instance;
}

Settings::Settings() : ini(dataDir() + "/settings.ini", QSettings::IniFormat) {}

QVariant Settings::value(const QString &key, const QVariant &fallback) const
{
	return ini.value(key, fallback);
}

void Settings::set(const QString &key, const QVariant &value)
{
	ini.setValue(key, value);
	ini.sync();
}

bool Settings::has(const QString &key) const
{
	return ini.contains(key);
}

QString Settings::secret(const QString &key) const
{
	const QByteArray stored = QByteArray::fromBase64(ini.value(key).toByteArray());
	return stored.isEmpty() ? QString() : QString::fromUtf8(dpapi(stored, false));
}

void Settings::setSecret(const QString &key, const QString &value)
{
	set(key, value.isEmpty() ? QByteArray() : dpapi(value.toUtf8(), true).toBase64());
}
