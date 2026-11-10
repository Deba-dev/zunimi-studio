#include "core/ObsCore.h"
#include "core/Settings.h"
#include "core/Updater.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/WindowModes.h"
#include "version.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMessageBox>
#include <QTimer>

#include <util/base.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include <windows.h>

namespace {
FILE *g_log = nullptr;
std::mutex g_logLock;

// libobs logs from many threads; one file per run, the last 10 kept.
void logHandler(int level, const char *format, va_list args, void *)
{
	char line[4096];
	vsnprintf(line, sizeof(line), format, args);
	std::lock_guard<std::mutex> lock(g_logLock);
	if (g_log) {
		const char *tag = level <= LOG_ERROR ? "error" : level <= LOG_WARNING ? "warn" : "info";
		fprintf(g_log, "%s [%s] %s\n", QTime::currentTime().toString("hh:mm:ss.zzz").toUtf8().constData(), tag,
			line);
		fflush(g_log);
	}
}

void openLog()
{
	QDir dir(Settings::dataDir() + "/logs");
	dir.mkpath(".");
	const QStringList old = dir.entryList({"*.txt"}, QDir::Files, QDir::Name);
	for (int i = 0; i + 9 < old.size(); i++)
		dir.remove(old[i]);
	const QString path = dir.filePath(QDateTime::currentDateTime().toString("yyyy-MM-dd hh-mm-ss") + ".txt");
	g_log = _wfopen(reinterpret_cast<const wchar_t *>(path.utf16()), L"w");
	base_set_log_handler(logHandler, nullptr);
}
} // namespace

int main(int argc, char *argv[])
{
	// libobs finds its data and modules relative to the exe's folder (bin/64bit).
	wchar_t exe[MAX_PATH];
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	SetCurrentDirectoryW(reinterpret_cast<const wchar_t *>(QFileInfo(QString::fromWCharArray(exe)).absolutePath().utf16()));

	QApplication app(argc, argv);
	app.setApplicationName(ZUNIMI_NAME);
	app.setApplicationVersion(ZUNIMI_VERSION);
	app.setQuitOnLastWindowClosed(false); // hiding to the tray must not end the app
	app.setStyle("Fusion");
	app.setStyleSheet(Theme::styleSheet());

	QLockFile lock(Settings::dataDir() + "/running.lock");
	if (!lock.tryLock(100)) {
		QMessageBox::information(nullptr, ZUNIMI_NAME,
					 "Zunimi Studio is already running. Look for its icon in the tray.");
		return 0;
	}

	openLog();
	blog(LOG_INFO, "%s %s starting", ZUNIMI_NAME, ZUNIMI_VERSION);
	int code = 0;
	{
		ObsCore core;
		QString error;
		if (!core.start(&error)) {
			QMessageBox::critical(nullptr, ZUNIMI_NAME, error);
			return 1;
		}
		MainWindow window(&core);
		window.modes()->show();
		if (app.arguments().contains("--selftest"))
			QTimer::singleShot(3000, &app, [&window] { window.modes()->quit(); });
		if (app.arguments().contains("--selftest-record")) {
			// Adds the screen, records five seconds, logs health, quits. Run with ZUNIMI_DATA set.
			QTimer::singleShot(1500, &app, [&core] {
				QString err;
				core.addSource("monitor_capture", "Screen", nullptr, ObsCore::Fit::Fill);
				if (!core.startRecording(Settings::dataDir() + "/selftest", &err))
					blog(LOG_ERROR, "selftest: record failed: %s", err.toUtf8().constData());
			});
			auto *probe = new QTimer(&app);
			QObject::connect(probe, &QTimer::timeout, &core, [&core] {
				const Health h = core.health();
				blog(LOG_INFO, "selftest health: fps %.1f cpu %.1f%% ram %.0fMB render %.1f%% encode %.1f%% [%s]",
				     h.fps, h.cpu, h.ramMb, h.renderLag, h.encodeLag, h.text.toUtf8().constData());
			});
			probe->start(1000);
			QTimer::singleShot(7000, &core, [&core] { core.stopRecording(); });
			QObject::connect(&core, &ObsCore::recordingChanged, &app, [&window](bool on, const QString &file) {
				if (!on) {
					blog(LOG_INFO, "selftest: saved %s", file.toUtf8().constData());
					QTimer::singleShot(500, [&window] { window.modes()->quit(); });
				}
			});
		}
		if (app.arguments().contains("--selftest-update")) {
			// Checks the link rules, then asks GitHub for releases and logs what it finds. Run with ZUNIMI_DATA set.
			const bool rules =
				Updater::isReleaseAsset("https://github.com/Deba-dev/zunimi-studio/releases/download/v1.0.0/ZunimiStudio-Setup.exe") &&
				!Updater::isReleaseAsset("https://github.com/someone/else/releases/download/v1/x-Setup.exe") &&
				!Updater::isReleaseAsset("http://github.com/Deba-dev/zunimi-studio/releases/download/v1/x-Setup.exe") &&
				!Updater::isReleaseAsset("https://github.com/Deba-dev/zunimi-studio/releases/download/../../x/Setup.exe") &&
				Updater::parseVersion("v0.10.1") > Updater::parseVersion("0.9.9") &&
				Updater::parseVersion("V1.2") == QList<int>({1, 2, 0, 0});
			blog(rules ? LOG_INFO : LOG_ERROR, "selftest update: link and version rules %s", rules ? "ok" : "FAILED");
			auto *updater = new Updater(&app);
			const auto done = [&window](const QString &what) {
				blog(LOG_INFO, "selftest update: %s", what.toUtf8().constData());
				QTimer::singleShot(200, [&window] { window.modes()->quit(); });
			};
			QObject::connect(updater, &Updater::found, &app, [done](const UpdateInfo &i) {
				done("found " + i.version + " " + i.asset + " " + i.digest);
			});
			QObject::connect(updater, &Updater::nothing, &app, [done] { done("no newer version"); });
			QObject::connect(updater, &Updater::failed, &app, [done](const QString &r) { done("failed: " + r); });
			updater->check(true);
		}
		code = app.exec();
	}
	blog(LOG_INFO, "%s closed", ZUNIMI_NAME);
	base_set_log_handler(nullptr, nullptr);
	if (g_log)
		fclose(g_log);
	return code;
}
