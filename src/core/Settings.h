#pragma once
#include <QSettings>
#include <QString>
#include <QVariant>

// One INI file in %APPDATA%\ZunimiStudio. Secrets (the stream key) are encrypted with Windows DPAPI,
// so they only open for this Windows user on this PC.
class Settings {
public:
	static Settings &get();
	static QString dataDir();

	QVariant value(const QString &key, const QVariant &fallback = {}) const;
	void set(const QString &key, const QVariant &value);
	bool has(const QString &key) const;
	QString secret(const QString &key) const;
	void setSecret(const QString &key, const QString &value);

private:
	Settings();
	QSettings ini;
};
