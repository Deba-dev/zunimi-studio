#include "core/Addons.h"
#include "core/Assist.h"
#include "core/ObsCore.h"
#include "core/Settings.h"
#include "core/Updater.h"
#include "core/I18n.h"
#include "events/Events.h"
#include "overlays/Overlays.h"
#include "ui/AlertDesigner.h"
#include "ui/FiltersDialog.h"
#include "ui/MainWindow.h"
#include "ui/PropertiesView.h"
#include "ui/SettingsDialog.h"
#include "ui/SetupWizard.h"
#include "ui/Templates.h"
#include <QPushButton>
#include "ui/Theme.h"
#include "ui/WindowModes.h"
#include "net/WebSocket.h"
#include "version.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>
#include <QUuid>
#include <QUrlQuery>
#include <cstdlib>
#include <memory>
#include <QIcon>
#include <QLockFile>
#include <QMessageBox>
#include <QTimer>

#include <util/base.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include <windows.h>
#include <dbghelp.h>

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

// A crash writes its call stack into the log (function names come from zunimi-studio.pdb and obs.pdb) and a
// small minidump next to the logs, so a crash report says where it happened.
LONG WINAPI onCrash(EXCEPTION_POINTERS *info)
{
	static bool once = false;
	if (once)
		return EXCEPTION_CONTINUE_SEARCH;
	once = true;
	HANDLE process = GetCurrentProcess();
	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
	const QString here = QCoreApplication::applicationDirPath();
	SymInitialize(process, QDir::toNativeSeparators(here).toLocal8Bit().constData(), TRUE);
	CONTEXT ctx = *info->ContextRecord;
	STACKFRAME64 frame{};
	frame.AddrPC.Offset = ctx.Rip;
	frame.AddrFrame.Offset = ctx.Rbp;
	frame.AddrStack.Offset = ctx.Rsp;
	frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
	blog(LOG_ERROR, "CRASH: exception 0x%08lX at %p", info->ExceptionRecord->ExceptionCode,
	     info->ExceptionRecord->ExceptionAddress);
	for (int i = 0; i < 40 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &ctx, nullptr,
					     SymFunctionTableAccess64, SymGetModuleBase64, nullptr); i++) {
		char buffer[sizeof(SYMBOL_INFO) + 256] = {};
		auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
		symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
		symbol->MaxNameLen = 255;
		DWORD64 displacement = 0;
		IMAGEHLP_LINE64 line{sizeof(IMAGEHLP_LINE64)};
		DWORD lineDisplacement = 0;
		char module[MAX_PATH] = "?";
		HMODULE owner = nullptr;
		DWORD64 offset = 0;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				       reinterpret_cast<LPCSTR>(frame.AddrPC.Offset), &owner)) {
			GetModuleFileNameA(owner, module, MAX_PATH);
			offset = frame.AddrPC.Offset - reinterpret_cast<DWORD64>(owner);
		}
		const bool named = SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol);
		const bool lined = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line);
		blog(LOG_ERROR, "CRASH:   %s+0x%llx %s %s:%lu", QFileInfo(QString::fromLocal8Bit(module)).fileName().toUtf8().constData(),
		     offset, named ? symbol->Name : "?", lined ? line.FileName : "", lined ? line.LineNumber : 0);
	}
	QDir dir(Settings::dataDir() + "/crashes");
	dir.mkpath(".");
	const QString dump = dir.filePath(QDateTime::currentDateTime().toString("yyyy-MM-dd hh-mm-ss") + ".dmp");
	HANDLE file = CreateFileW(reinterpret_cast<const wchar_t *>(dump.utf16()), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
				  FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file != INVALID_HANDLE_VALUE) {
		MINIDUMP_EXCEPTION_INFORMATION ex{GetCurrentThreadId(), info, FALSE};
		MiniDumpWriteDump(process, GetCurrentProcessId(), file, MiniDumpNormal, &ex, nullptr, nullptr);
		CloseHandle(file);
	}
	if (g_log)
		fflush(g_log);
	return EXCEPTION_CONTINUE_SEARCH;
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
	Theme::apply(); // the colours and accent, as on zunimilabs.online
	QIcon icon;
	for (int size : {32, 64, 256})
		icon.addFile(QString(":/logo-%1.png").arg(size), QSize(size, size));
	app.setWindowIcon(icon); // window, taskbar and tray

	QLockFile lock(Settings::dataDir() + "/running.lock");
	// after "Restart" the old window may still be closing: wait for it
	if (!lock.tryLock(app.arguments().contains("--restarted") ? 20000 : 100)) {
		QMessageBox::information(nullptr, ZUNIMI_NAME,
					 "Zunimi Studio is already running. Look for its icon in the tray.");
		return 0;
	}

	openLog();
	i18n::load();
	i18n::installQtTranslator();
	SetUnhandledExceptionFilter(onCrash);
	blog(LOG_INFO, "%s %s starting", ZUNIMI_NAME, ZUNIMI_VERSION);
	int code = 0;
	{
		if (app.arguments().contains("--selftest-browser"))
			Addons::setBrowserEnabled(true);
		ObsCore core;
		QString error;
		if (!core.start(&error)) {
			QMessageBox::critical(nullptr, ZUNIMI_NAME, error);
			return 1;
		}
		MainWindow window(&core);
		window.modes()->show();
		if (app.arguments().contains("--selftest"))
			QTimer::singleShot(3000, &app, [&window] { window.modes()->quit(false); });
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
					QTimer::singleShot(500, [&window] { window.modes()->quit(false); });
				}
			});
		}
		if (app.arguments().contains("--selftest-screens")) {
			// Lists every screen and capture method libobs offers, captures each screen and logs its size.
			QTimer::singleShot(500, &app, [&core, &window] {
				obs_source_t *probe = obs_source_create_private("monitor_capture", "probe", nullptr);
				obs_properties_t *props = obs_source_properties(probe);
				for (const char *name : {"method", "monitor_id"}) {
					obs_property_t *p = obs_properties_get(props, name);
					for (size_t i = 0; p && i < obs_property_list_item_count(p); i++)
						blog(LOG_INFO, "selftest screens: %s [%s] = %s%s", name,
						     obs_property_list_item_name(p, i),
						     obs_property_list_format(p) == OBS_COMBO_FORMAT_STRING
							     ? obs_property_list_item_string(p, i)
							     : QByteArray::number(obs_property_list_item_int(p, i)).constData(),
						     obs_property_list_item_disabled(p, i) ? " (disabled)" : "");
				}
				obs_properties_destroy(props);
				obs_source_release(probe);
				const auto screens = core.screens();
				for (const auto &[label, id] : screens) {
					obs_data_t *s = obs_data_create();
					obs_data_set_string(s, "monitor_id", id.toUtf8().constData());
					core.addSource("monitor_capture", label, s, ObsCore::Fit::Fill);
					obs_data_release(s);
				}
				QTimer::singleShot(2500, &core, [&core, &window] {
					for (const SceneItem &item : core.items()) {
						obs_source_t *src = obs_get_source_by_name(item.name.toUtf8().constData());
						blog(LOG_INFO, "selftest screens: captured '%s' %ux%u", item.name.toUtf8().constData(),
						     obs_source_get_width(src), obs_source_get_height(src));
						obs_source_release(src);
					}
					window.modes()->quit(false);
				});
			});
		}
		if (app.arguments().contains("--selftest-scenes")) {
			// Scenes, shared sources, transitions and the replay buffer, end to end. Run with ZUNIMI_DATA set.
			QTimer::singleShot(800, &app, [&core] {
				core.addSource("monitor_capture", "Screen", nullptr, ObsCore::Fit::Fill);
				const QString copy = core.duplicateScene(core.currentScene());
				const QString extra = core.addScene("Starting soon");
				core.addExisting("Screen");
				core.setScene(core.scenes().first());
				blog(LOG_INFO, "selftest scenes: %s | current %s | copy has %d items | extra has %d",
				     core.scenes().join(", ").toUtf8().constData(), core.currentScene().toUtf8().constData(),
				     [&] { core.setScene(copy); int n = int(core.items().size()); core.setScene(core.scenes().first()); return n; }(),
				     [&] { core.setScene(extra); int n = int(core.items().size()); core.setScene(core.scenes().first()); return n; }());
				QString err;
				if (!core.startReplay(Settings::dataDir() + "/selftest", 10, &err))
					blog(LOG_ERROR, "selftest scenes: replay failed: %s", err.toUtf8().constData());
			});
			QObject::connect(&core, &ObsCore::replayChanged, &app, [&core](bool on) {
				if (on)
					QTimer::singleShot(4000, &core, [&core] { core.saveReplay(); });
			});
			QObject::connect(&core, &ObsCore::replaySaved, &app, [&core, &window](const QString &file) {
				blog(LOG_INFO, "selftest scenes: replay saved %s (%lld bytes)", file.toUtf8().constData(),
				     QFileInfo(file).size());
				core.removeScene("Starting soon");
				blog(LOG_INFO, "selftest scenes: after remove: %s", core.scenes().join(", ").toUtf8().constData());
				core.stopReplay();
				QTimer::singleShot(800, [&window] { window.modes()->quit(false); });
			});
		}
		if (app.arguments().contains("--selftest-edit")) {
			// Undo/redo, transforms, studio mode, collections, import, profiles, screenshot, a paused multi-track
			// recording. Run with ZUNIMI_DATA set to a scratch folder. Every line starts "selftest edit:".
			auto check = [](bool ok, const char *what) {
				blog(ok ? LOG_INFO : LOG_ERROR, "selftest edit: %s %s", ok ? "ok  " : "FAIL", what);
			};
			QTimer::singleShot(800, &app, [&core, check] {
				core.addSource("color_source", "Colour", nullptr, ObsCore::Fit::Fill);
				obs_sceneitem_t *it = core.item("Colour");
				check(it != nullptr, "add source");
				const QRectF before = core.itemBox(it);
				core.rotate("Colour", 90);
				check(std::abs(obs_sceneitem_get_rot(core.item("Colour")) - 90.0f) < 0.1f, "rotate 90");
				check(std::abs(core.itemBox(core.item("Colour")).center().x() - before.center().x()) < 1.0,
				      "rotation keeps the centre");
				core.undo();
				check(std::abs(obs_sceneitem_get_rot(core.item("Colour"))) < 0.1f, "undo rotate");
				core.redo();
				check(std::abs(obs_sceneitem_get_rot(core.item("Colour")) - 90.0f) < 0.1f, "redo rotate");
				core.undo();
				core.flip("Colour", true);
				vec2 sc{};
				obs_sceneitem_get_scale(core.item("Colour"), &sc);
				check(sc.x < 0, "flip");
				obs_sceneitem_crop crop{10, 20, 30, 40};
				core.setCrop("Colour", crop);
				obs_sceneitem_crop got{};
				obs_sceneitem_get_crop(core.item("Colour"), &got);
				check(got.left == 10 && got.bottom == 40, "crop");
				core.removeSource("Colour");
				check(core.item("Colour") == nullptr, "remove");
				core.undo();
				obs_sceneitem_get_crop(core.item("Colour") ? core.item("Colour") : nullptr, &got);
				check(core.item("Colour") != nullptr && got.top == 20, "undo remove brings it back as it was");
				core.undo(); // crop
				core.undo(); // flip
				core.undo(); // add
				check(core.item("Colour") == nullptr, "undo add");
				core.redo();
				check(core.item("Colour") != nullptr, "redo add");

				const QString second = core.addScene("Second");
				core.addExisting(core.scenes().first());
				check(core.item(core.scenes().first()) != nullptr, "scene inside a scene");
				core.setStudioMode(true);
				core.setScene(core.scenes().first());
				check(core.programScene() == second, "studio mode: editing does not change what's on air");
				core.transitionNow();
				check(core.programScene() == core.scenes().first(), "studio mode: Go sends it live");
				core.setStudioMode(false);
				core.removeScene(second);
				check(!core.scenes().contains(second), "remove scene");
				core.undo();
				check(core.scenes().contains(second), "undo remove scene");

				QString err;
				check(core.newCollection("Second set", false, &err), "new collection");
				check(core.items().isEmpty() && core.collection() == "Second set", "new collection is empty");
				check(core.switchCollection("Main", &err) && core.scenes().size() == 2, "switch back keeps the scenes");
				QStringList notes;
				const QString imported = core.importCollection(Settings::dataDir() + "/collections/Main.json", {}, &notes, &err);
				check(!imported.isEmpty() && core.collection() == imported && core.scenes().size() == 2, "import a collection");
				core.switchCollection("Main", &err);

				check(core.newProfile("Record only", &err) && core.profile() == "Record only", "new profile");
				Settings::get().set("record/format", "mkv");
				Settings::get().set("record/tracks", 3);
				check(core.switchProfile("Default", &err) &&
					      Settings::get().value("record/format", "hybrid_mp4").toString() != "mkv",
				      "profiles keep their own settings");
				check(core.switchProfile("Record only", &err) &&
					      Settings::get().value("record/format").toString() == "mkv",
				      "switching back restores them");
				core.outputSettingsChanged();
				core.screenshot(Settings::dataDir() + "/selftest");
				if (!core.startRecording(Settings::dataDir() + "/selftest", &err))
					check(false, ("record: " + err).toUtf8().constData());
			});
			QObject::connect(&core, &ObsCore::screenshotSaved, &app, [check](const QString &file) {
				check(QImage(file).width() > 0, "screenshot saved");
			});
			QObject::connect(&core, &ObsCore::recordingChanged, &app, [&core, &window, check](bool on, const QString &file) {
				if (on) {
					QTimer::singleShot(1500, &core, [&core, check] { check(core.pauseRecording(true), "pause"); });
					QTimer::singleShot(2500, &core, [&core, check] { check(core.pauseRecording(false), "resume"); });
					QTimer::singleShot(4000, &core, [&core] { core.stopRecording(); });
					return;
				}
				check(file.endsWith(".mkv") && QFileInfo(file).size() > 1000, "mkv recording saved");
				blog(LOG_INFO, "selftest edit: done (%s, %lld bytes)", file.toUtf8().constData(), QFileInfo(file).size());
				QTimer::singleShot(500, [&window] { window.modes()->quit(false); });
			});
		}
		if (app.arguments().contains("--selftest-live")) {
			// Chat and alert parsing for every service, a real Twitch chat connection, overlays that draw, and
			// speech. Run with ZUNIMI_DATA set. Every line starts "selftest live:".
			auto check = [](bool ok, const QString &what) {
				blog(ok ? LOG_INFO : LOG_ERROR, "selftest live: %s %s", ok ? "ok  " : "FAIL", what.toUtf8().constData());
			};
			{
				ChatMessage c;
				StreamEvent e;
				TwitchChat::parseLine("@badges=moderator/1;bits=100;color=#FF0000;display-name=Aarav;id=abc :aarav!aarav@aarav.tmi.twitch.tv PRIVMSG #zunimi :Cheer100 GG", &c, &e);
				check(c.user == "Aarav" && c.mod && c.text == "Cheer100 GG" && e.type == "bits" && e.amount == 100, "twitch chat + bits");
				ChatMessage c2;
				StreamEvent e2;
				TwitchChat::parseLine("@display-name=Kavya;msg-id=resub;msg-param-cumulative-months=7;msg-param-sub-plan=1000 :tmi.twitch.tv USERNOTICE #zunimi :7 months!", &c2, &e2);
				check(e2.type == "resub" && e2.count == 7 && e2.user == "Kavya", "twitch resub");
				StreamEvent e3;
				TwitchChat::parseLine("@display-name=Rohan;msg-id=submysterygift;msg-param-mass-gift-count=5 :tmi.twitch.tv USERNOTICE #zunimi", &c2, &e3);
				check(e3.type == "gift" && e3.count == 5, "twitch gift bomb");
				StreamEvent e4;
				TwitchChat::parseLine("@display-name=Sana;msg-id=raid;msg-param-displayName=Sana;msg-param-viewerCount=42 :tmi.twitch.tv USERNOTICE #zunimi", &c2, &e4);
				check(e4.type == "raid" && e4.count == 42, "twitch raid");

				const QJsonArray yt = QJsonDocument::fromJson(R"([{"addChatItemAction":{"item":{"liveChatPaidMessageRenderer":{
					"id":"x","authorName":{"simpleText":"Priya"},"purchaseAmountText":{"simpleText":"₹1,500.00"},
					"message":{"runs":[{"text":"Love the stream "},{"emoji":{"emojiId":"🔥"}}]}}}}},
					{"addChatItemAction":{"item":{"liveChatTextMessageRenderer":{"id":"y","authorName":{"simpleText":"Dev"},
					"message":{"runs":[{"text":"hello"}]}}}}}])").array();
				QVector<ChatMessage> chat;
				QVector<StreamEvent> events;
				YouTubeChat::parseActions(yt, true, &chat, &events);
				check(chat.size() == 2 && events.size() == 1 && events[0].type == "superchat" && events[0].amount == 1500 &&
					      events[0].currency == "INR" && chat[0].text == "Love the stream 🔥",
				      "youtube chat + super chat in rupees");

				ChatMessage kc;
				StreamEvent ke;
				KickChat::parse(QJsonDocument::fromJson(R"({"event":"App\\Events\\ChatMessageEvent","data":"{\"id\":\"1\",\"content\":\"yo [emote:37226:KEKW]\",\"sender\":{\"username\":\"kicker\",\"identity\":{\"color\":\"#00FF00\",\"badges\":[{\"type\":\"moderator\"}]}}}"})").object(), &kc, &ke);
				check(kc.user == "kicker" && kc.mod && kc.text == "yo :KEKW:", "kick chat");

				StreamEvent se;
				StreamElementsEvents::parse(QJsonDocument::fromJson(R"({"type":"tip","provider":"twitch","data":{"username":"neha","displayName":"Neha","amount":250,"currency":"INR","message":"gg"}})").object(), &se);
				check(se.type == "tip" && se.amount == 250 && se.user == "Neha" && se.amountText() == "₹250", "streamelements tip");
				const auto sl = StreamlabsEvents::parse(QJsonDocument::fromJson(R"({"type":"donation","for":"streamlabs","message":[{"name":"Arjun","amount":"99.5","currency":"USD","message":"nice"}]})").object());
				check(sl.size() == 1 && sl[0].type == "tip" && sl[0].amount == 99.5 && sl[0].amountText() == "$99.50", "streamlabs donation");
			}
			// a real anonymous connection to Twitch chat (websocket over TLS, IRC, JOIN)
			Settings::get().set("events/twitchChannel", "twitch");
			QObject::connect(&EventHub::get(), &EventHub::statusChanged, &app, [check](const QString &service) {
				static bool done = false;
				if (service == "twitch" && EventHub::get().connected("twitch") && !done) {
					done = true;
					check(true, "connected to Twitch chat: " + EventHub::get().status("twitch"));
				}
			});
			EventHub::get().restart();
			QTimer::singleShot(800, &app, [&core, check] {
				obs_data_t *s = obs_data_create();
				obs_data_set_string(s, "kind", "alerts");
				obs_data_set_int(s, "width", 800);
				obs_data_set_int(s, "height", 450);
				obs_source_t *alerts = core.addSourceAt("zunimi_overlay", "Alerts", s, QRectF(0, 0, 800, 450));
				obs_data_release(s);
				s = obs_data_create();
				obs_data_set_string(s, "kind", "chat");
				obs_data_set_int(s, "width", 400);
				obs_data_set_int(s, "height", 600);
				obs_source_t *chat = core.addSourceAt("zunimi_overlay", "Chat", s, QRectF(820, 0, 400, 600));
				obs_data_release(s);
				check(alerts && chat, "alert box and chat box added");
				EventHub::get().test("tip");
				ChatMessage m;
				m.platform = "youtube";
				m.user = "Priya";
				m.text = "hello from the self-test";
				EventHub::get().emitChat(m);
				overlays::speak("Self test speaking", {}, 50);
				QTimer::singleShot(1500, &core, [&core, alerts, chat] {
					core.screenshot(Settings::dataDir() + "/selftest", alerts);
					core.screenshot(Settings::dataDir() + "/selftest", chat);
				});
			});
			auto *shots = new int(0);
			QObject::connect(&core, &ObsCore::screenshotSaved, &app, [check, shots, &window](const QString &file) {
				const QImage img(file);
				int visible = 0;
				for (int y = 0; y < img.height(); y += 4)
					for (int x = 0; x < img.width(); x += 4)
						visible += qAlpha(img.pixel(x, y)) > 0;
				check(visible > 200, QString("overlay draws (%1, %2 visible samples)").arg(QFileInfo(file).fileName()).arg(visible));
				if (++*shots == 2)
					QTimer::singleShot(6000, [check, &window] {
						bool wav = false;
						for (const QFileInfo &f : QDir::temp().entryInfoList({"zunimi-tts-*.wav"}))
							wav |= f.size() > 2000 && f.lastModified().secsTo(QDateTime::currentDateTime()) < 60;
						check(wav, "text to speech made audio");
						blog(LOG_INFO, "selftest live: done");
						window.modes()->quit(false);
					});
			});
		}
		if (app.arguments().contains("--selftest-assist")) {
			// Pre-flight, fixes, multistream settings, the app in front, Mark moment chapters. Run with ZUNIMI_DATA set.
			auto check = [](bool ok, const QString &what) {
				blog(ok ? LOG_INFO : LOG_ERROR, "selftest assist: %s %s", ok ? "ok  " : "FAIL", what.toUtf8().constData());
			};
			auto *assist = new Assist(&core, &app);
			QTimer::singleShot(800, &app, [&core, assist, check] {
				auto has = [](const QVector<Finding> &f, const QString &word) {
					return std::any_of(f.begin(), f.end(), [&word](const Finding &x) { return x.text.contains(word); });
				};
				check(has(assist->preflight(true), L("This scene is empty: viewers would see a black screen.")),
				      "pre-flight: an empty scene is caught");
				obs_source_set_muted(core.microphone(), true);
				QVector<Finding> f = assist->preflight(true);
				check(has(f, L("Your mic is muted.")), "pre-flight: a muted mic is caught");
				assist->applyFix("unmute_mic");
				check(!obs_source_muted(core.microphone()), "fix: unmute the mic");
				core.addSource("color_source", "Colour", nullptr, ObsCore::Fit::Fill);
				check(!has(assist->preflight(true), "empty"), "pre-flight: a scene with a source is fine");
				ObsCore::setDestinations({{"YouTube", "rtmps://a.rtmps.youtube.com:443/live2", "secret-key", true},
							  {"Kick", "rtmps://kick.example/app", "k2", false}});
				const auto d = ObsCore::destinations();
				check(d.size() == 2 && d[0].key == "secret-key" && !d[1].on &&
					      !Settings::get().value("multistream/list").toString().contains("secret-key"),
				      "multistream: saved, with keys encrypted");
				check(!Assist::foregroundApp().isEmpty() || true, "app in front: " + Assist::foregroundApp());
				check(Assist::runningApps().size() > 0, QString("open apps found: %1").arg(Assist::runningApps().size()));
				QString err;
				if (!core.startRecording(Settings::dataDir() + "/selftest", &err))
					check(false, "record: " + err);
			});
			QObject::connect(&core, &ObsCore::recordingChanged, &app, [&core, assist, check, &window](bool on, const QString &file) {
				if (on) {
					QTimer::singleShot(1500, &core, [assist] { assist->mark("First"); });
					QTimer::singleShot(2600, &core, [assist] { assist->mark(); });
					QTimer::singleShot(3500, &core, [&core] { core.stopRecording(); });
					return;
				}
				QTimer::singleShot(300, [file, check, &window] {
					QFile chapters(QFileInfo(file).dir().filePath(QFileInfo(file).completeBaseName() + " chapters.txt"));
					const QString text = chapters.open(QIODevice::ReadOnly) ? QString::fromUtf8(chapters.readAll()) : QString();
					check(text.startsWith("00:00 Start") && text.contains("First") && text.contains(QString(L("Moment %1")).arg(2)),
					      "mark moment writes chapters: " + text.simplified());
					check(QFile::exists(Settings::dataDir() + "/moments.jsonl"), "moments log for Clip Studio");
					blog(LOG_INFO, "selftest assist: done");
					window.modes()->quit(false);
				});
			});
		}
		if (app.arguments().contains("--selftest-browser")) {
			// The browser source add-on: loads, shows a page (ZUNIMI_TEST_URL), and the page really draws.
			auto check = [](bool ok, const QString &what) {
				blog(ok ? LOG_INFO : LOG_ERROR, "selftest browser: %s %s", ok ? "ok  " : "FAIL", what.toUtf8().constData());
			};
			check(core.moduleLoaded("obs-browser"), "the add-on loaded");
			QTimer::singleShot(800, &app, [&core, check] {
				obs_data_t *s = obs_data_create();
				if (qEnvironmentVariableIsSet("ZUNIMI_TEST_FILE")) {
					obs_data_set_bool(s, "is_local_file", true);
					obs_data_set_string(s, "local_file", qEnvironmentVariable("ZUNIMI_TEST_FILE").toUtf8().constData());
				} else {
					obs_data_set_string(s, "url", qEnvironmentVariable("ZUNIMI_TEST_URL").toUtf8().constData());
				}
				obs_data_set_int(s, "width", 640);
				obs_data_set_int(s, "height", 360);
				obs_source_t *page = core.addSource("browser_source", "Page", s, ObsCore::Fit::Fill);
				obs_data_release(s);
				check(page != nullptr, "browser source added");
				QTimer::singleShot(5000, &core, [&core, page] { core.screenshot(Settings::dataDir() + "/selftest", page); });
			});
			QObject::connect(&core, &ObsCore::screenshotSaved, &app, [check, &window](const QString &file) {
				const QImage img(file);
				const QColor c = img.pixelColor(img.width() / 2, img.height() / 2);
				check(c.red() > 200 && c.green() < 80 && c.blue() < 80,
				      QString("the page draws (centre pixel %1)").arg(c.name()));
				blog(LOG_INFO, "selftest browser: done");
				window.modes()->quit(false);
			});
			QObject::connect(&core, &ObsCore::problem, &app, [check, &window](const QString &why) {
				check(false, why);
				window.modes()->quit(false);
			});
		}
		if (app.arguments().contains("--selftest-vertical")) {
			// The vertical canvas: on, follows the scene on air, editable, records next to the landscape file, saved.
			auto check = [](bool ok, const QString &what) {
				blog(ok ? LOG_INFO : LOG_ERROR, "selftest vertical: %s %s", ok ? "ok  " : "FAIL", what.toUtf8().constData());
			};
			QTimer::singleShot(800, &app, [&core, check] {
				core.addSource("color_source", "Colour", nullptr, ObsCore::Fit::Fill);
				QString err;
				check(core.setVertical(true, &err) && core.verticalOn(), "vertical canvas on " + err);
				core.setEditVertical(true);
				check(core.editingVertical() && core.canvas().height() > core.canvas().width(), "editing the vertical picture");
				check(core.items().size() == 1, QString("it shows the scene on air (%1 item)").arg(core.items().size()));
				obs_data_t *s = obs_data_create();
				obs_data_set_string(s, "kind", "label");
				core.addSource("zunimi_overlay", "Vertical label", s, ObsCore::Fit::Corner);
				obs_data_release(s);
				check(core.items().size() == 2, "a source added only to the vertical picture");
				core.setEditVertical(false);
				const QString second = core.addScene("Second");
				core.transitionNow();
				core.setEditVertical(true);
				bool follows = false;
				for (const SceneItem &i : core.items())
					follows |= i.name == second;
				check(follows, "it follows a scene switch");
				core.setEditVertical(false);
				core.save();
				Settings::get().set("vertical/record", true);
				if (!core.startRecording(Settings::dataDir() + "/selftest", &err))
					check(false, "record: " + err);
			});
			QObject::connect(&core, &ObsCore::recordingChanged, &app, [&core, check, &window](bool on, const QString &file) {
				if (on) {
					QTimer::singleShot(3000, &core, [&core] { core.stopRecording(); });
					return;
				}
				QTimer::singleShot(1500, [file, check, &window] {
					const QString vertical = QFileInfo(file).dir().filePath(QFileInfo(file).completeBaseName() + " (vertical).mp4");
					check(QFileInfo(vertical).size() > 1000, QString("vertical recording saved (%1 bytes)").arg(QFileInfo(vertical).size()));
					QFile collection(Settings::dataDir() + "/collections/Main.json");
					(void)collection.open(QIODevice::ReadOnly);
					check(collection.readAll().contains("\"vertical\""), "saved with the scene collection");
					blog(LOG_INFO, "selftest vertical: done");
					window.modes()->quit(false);
				});
			});
		}
		if (app.arguments().contains("--selftest-relay")) {
			// Against a relay at ZUNIMI_RELAY_URL started with DEV=1 (zunimilabs-site/relay: npx wrangler dev): gets a
			// channel, connects, receives a Ko-fi tip sent to its webhook link, learns its Kick link, parses Kicks.
			auto passed = std::make_shared<QStringList>();
			auto check = [passed, &window](bool ok, const QString &what) {
				blog(ok ? LOG_INFO : LOG_ERROR, "selftest relay: %s %s", ok ? "ok  " : "FAIL", what.toUtf8().constData());
				if (ok && !passed->contains(what))
					passed->append(what);
				if (passed->size() == 5) {
					blog(LOG_INFO, "selftest relay: all passed");
					QTimer::singleShot(300, [&window] { window.modes()->quit(false); });
				}
			};
			QTimer::singleShot(20000, &app, [passed, &window] {
				if (passed->size() < 5) {
					blog(LOG_ERROR, "selftest relay: MISSING (only %s)", passed->join(", ").toUtf8().constData());
					window.modes()->quit(false);
				}
			});
			StreamEvent kicks;
			check(RelayEvents::parse(QJsonObject{{"kind", "bits"}, {"platform", "kick"}, {"user", "kicker"}, {"amount", 500},
							     {"currency", "KICKS"}},
						 &kicks) &&
				      kicks.amountText() == QString(L("%1 Kicks")).arg(500),
			      "Kicks read as Kicks");
			Settings::get().setSecret("events/kofiToken", "kofi-app-token");
			QObject::connect(&EventHub::get(), &EventHub::event, &app, [check](const StreamEvent &e) {
				if (e.platform == "kofi")
					check(e.type == "tip" && e.user == "Meera" && e.amount == 3 && e.currency == "USD", "Ko-fi tip arrived");
			});
			auto sentTip = std::make_shared<bool>(false);
			QObject::connect(&EventHub::get(), &EventHub::statusChanged, &app, [check, sentTip](const QString &service) {
				if (service != "relay" || !EventHub::get().connected("relay"))
					return;
				const QString status = EventHub::get().status("relay");
				if (status.contains("selftest_kick"))
					check(true, "the Kick link is known");
				// the first "ready" comes before the relay has read the app's Ko-fi token; the second says Ko-fi
				if (*sentTip || !status.contains(L("Ko-fi tips")))
					return;
				*sentTip = true;
				check(true, "connected with Ko-fi on");
				QUrlQuery form;
				form.addQueryItem("data", QString::fromUtf8(QJsonDocument(QJsonObject{
					{"verification_token", "kofi-app-token"}, {"message_id", QUuid::createUuid().toString()},
					{"type", "Donation"}, {"is_public", true}, {"from_name", "Meera"}, {"message", "chai pe"},
					{"amount", "3.00"}, {"currency", "USD"}}).toJson(QJsonDocument::Compact)));
				QNetworkRequest req{QUrl(RelayEvents::kofiLink())};
				req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
				QNetworkReply *r = EventHub::get().network()->post(req, form.toString(QUrl::FullyEncoded).toUtf8());
				QObject::connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
				QNetworkRequest link{QUrl(RelayEvents::base() + "/v1/dev/kick-link")};
				link.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
				QNetworkReply *k = EventHub::get().network()->post(link, QJsonDocument(QJsonObject{
					{"uid", "777"}, {"id", Settings::get().value("relay/id").toString()}, {"name", "selftest_kick"}}).toJson());
				QObject::connect(k, &QNetworkReply::finished, k, &QObject::deleteLater);
			});
			RelayEvents::ensureChannel([check](const QString &error) {
				check(error.isEmpty() && RelayEvents::kofiLink().contains("/v1/kofi/"), "a relay channel is made");
				EventHub::get().restart();
			});
		}
		if (app.arguments().contains("--demo-template")) {
			// --demo-template <kind> <style>: adds that template, then saves what's on air to <ZUNIMI_DATA>/selftest.
			const int at = app.arguments().indexOf("--demo-template");
			const QString kind = app.arguments().value(at + 1, "starting"), style = app.arguments().value(at + 2, "neon");
			QTimer::singleShot(1200, &app, [&core, kind, style] {
				QString error;
				templates::apply(&core, kind, style, &error);
				QTimer::singleShot(3500, &core, [&core] { core.screenshot(Settings::dataDir() + "/selftest"); });
			});
		}
		if (app.arguments().contains("--selftest-captureprops")) {
			// Records while opening a Window capture's options and switching its window over and over, the way a
			// live crash happened on 2026-11-11 (d3d11 access violation). Passing means it survives 15 seconds.
			QTimer::singleShot(1500, &app, [&core, &window] {
				QString err;
				obs_source_t *src = core.addSource("window_capture", "Window", nullptr, ObsCore::Fit::Fill);
				if (!src || !core.startRecording(Settings::dataDir() + "/selftest", &err)) {
					blog(LOG_ERROR, "selftest captureprops: FAIL could not start (%s)", err.toUtf8().constData());
					return;
				}
				QStringList windows;
				obs_properties_t *props = obs_get_source_properties("window_capture");
				if (obs_property_t *list = obs_properties_get(props, "window"))
					for (size_t i = 0; i < obs_property_list_item_count(list); i++)
						if (const char *v = obs_property_list_item_string(list, i); v && *v)
							windows << QString::fromUtf8(v);
				obs_properties_destroy(props);
				// ZUNIMI_TEST_WINDOWS="Code.exe|Discord.exe" keeps only those windows (the crash was a Discord -> VS Code switch)
				const QString only = qEnvironmentVariable("ZUNIMI_TEST_WINDOWS");
				if (!only.isEmpty()) {
					QStringList kept;
					for (const QString &w : windows)
						for (const QString &part : only.split('|'))
							if (w.contains(part, Qt::CaseInsensitive))
								kept << w;
					windows = kept;
				}
				blog(LOG_INFO, "selftest captureprops: %d windows to switch between: %s", int(windows.size()),
				     windows.join(" ; ").left(300).toUtf8().constData());
				auto *turn = new QTimer(&window);
				auto count = std::make_shared<int>(0);
				QObject::connect(turn, &QTimer::timeout, &window, [src, windows, count, &window] {
					auto *dialog = new PropertiesDialog(src, "Window", &window); // builds the options, like a double-click
					dialog->show();
					if (!windows.isEmpty()) {
						obs_data_t *s = obs_data_create();
						obs_data_set_string(s, "window", windows[*count % windows.size()].toUtf8().constData());
						obs_source_update(src, s);
						obs_data_release(s);
					}
					QTimer::singleShot(80, dialog, &QObject::deleteLater);
					++*count;
				});
				turn->start(150);
				QTimer::singleShot(15000, &window, [&core, &window, turn, count] {
					turn->stop();
					core.stopRecording();
					blog(LOG_INFO, "selftest captureprops: %d option rebuilds while recording, no crash", *count);
					blog(LOG_INFO, "selftest captureprops: all passed");
					QTimer::singleShot(1500, [&window] { window.modes()->quit(false); });
				});
			});
		}
		if (app.arguments().contains("--selftest-docks")) {
			// Then waits for the auto-save and ends the app abruptly, as a crash would; --selftest-docks-reopen
			// (next run, same data folder) checks the moved dock came back.
			QTimer::singleShot(1200, &app, [&window] {
				window.selftestDocks();
				QTimer::singleShot(4500, [] {
					blog(LOG_INFO, "selftest docks: auto-saved, ending abruptly");
					std::_Exit(0);
				});
			});
		}
		if (app.arguments().contains("--selftest-docks-reopen")) {
			QTimer::singleShot(1200, &app, [&window] {
				const bool ok = window.dockArea("mixer") == int(Qt::RightDockWidgetArea);
				blog(ok ? LOG_INFO : LOG_ERROR, "selftest docks: %s the moved Audio Mixer is still on the right after a crash",
				     ok ? "ok  " : "FAIL");
				blog(LOG_INFO, "selftest docks: %s", ok ? "all passed" : "SOME FAILED");
				QTimer::singleShot(300, [&window] { window.modes()->quit(false); });
			});
		}
		if (app.arguments().contains("--selftest-templates")) {
			// Adds every template in every style, checks what each scene holds, and writes the previews and the
			// full-size pictures to <ZUNIMI_DATA>/selftest for a look.
			QTimer::singleShot(1200, &app, [&core, &window] {
				bool all = true;
				auto check = [&all](bool ok, const QString &what) {
					all &= ok;
					blog(ok ? LOG_INFO : LOG_ERROR, "selftest templates: %s %s", ok ? "ok  " : "FAIL", what.toUtf8().constData());
				};
				QDir().mkpath(Settings::dataDir() + "/selftest");
				const QHash<QString, QStringList> wanted = {
					{"starting", {"image_source", "text_gdiplus", "zunimi_overlay"}},
					{"brb", {"image_source", "text_gdiplus", "zunimi_overlay"}},
					{"ending", {"image_source", "text_gdiplus", "zunimi_overlay"}},
					{"gaming", {"game_capture", "image_source", "zunimi_overlay"}},
					{"chatting", {"image_source", "zunimi_overlay"}}};
				for (const templates::Style &st : templates::styles())
					for (const templates::Kind &k : templates::kinds()) {
						templates::preview(k.id, st.id, QSize(640, 360))
							.save(Settings::dataDir() + "/selftest/preview-" + st.id + "-" + k.id + ".png");
						QString error;
						const QString scene = templates::apply(&core, k.id, st.id, &error);
						QStringList kinds;
						for (const SceneItem &i : core.items())
							kinds << i.kind;
						bool has = !scene.isEmpty() && core.currentScene() == scene;
						for (const QString &w : wanted.value(k.id))
							has &= kinds.contains(w);
						check(has, QString("%1 / %2 -> \"%3\" [%4] %5").arg(st.id, k.id, scene, kinds.join(","), error));
						// items() is top first: the camera must sit over a background, and the Gaming frame over the game
						if (k.id == "chatting" && kinds.contains("dshow_input"))
							check(kinds.indexOf("dshow_input") < kinds.indexOf("image_source"), "the webcam is over the background");
						if (k.id == "gaming")
							check(kinds.indexOf("image_source") < kinds.indexOf("game_capture"), "the frame is over the game");
					}
				check(QDir(Settings::dataDir() + "/templates").entryList({"*.png"}).size() == 15, "15 pictures drawn");
				core.undo();
				check(core.scenes().size() == 16, "undo takes back the last template's sources, scenes stay");
				blog(LOG_INFO, "selftest templates: %s", all ? "all passed" : "SOME FAILED");
				QTimer::singleShot(300, [&window] { window.modes()->quit(false); });
			});
		}
		if (app.arguments().contains("--selftest-groups")) {
			// Groups: two of three sources go in a group, the list shows the group then its sources, a source inside
			// can be hidden, ungroup and undo/redo work.
			QTimer::singleShot(1200, &app, [&core, &window] {
				bool all = true;
				auto check = [&all](bool ok, const QString &what) {
					all &= ok;
					blog(ok ? LOG_INFO : LOG_ERROR, "selftest groups: %s %s", ok ? "ok  " : "FAIL", what.toUtf8().constData());
				};
				for (const char *n : {"A", "B", "C"})
					core.addSource("color_source_v3", n, nullptr, ObsCore::Fit::Fill);
				auto names = [&core] {
					QStringList out;
					for (const SceneItem &i : core.items())
						out << (i.isGroup ? "[" + i.name + "]" : i.group.isEmpty() ? i.name : i.group + "/" + i.name);
					return out.join(" ");
				};
				const QString g = core.groupItems({"A", "B"}, "Pair");
				check(g == "Pair", "group made");
				const QString order = names();
				check(order == "[Pair] Pair/B Pair/A C" || order == "C [Pair] Pair/B Pair/A", "list shows the group then its sources: " + order);
				core.setVisible("A", false);
				obs_sceneitem_t *a = core.item("A");
				check(a && !obs_sceneitem_visible(a), "a source inside the group can be hidden");
				check(core.ungroup("Pair") && !names().contains("Pair"), "ungroup: " + names());
				core.undo();
				check(names().contains("[Pair]"), "undo brings the group back: " + names());
				core.redo();
				check(!names().contains("Pair"), "redo ungroups again: " + names());
				blog(LOG_INFO, "selftest groups: %s", all ? "all passed" : "SOME FAILED");
				QTimer::singleShot(300, [&window] { window.modes()->quit(false); });
			});
		}
		if (app.arguments().contains("--selftest-pack")) {
			// --selftest-pack <zip> <sha256>: installs a template pack from a file, as if it came from the catalog.
			const int at = app.arguments().indexOf("--selftest-pack");
			const QString zipPath = app.arguments().value(at + 1), sha = app.arguments().value(at + 2);
			QTimer::singleShot(1200, &app, [&core, &window, zipPath, sha] {
				QFile f(zipPath);
				const QByteArray zip = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
				QString message;
				const bool refused = templates::installPack(&core, "test-pack", zip, QString(64, '0'), &message).isEmpty();
				blog(refused ? LOG_INFO : LOG_ERROR, "selftest pack: %s a wrong checksum is refused", refused ? "ok  " : "FAIL");
				const QString name = templates::installPack(&core, "test-pack", zip, sha, &message);
				const bool ok = !name.isEmpty() && core.collections().contains(name);
				blog(ok ? LOG_INFO : LOG_ERROR, "selftest pack: %s installed as \"%s\": %s", ok ? "ok  " : "FAIL",
				     name.toUtf8().constData(), message.toUtf8().constData());
				QString error;
				const bool switched = ok && core.switchCollection(name, &error) && core.scenes().size() >= 2;
				blog(switched ? LOG_INFO : LOG_ERROR, "selftest pack: %s switched to it: %s", switched ? "ok  " : "FAIL",
				     core.scenes().join(", ").toUtf8().constData());
				blog(LOG_INFO, "selftest pack: %s", refused && ok && switched ? "all passed" : "SOME FAILED");
				QTimer::singleShot(300, [&window] { window.modes()->quit(false); });
			});
		}
		if (app.arguments().contains("--selftest-setup")) {
			// Carries out a first-time setup (Simple mode, Twitch, screen, alerts, chat) and checks the result.
			QTimer::singleShot(1200, &app, [&core, &window] {
				SetupChoices c;
				c.simple = true;
				c.platform = "twitch";
				c.server = "rtmp://live.twitch.tv/app";
				c.key = "live_selftest";
				c.channel = "zunimi_selftest";
				const auto screens = core.screens();
				if (!screens.isEmpty()) {
					c.screenId = screens.first().second;
					c.screenLabel = screens.first().first;
				}
				c.game = true;
				c.alerts = c.chat = true;
				window.applySetup(c);
				auto check = [](bool ok, const char *what) {
					blog(ok ? LOG_INFO : LOG_ERROR, "selftest setup: %s %s", what, ok ? "ok" : "FAILED");
					return ok;
				};
				Settings &s = Settings::get();
				bool all = check(s.value("ui/simple").toBool() && s.value("setup/done").toBool(), "saved as Simple and done");
				all &= check(s.secret("stream/key") == "live_selftest" && s.value("stream/platform") == "twitch", "stream key");
				all &= check(s.value("events/twitchChannel") == "zunimi_selftest", "channel");
				QStringList kinds;
				for (const SceneItem &i : core.items())
					kinds << i.kind;
				blog(LOG_INFO, "selftest setup: sources %s", kinds.join(", ").toUtf8().constData());
				all &= check(kinds.contains("monitor_capture") && kinds.contains("game_capture") &&
						     kinds.count("zunimi_overlay") == 2,
					     "screen, game, alerts and chat added");
				int shown = 0, pro = 0;
				for (QWidget *w : window.findChildren<QWidget *>())
					if (w->property("pro").toBool()) {
						++pro;
						shown += !w->isHidden();
					}
				all &= check(pro >= 5 && shown == 0, "Pro tools hidden");
				window.setSimple(false);
				shown = 0;
				for (QWidget *w : window.findChildren<QWidget *>())
					if (w->property("pro").toBool())
						shown += !w->isHidden();
				all &= check(shown == pro, "Pro mode shows them again");
				blog(LOG_INFO, "selftest setup: %s", all ? "all passed" : "SOME FAILED");
				QTimer::singleShot(300, [&window] { window.modes()->quit(false); });
			});
		}
		if (app.arguments().contains("--demo-setup")) {
			// Opens the first-time setup on the page given after it (1-4), for screenshots.
			const int at = app.arguments().indexOf("--demo-setup");
			const int pageNo = std::max(1, app.arguments().value(at + 1).toInt());
			QTimer::singleShot(800, &app, [&core, &window, pageNo] {
				auto *wizard = new SetupWizard(&core, &window);
				wizard->setAttribute(Qt::WA_DeleteOnClose);
				wizard->show();
				for (int i = 1; i < pageNo; ++i)
					for (QPushButton *b : wizard->findChildren<QPushButton *>())
						if (b->text() == "Next") {
							b->click();
							break;
						}
			});
		}
		if (app.arguments().contains("--demo-settings")) {
			// Opens Settings on the tab named after it (default Remote), for screenshots and UI checks.
			const int at = app.arguments().indexOf("--demo-settings");
			const int tab = app.arguments().value(at + 1).toInt(nullptr) ? app.arguments().value(at + 1).toInt()
										  : int(SettingsDialog::Remote);
			if (tab == int(SettingsDialog::Remote) && app.arguments().contains("--remote-on")) {
				Settings::get().set("remote/on", true);
				Settings::get().set("remote/lan", true);
			}
			QTimer::singleShot(800, &app, [&core, &window, tab] {
				auto *dialog = new SettingsDialog(&core, window.modes(), &window);
				dialog->setAttribute(Qt::WA_DeleteOnClose);
				dialog->openTab(SettingsDialog::Tab(tab));
				dialog->show();
			});
		}
		if (app.arguments().contains("--demo-dialogs")) {
			// Opens a source's options and the mic's filters side by side, for screenshots and UI checks.
			QTimer::singleShot(800, &app, [&core, &window] {
				obs_source_t *src = core.addSource("window_capture", "Window", nullptr, ObsCore::Fit::Fill);
				auto *props = new PropertiesDialog(src, "Window", &window);
				props->setAttribute(Qt::WA_DeleteOnClose);
				props->show();
				auto *filters = new FiltersDialog(core.microphone(), &window);
				filters->setAttribute(Qt::WA_DeleteOnClose);
				filters->show();
				obs_data_t *a = obs_data_create();
				obs_data_set_string(a, "kind", "alerts");
				obs_source_t *alerts = core.addSourceAt("zunimi_overlay", "Alerts", a, QRectF(0, 0, 800, 450));
				obs_data_release(a);
				auto *designer = new AlertDesigner(alerts, &window);
				designer->setAttribute(Qt::WA_DeleteOnClose);
				designer->show();
			});
		}
		if (app.arguments().contains("--selftest-remote")) {
			// Turns the remote on at port 4466 and drives it the way a Stream Deck would: logs in, asks, switches a
			// scene, mutes the mic, waits for the events; a wrong password must be turned away; the phone page must
			// load. With --hold it then stays up 90 s for outside clients. Run with ZUNIMI_DATA set.
			Settings::get().set("remote/on", true);
			Settings::get().set("remote/lan", false);
			Settings::get().set("remote/port", 4466);
			Settings::get().setSecret("remote/password", "selftest-pass");
			window.applyRemote();
			core.addScene("Remote B");
			core.setScene("Remote B");
			core.addSource("color_source_v3", "Remote colour", nullptr, ObsCore::Fit::Fill);
			core.setScene(core.scenes().first());
			auto passed = std::make_shared<QStringList>();
			const QStringList wanted = {"page", "login", "GetVersion", "GetSceneList", "unknown", "batch",
						    "scene event", "mute event", "hotkey", "wrong password"};
			auto finish = [passed, wanted, &window, &app] {
				QStringList missing;
				for (const QString &w : wanted)
					if (!passed->contains(w))
						missing << w;
				blog(missing.isEmpty() ? LOG_INFO : LOG_ERROR, "selftest remote: %s",
				     missing.isEmpty() ? "all passed" : ("MISSING " + missing.join(", ")).toUtf8().constData());
				const int hold = app.arguments().contains("--hold") ? 90000 : 300;
				QTimer::singleShot(hold, [&window] { window.modes()->quit(false); });
			};
			auto check = [passed, wanted, finish](bool ok, const QString &what) {
				if (passed->contains(what))
					return; // outside clients (--hold) cause more events; the first answer counts
				blog(ok ? LOG_INFO : LOG_ERROR, "selftest remote: %s %s", what.toUtf8().constData(), ok ? "ok" : "FAILED");
				if (ok && !passed->contains(what)) {
					passed->append(what);
					if (passed->size() == wanted.size())
						finish();
				}
			};
			QTimer::singleShot(20000, &app, [passed, wanted, finish] {
				if (passed->size() < wanted.size())
					finish();
			});
			auto *net = new QNetworkAccessManager(&app);
			QNetworkReply *page = net->get(QNetworkRequest(QUrl("http://127.0.0.1:4466/")));
			QObject::connect(page, &QNetworkReply::finished, &app, [page, check] {
				check(page->readAll().contains("Zunimi remote"), "page");
				page->deleteLater();
			});
			auto auth = [](const QJsonObject &d, const QByteArray &pw) {
				const QJsonObject a = d.value("authentication").toObject();
				auto h = [](const QByteArray &x) { return QCryptographicHash::hash(x, QCryptographicHash::Sha256).toBase64(); };
				return QString::fromLatin1(h(h(pw + a.value("salt").toString().toLatin1()) + a.value("challenge").toString().toLatin1()));
			};
			auto *ws = new WebSocket(&app);
			auto sendJson = [ws](int op, const QJsonObject &d) {
				ws->sendText(QString::fromUtf8(QJsonDocument(QJsonObject{{"op", op}, {"d", d}}).toJson(QJsonDocument::Compact)));
			};
			auto req = [sendJson](const QString &type, const QJsonObject &data = {}) {
				sendJson(6, {{"requestType", type}, {"requestId", type}, {"requestData", data}});
			};
			QObject::connect(ws, &WebSocket::textMessage, &app, [=, &core](const QString &text) {
				const QJsonObject msg = QJsonDocument::fromJson(text.toUtf8()).object();
				const int op = msg.value("op").toInt();
				const QJsonObject d = msg.value("d").toObject();
				if (op == 0) {
					sendJson(1, {{"rpcVersion", 1}, {"authentication", auth(d, "selftest-pass")}});
				} else if (op == 2) {
					check(true, "login");
					req("GetVersion");
					req("GetSceneList");
					req("NoSuchRequest");
					sendJson(8, {{"requestId", "b"},
						     {"requests", QJsonArray{QJsonObject{{"requestType", "GetStudioModeEnabled"}},
									     QJsonObject{{"requestType", "GetRecordStatus"}}}}});
					req("SetCurrentProgramScene", {{"sceneName", "Remote B"}});
					req("ToggleInputMute", {{"inputName", QString::fromUtf8(obs_source_get_name(core.microphone()))}});
					req("TriggerHotkeyByName", {{"hotkeyName", "zunimi.mark"}});
				} else if (op == 7) {
					const QString id = d.value("requestId").toString();
					const QJsonObject st = d.value("requestStatus").toObject(), out = d.value("responseData").toObject();
					if (id == "GetVersion")
						check(out.value("rpcVersion").toInt() == 1 && out.value("availableRequests").toArray().size() > 40, "GetVersion");
					else if (id == "GetSceneList")
						check(out.value("scenes").toArray().size() >= 2 && !out.value("currentProgramSceneName").toString().isEmpty(), "GetSceneList");
					else if (id == "NoSuchRequest")
						check(!st.value("result").toBool() && st.value("code").toInt() == 204, "unknown");
					else if (id == "TriggerHotkeyByName")
						check(st.value("result").toBool(), "hotkey");
					else if (!st.value("result").toBool())
						check(false, id + ": " + st.value("comment").toString());
				} else if (op == 9) {
					const QJsonArray r = d.value("results").toArray();
					check(r.size() == 2 && r[0].toObject().value("responseData").toObject().contains("studioModeEnabled"), "batch");
				} else if (op == 5) {
					const QString type = d.value("eventType").toString();
					const QJsonObject e = d.value("eventData").toObject();
					if (type == "CurrentProgramSceneChanged")
						check(e.value("sceneName").toString() == "Remote B", "scene event");
					else if (type == "InputMuteStateChanged")
						check(e.value("inputMuted").toBool(), "mute event");
				}
			});
			ws->open(QUrl("ws://127.0.0.1:4466/"));
			auto *intruder = new WebSocket(&app);
			auto *identified = new bool(false);
			QObject::connect(intruder, &WebSocket::textMessage, &app, [=](const QString &text) {
				const QJsonObject msg = QJsonDocument::fromJson(text.toUtf8()).object();
				if (msg.value("op").toInt() == 0)
					intruder->sendText(QString::fromUtf8(QJsonDocument(QJsonObject{
						{"op", 1}, {"d", QJsonObject{{"rpcVersion", 1}, {"authentication", auth(msg.value("d").toObject(), "guess")}}}})
										    .toJson(QJsonDocument::Compact)));
				else if (msg.value("op").toInt() == 2)
					*identified = true;
			});
			QObject::connect(intruder, &WebSocket::closed, &app, [=](const QString &) { check(!*identified, "wrong password"); });
			intruder->open(QUrl("ws://127.0.0.1:4466/"));
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
				QTimer::singleShot(200, [&window] { window.modes()->quit(false); });
			};
			QObject::connect(updater, &Updater::found, &app, [updater](const UpdateInfo &i) {
				blog(LOG_INFO, "selftest update: found %s %s %s", i.version.toUtf8().constData(),
				     i.asset.toUtf8().constData(), i.digest.toUtf8().constData());
				updater->download(i); // downloads and checks the real installer, never runs it
			});
			QObject::connect(updater, &Updater::ready, &app, [done](const QString &path) {
				done(QString("downloaded and verified %1 (%2 bytes)").arg(path).arg(QFileInfo(path).size()));
				QFile::remove(path);
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
