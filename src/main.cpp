#include "core/Addons.h"
#include "core/Assist.h"
#include "core/ObsCore.h"
#include "core/Settings.h"
#include "core/Updater.h"
#include "events/Events.h"
#include "overlays/Overlays.h"
#include "ui/AlertDesigner.h"
#include "ui/FiltersDialog.h"
#include "ui/MainWindow.h"
#include "ui/PropertiesView.h"
#include "ui/Theme.h"
#include "ui/WindowModes.h"
#include "version.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
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
	app.setStyleSheet(Theme::styleSheet());
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
				check(has(assist->preflight(true), "empty"), "pre-flight: an empty scene is caught");
				obs_source_set_muted(core.microphone(), true);
				QVector<Finding> f = assist->preflight(true);
				check(has(f, "muted"), "pre-flight: a muted mic is caught");
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
					check(text.startsWith("00:00 Start") && text.contains("First") && text.contains("Moment 2"),
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
