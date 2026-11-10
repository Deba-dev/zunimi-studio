#include "core/Assist.h"

#include "core/Settings.h"
#include "events/Events.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <functional>

#include <windows.h>
#include <psapi.h>

namespace {
const QStringList PRIVATE_WINDOWS = {"password",   "bitwarden",  "1password", "lastpass",     "keepass",
				     "stream key", "stream url", "phonepe",   "paytm",        "google pay",
				     "net banking", "netbanking", "whatsapp",  "otp",          "authenticator"};

QString clock(qint64 s)
{
	return QString("%1:%2:%3").arg(s / 3600, 2, 10, QChar('0')).arg((s / 60) % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
}

bool isCapture(const char *id)
{
	return !strcmp(id, "monitor_capture") || !strcmp(id, "window_capture") || !strcmp(id, "game_capture");
}
} // namespace

Assist::Assist(ObsCore *core, QObject *parent) : QObject(parent), m_core(core), m_timer(new QTimer(this))
{
	connect(m_timer, &QTimer::timeout, this, &Assist::tick);
	m_timer->start(1000);
	if (obs_source_t *mic = core->microphone()) {
		m_meter = obs_volmeter_create(OBS_FADER_LOG);
		obs_volmeter_attach_source(m_meter, mic);
		obs_volmeter_add_callback(m_meter, onMic, this);
	}
	connect(core, &ObsCore::streamingChanged, this, [this](bool on) {
		m_live = on;
		if (on) {
			m_report = Report();
			m_minuteDrops.clear();
			m_kbpsSum = m_fpsSum = 0;
			m_samples = 0;
			m_lastHealBitrate = m_core->plan().bitrate;
			m_micQuietSince = m_blackSince = 0;
			m_micWarned = m_blackWarned = false;
			return;
		}
		// the after-stream report
		Report &r = m_report;
		if (m_samples < 10)
			return;
		r.avgKbps = m_kbpsSum / m_samples;
		r.avgFps = m_fpsSum / m_samples;
		for (int i = 0; i < m_minuteDrops.size(); i++)
			if (m_minuteDrops[i] > r.worstDrop) {
				r.worstDrop = m_minuteDrops[i];
				r.worstMinute = i;
			}
		QString html = QString("<h3>Stream report</h3><table cellspacing=4>"
				       "<tr><td>Live for</td><td><b>%1</b></td></tr>"
				       "<tr><td>Average bitrate</td><td>%2 kbps</td></tr>"
				       "<tr><td>Frames dropped by the network</td><td>%3%</td></tr>"
				       "<tr><td>Average frame rate</td><td>%4 fps</td></tr>"
				       "<tr><td>Busiest moment for the CPU</td><td>%5%</td></tr>"
				       "<tr><td>Times Zunimi lowered the bitrate to keep you live</td><td>%6</td></tr>")
				       .arg(clock(r.seconds))
				       .arg(r.avgKbps, 0, 'f', 0)
				       .arg(r.droppedPercent, 0, 'f', 2)
				       .arg(r.avgFps, 0, 'f', 1)
				       .arg(r.peakCpu, 0, 'f', 0)
				       .arg(r.heals);
		if (r.worstMinute >= 0 && r.worstDrop > 0.5)
			html += QString("<tr><td>Roughest minute</td><td>minute %1 (%2% dropped)</td></tr>")
					.arg(r.worstMinute + 1)
					.arg(r.worstDrop, 0, 'f', 1);
		html += "</table>";
		QStringList tips;
		if (r.droppedPercent > 1)
			tips << "Your internet dropped frames. A lower bitrate, or a wired connection instead of Wi-Fi, would help.";
		if (r.peakCpu > 85)
			tips << "Your CPU was nearly full. A hardware encoder (NVENC, AMF, QuickSync) or the Potato profile frees it.";
		if (r.avgFps > 0 && r.avgFps < m_core->plan().fps * 0.95)
			tips << "Frames were missed. Capping your game's FPS gives the stream room.";
		if (tips.isEmpty())
			tips << "Smooth stream. Nothing to change.";
		html += "<p>" + tips.join("<br>") + "</p>";
		// keep it with the logs
		QDir dir(Settings::dataDir() + "/reports");
		dir.mkpath(".");
		QFile f(dir.filePath(QDateTime::currentDateTime().toString("yyyy-MM-dd hh-mm") + ".html"));
		if (f.open(QIODevice::WriteOnly))
			f.write(html.toUtf8());
		emit reportReady(html);
	});
	connect(core, &ObsCore::recordingChanged, this, [this](bool on, const QString &file) {
		if (on) {
			m_marks.clear();
			m_recording = file;
		} else if (!m_marks.isEmpty()) {
			writeChapters(file);
		}
	});
}

Assist::~Assist()
{
	if (m_meter) {
		obs_volmeter_remove_callback(m_meter, onMic, this);
		obs_volmeter_destroy(m_meter);
	}
}

void Assist::onMic(void *param, const float[MAX_AUDIO_CHANNELS], const float peak[MAX_AUDIO_CHANNELS], const float[MAX_AUDIO_CHANNELS])
{
	auto *self = static_cast<Assist *>(param);
	float loudest = -100.0f;
	for (int i = 0; i < MAX_AUDIO_CHANNELS; i++)
		if (std::isfinite(peak[i]))
			loudest = std::max(loudest, peak[i]);
	float was = self->m_micPeak.load();
	while (loudest > was && !self->m_micPeak.compare_exchange_weak(was, loudest)) {
	}
}

// ---- pre-flight
QVector<Finding> Assist::preflight(bool forLive)
{
	QVector<Finding> out;
	Settings &st = Settings::get();
	const QVector<SceneItem> items = m_core->items();
	bool anyVisible = false, captureEmpty = false;
	for (const SceneItem &i : items) {
		if (!i.visible)
			continue;
		anyVisible = true;
		obs_sceneitem_t *it = m_core->item(i.name);
		obs_source_t *src = it ? obs_sceneitem_get_source(it) : nullptr;
		if (src && isCapture(obs_source_get_id(src)) && obs_source_get_width(src) == 0)
			captureEmpty = true;
	}
	if (!anyVisible)
		out << Finding{2, "This scene is empty: viewers would see a black screen.", "", ""};
	else if (captureEmpty)
		out << Finding{1, "A capture in this scene shows nothing yet (is the game or window open?).", "", ""};
	if (obs_source_t *mic = m_core->microphone()) {
		if (obs_source_muted(mic))
			out << Finding{2, "Your mic is muted.", "unmute_mic", "Unmute"};
		else if (m_micQuietSince && QDateTime::currentMSecsSinceEpoch() - m_micQuietSince > 30000)
			out << Finding{1, "Your mic hasn't picked up any sound for a while. Is the right mic chosen?", "", ""};
	}
	if (!forLive || st.value("stream/autoRecord", false).toBool()) {
		const QString folder = st.value("record/folder").toString();
		QStorageInfo disk(folder.isEmpty() ? QDir::homePath() : folder);
		if (disk.isValid() && disk.bytesAvailable() < 3LL * 1024 * 1024 * 1024)
			out << Finding{2, QString("Only %1 GB free where recordings go.").arg(disk.bytesAvailable() / 1073741824.0, 0, 'f', 1), "", ""};
	}
	const VideoPlan &p = m_core->plan();
	if (p.encoder == "obs_x264" && m_core->hardware().anyHardwareEncoder())
		out << Finding{1, "You're encoding on the CPU though your graphics card can do it for almost nothing.",
			       "hw_encoder", "Use the graphics card"};
	if (p.encoder == "obs_x264" && m_core->hardware().cores <= 4 && p.height >= 1080)
		out << Finding{2, "1080p on a 4-thread CPU will likely stutter.", "potato", "Use 720p"};
	if (forLive && p.bitrate > 6000 && st.value("stream/platform").toString() == "twitch")
		out << Finding{1, "Twitch caps most channels at 6000 kbps; higher may be refused.", "twitch_bitrate", "Set 6000"};
	if (forLive && m_onBattery)
		out << Finding{1, "You're on battery: plug in, or the laptop may slow down mid-stream.", "", ""};
	return out;
}

// ---- stream doctor
Finding Assist::doctor(const Health &h) const
{
	if (h.encodeLag >= 1.0) {
		if (m_core->plan().encoder == "obs_x264" && m_core->hardware().anyHardwareEncoder())
			return {2, h.text, "hw_encoder", m_core->busy() ? "Use GPU next time" : "Use the graphics card"};
		return {2, h.text, "potato", m_core->busy() ? "Lighter next time" : "Lighter settings"};
	}
	if (h.renderLag >= 1.0)
		return {h.level, h.text, "pause_preview", "Pause the preview"};
	if (h.dropped >= 1.0 && !Settings::get().value("stream/autoHeal", true).toBool())
		return {h.level, h.text, "auto_heal", "Keep me live automatically"};
	return {};
}

void Assist::applyFix(const QString &id)
{
	Settings &st = Settings::get();
	QString error;
	if (id == "unmute_mic") {
		if (obs_source_t *mic = m_core->microphone()) {
			obs_source_set_muted(mic, false);
			m_core->saveAudioSettings(mic);
		}
	} else if (id == "hw_encoder" || id == "potato" || id == "twitch_bitrate") {
		VideoPlan plan = m_core->plan();
		if (id == "hw_encoder")
			plan.encoder = bestEncoder(m_core->hardware());
		else if (id == "potato")
			plan = planFor(Profile::Potato, m_core->hardware());
		else
			plan.bitrate = 6000;
		if (m_core->busy()) {
			// can't change while encoding: saved for the next stream
			st.set("video/encoder", plan.encoder);
			st.set("video/width", plan.width);
			st.set("video/height", plan.height);
			st.set("video/fps", plan.fps);
			st.set("video/bitrate", plan.bitrate);
			emit alarm("Saved: it takes effect the next time you go live or restart Zunimi Studio.");
		} else if (m_core->applyPlan(plan, &error)) {
			if (id == "potato")
				st.set("video/profile", "potato");
			emit alarm("Done: " + encoderLabel(plan.encoder) + QString(", %1p%2, %3 kbps.").arg(plan.height).arg(plan.fps).arg(plan.bitrate));
		}
	} else if (id == "pause_preview") {
		st.set("preview/pauseWhileLive", true);
		emit alarm("The preview pauses while you're live; that frees your graphics card.");
	} else if (id == "auto_heal") {
		st.set("stream/autoHeal", true);
	}
}

// ---- once a second
void Assist::sample(const Health &h)
{
	if (!m_live)
		return;
	m_samples++;
	m_report.seconds++;
	m_fpsSum += h.fps;
	m_kbpsSum += h.bitrate;
	m_report.peakCpu = std::max(m_report.peakCpu, h.cpu);
	const OutputStats s = m_core->streamStats();
	if (s.totalFrames > 0)
		m_report.droppedPercent = 100.0 * s.droppedFrames / s.totalFrames;
	const int minute = int(m_report.seconds / 60);
	if (m_minuteDrops.size() <= minute)
		m_minuteDrops.resize(minute + 1);
	m_minuteDrops[minute] = std::max(m_minuteDrops[minute], h.dropped);
	if (h.bitrate < m_lastHealBitrate)
		m_report.heals++;
	m_lastHealBitrate = h.bitrate;
}

void Assist::tick()
{
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	Settings &st = Settings::get();
	// mic silence
	const float peak = m_micPeak.exchange(-100.0f);
	obs_source_t *mic = m_core->microphone();
	if (peak > -50.0f || !mic || obs_source_muted(mic)) {
		m_micQuietSince = 0;
		m_micWarned = false;
	} else if (!m_micQuietSince) {
		m_micQuietSince = now;
	}
	const int quietMinutes = st.value("alarms/micMinutes", 3).toInt();
	if (m_live && quietMinutes > 0 && m_micQuietSince && !m_micWarned && now - m_micQuietSince > quietMinutes * 60000LL) {
		m_micWarned = true;
		emit alarm(QString("Your mic has been silent for %1 minutes. Is it unplugged, or the wrong one?").arg(quietMinutes));
	}
	// black picture
	if (m_live && st.value("alarms/black", true).toBool() && (now / 1000) % 3 == 0)
		blackCheck();
	const double luma = m_luma.load();
	if (m_live && luma >= 0 && luma < 3.0) {
		if (!m_blackSince)
			m_blackSince = now;
		if (!m_blackWarned && now - m_blackSince > 10000) {
			m_blackWarned = true;
			emit alarm("Your stream has been black for 10 seconds. A capture may have stopped (game minimized?).");
		}
	} else if (luma >= 3.0) {
		m_blackSince = 0;
		m_blackWarned = false;
	}
	automation();
	// battery, every 30 s
	if ((now / 1000) % 30 == 0) {
		SYSTEM_POWER_STATUS power{};
		if (GetSystemPowerStatus(&power) && power.BatteryFlag != 128) {
			const bool battery = power.ACLineStatus == 0;
			if (battery && !m_onBattery && st.value("video/profile").toString() != "potato")
				emit alarm("Your laptop is on battery. Plug in for the stream, or pick the Potato profile to save power.");
			m_onBattery = battery;
		}
	}
}

void Assist::blackCheck()
{
	// the picture shrunk to 32x18 on the graphics thread; its average brightness tells black from not
	QPointer<Assist> self(this);
	auto *job = new std::function<void()>([self] {
		obs_enter_graphics();
		gs_texrender_t *tex = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		gs_stagesurf_t *stage = gs_stagesurface_create(32, 18, GS_RGBA);
		obs_video_info ovi{};
		obs_get_video_info(&ovi);
		double sum = -1;
		if (gs_texrender_begin(tex, 32, 18)) {
			vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, float(ovi.base_width), 0.0f, float(ovi.base_height), -100.0f, 100.0f);
			obs_render_main_texture();
			gs_texrender_end(tex);
			gs_stage_texture(stage, gs_texrender_get_texture(tex));
			uint8_t *data = nullptr;
			uint32_t line = 0;
			if (gs_stagesurface_map(stage, &data, &line)) {
				sum = 0;
				for (int y = 0; y < 18; y++)
					for (int x = 0; x < 32; x++) {
						const uint8_t *p = data + y * line + x * 4;
						sum += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
					}
				sum /= 32 * 18;
				gs_stagesurface_unmap(stage);
			}
		}
		gs_stagesurface_destroy(stage);
		gs_texrender_destroy(tex);
		obs_leave_graphics();
		if (self)
			self->m_luma.store(sum);
	});
	obs_queue_task(
		OBS_TASK_GRAPHICS,
		[](void *p) {
			auto *fn = static_cast<std::function<void()> *>(p);
			(*fn)();
			delete fn;
		},
		job, false);
}

// ---- automation
QString Assist::foregroundApp(QString *title)
{
	HWND w = GetForegroundWindow();
	if (!w)
		return {};
	if (title) {
		wchar_t t[512] = {};
		GetWindowTextW(w, t, 511);
		*title = QString::fromWCharArray(t);
	}
	DWORD pid = 0;
	GetWindowThreadProcessId(w, &pid);
	HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!proc)
		return {};
	wchar_t path[MAX_PATH] = {};
	DWORD size = MAX_PATH;
	QString exe;
	if (QueryFullProcessImageNameW(proc, 0, path, &size))
		exe = QFileInfo(QString::fromWCharArray(path, int(size))).fileName();
	CloseHandle(proc);
	return exe;
}

QStringList Assist::runningApps()
{
	QStringList apps;
	EnumWindows(
		[](HWND w, LPARAM param) -> BOOL {
			if (!IsWindowVisible(w) || GetWindowTextLengthW(w) == 0)
				return TRUE;
			DWORD pid = 0;
			GetWindowThreadProcessId(w, &pid);
			HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
			if (proc) {
				wchar_t path[MAX_PATH] = {};
				DWORD size = MAX_PATH;
				if (QueryFullProcessImageNameW(proc, 0, path, &size)) {
					auto *list = reinterpret_cast<QStringList *>(param);
					const QString exe = QFileInfo(QString::fromWCharArray(path, int(size))).fileName();
					if (!list->contains(exe, Qt::CaseInsensitive) && exe.compare("explorer.exe", Qt::CaseInsensitive))
						list->append(exe);
				}
				CloseHandle(proc);
			}
			return TRUE;
		},
		reinterpret_cast<LPARAM>(&apps));
	apps.sort(Qt::CaseInsensitive);
	return apps;
}

void Assist::automation()
{
	Settings &st = Settings::get();
	QString title;
	const QString app = foregroundApp(&title);

	// privacy guard: while a private window is in front, captures that could show it are hidden
	const bool guard = st.value("privacy/on", true).toBool() &&
			   (!st.value("privacy/onlyLive", true).toBool() || m_core->streaming() || m_core->recording());
	QStringList patterns = st.value("privacy/patterns").toStringList();
	if (patterns.isEmpty())
		patterns = PRIVATE_WINDOWS;
	bool privateNow = false;
	if (guard && app.compare("zunimi-studio.exe", Qt::CaseInsensitive) != 0) {
		const QString hay = (title + " " + app).toLower();
		for (const QString &p : patterns)
			if (!p.trimmed().isEmpty() && hay.contains(p.trimmed().toLower()))
				privateNow = true;
	}
	if (privateNow && m_hiddenForPrivacy.isEmpty()) {
		m_privacyScene = m_core->programScene();
		obs_source_t *scene = obs_get_source_by_name(m_privacyScene.toUtf8().constData());
		if (scene) {
			obs_scene_enum_items(
				obs_scene_from_source(scene),
				[](obs_scene_t *, obs_sceneitem_t *it, void *param) {
					obs_source_t *src = obs_sceneitem_get_source(it);
					const char *id = obs_source_get_id(src);
					if (obs_sceneitem_visible(it) && (!strcmp(id, "monitor_capture") || !strcmp(id, "window_capture"))) {
						obs_sceneitem_set_visible(it, false);
						static_cast<QStringList *>(param)->append(QString::fromUtf8(obs_source_get_name(src)));
					}
					return true;
				},
				&m_hiddenForPrivacy);
			obs_source_release(scene);
		}
		if (!m_hiddenForPrivacy.isEmpty())
			emit alarm("Privacy guard: a private window is in front, so your screen capture is hidden until you leave it.");
	} else if (!privateNow && !m_hiddenForPrivacy.isEmpty()) {
		obs_source_t *scene = obs_get_source_by_name(m_privacyScene.toUtf8().constData());
		if (scene) {
			for (const QString &name : m_hiddenForPrivacy)
				if (obs_sceneitem_t *it = obs_scene_find_source(obs_scene_from_source(scene), name.toUtf8().constData()))
					obs_sceneitem_set_visible(it, true);
			obs_source_release(scene);
		}
		m_hiddenForPrivacy.clear();
	}

	// scene switching by the app in front (only when that app changes, so a manual switch sticks)
	if (!st.value("auto/on", false).toBool() || app == m_lastApp || app.isEmpty()) {
		if (!app.isEmpty())
			m_lastApp = app;
		return;
	}
	m_lastApp = app;
	const QJsonArray rules = QJsonDocument::fromJson(st.value("auto/rules").toByteArray()).array();
	for (const QJsonValue &v : rules) {
		const QJsonObject r = v.toObject();
		if (r.value("app").toString().compare(app, Qt::CaseInsensitive) != 0)
			continue;
		const QString scene = r.value("scene").toString();
		if (!m_core->scenes().contains(scene) || m_core->programScene() == scene)
			return;
		m_core->setScene(scene);
		if (m_core->studioMode())
			m_core->transitionNow();
		return;
	}
}

// ---- mark moment
void Assist::mark(const QString &label)
{
	const OutputStats rec = m_core->recordStats();
	const OutputStats live = m_core->streamStats();
	if (!rec.active && !live.active && !m_core->replaying()) {
		emit alarm("Mark moment works while recording, live or with the replay buffer on.");
		return;
	}
	const QString name = label.isEmpty() ? QString("Moment %1").arg(m_marks.size() + 1) : label;
	if (rec.active)
		m_marks.append({rec.seconds, name});
	if (m_core->replaying())
		m_core->saveReplay(); // the moment itself, as a clip
	// a log Clip Studio can read: when it happened, relative to the recording and to the stream
	QFile f(Settings::dataDir() + "/moments.jsonl");
	if (f.open(QIODevice::Append)) {
		const QJsonObject o{{"at", QDateTime::currentDateTime().toString(Qt::ISODate)},
				    {"label", name},
				    {"recording", m_recording},
				    {"recordingSeconds", rec.active ? rec.seconds : -1},
				    {"liveSeconds", live.active ? live.seconds : -1}};
		f.write(QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n");
	}
	emit alarm(QString("Marked: %1%2").arg(name, rec.active ? " (" + clock(rec.seconds) + " into the recording)" : QString()));
}

void Assist::writeChapters(const QString &file)
{
	// YouTube-style chapters next to the video: paste into the description
	QFile f(QFileInfo(file).dir().filePath(QFileInfo(file).completeBaseName() + " chapters.txt"));
	if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
		return;
	QString out = "00:00 Start\n";
	for (const auto &[s, label] : m_marks)
		out += clock(s).replace(QRegularExpression("^00:"), "") + " " + label + "\n";
	f.write(out.toUtf8());
	m_marks.clear();
}
