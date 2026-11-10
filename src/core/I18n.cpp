#include "core/I18n.h"

#include "core/Settings.h"

#include <QCoreApplication>
#include <QFile>
#include <QTranslator>
#include <QJsonDocument>
#include <QJsonObject>

#include <string>
#include <unordered_map>

namespace {
QString g_language = "en";
// English to translation. Filled once by load() and never changed after, so pointers into it stay valid.
std::unordered_map<std::string, std::string> g_table;
} // namespace

namespace i18n {

QStringList languages()
{
	return {"en", "hi"};
}

QString languageName(const QString &code)
{
	return code == "hi" ? QString::fromUtf8("हिन्दी (Hindi)") : QString("English");
}

void load()
{
	g_table.clear();
	const QString wanted = Settings::get().value("ui/language", "en").toString();
	g_language = languages().contains(wanted) ? wanted : "en";
	if (g_language == "en")
		return;
	QFile f(":/i18n/" + g_language + ".json");
	if (!f.open(QIODevice::ReadOnly)) {
		g_language = "en";
		return;
	}
	const QJsonObject table = QJsonDocument::fromJson(f.readAll()).object();
	g_table.reserve(size_t(table.size()));
	// An empty translation is real: a sentence piece like "The " has no word of its own in Hindi.
	for (auto it = table.begin(); it != table.end(); ++it)
		if (it.value().isString())
			g_table.emplace(it.key().toStdString(), it.value().toString().toStdString());
}

QString language()
{
	return g_language;
}

QString obsLocale()
{
	return g_language == "hi" ? "hi-IN" : "en-US";
}

QString t(const char *english)
{
	if (!english)
		return {};
	if (!g_table.empty()) {
		const auto it = g_table.find(english);
		if (it != g_table.end())
			return QString::fromStdString(it->second);
	}
	return QString::fromUtf8(english);
}

namespace {
// Answers only for the dialog buttons Qt draws itself; everything else is Zunimi's own L() lookups.
class QtWords : public QTranslator {
public:
	using QTranslator::QTranslator;
	bool isEmpty() const override { return g_table.empty(); }
	QString translate(const char *context, const char *source, const char *, int) const override
	{
		if (!source || g_table.empty())
			return {};
		const QByteArray ctx(context ? context : "");
		if (ctx != "QPlatformTheme" && ctx != "QDialogButtonBox" && ctx != "QMessageBox")
			return {};
		const auto it = g_table.find(source);
		return it != g_table.end() ? QString::fromStdString(it->second) : QString();
	}
};
} // namespace

void installQtTranslator()
{
	if (!g_table.empty())
		QCoreApplication::installTranslator(new QtWords(QCoreApplication::instance()));
}

const char *c(const char *english)
{
	if (!english || g_table.empty())
		return english;
	const auto it = g_table.find(english);
	return it != g_table.end() ? it->second.c_str() : english;
}

} // namespace i18n
