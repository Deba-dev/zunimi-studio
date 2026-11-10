#include "core/ObsCore.h"

#include "core/Settings.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>

#include <util/platform.h>

#include <algorithm>
#include <cmath>

#include <windows.h>
#include <psapi.h>

namespace {
const char *SCENE_NAME = "Zunimi Main";
const char *DESKTOP_NAME = "Desktop audio";
const char *MIC_NAME = "Microphone";

// Only these modules are ever loaded. A module that is not listed costs no RAM and no CPU.
// Later features (browser source, AI, vertical canvas...) are added here only when switched on.
const QStringList CORE_MODULES = {"image-source", "obs-ffmpeg",   "obs-filters", "obs-nvenc", "obs-outputs",
				  "obs-qsv11",    "obs-text",     "obs-transitions", "obs-x264", "rtmp-services",
				  "win-capture",  "win-dshow",    "win-wasapi"};

struct ModuleList {
	QStringList allowed;
	QStringList loaded;
	QStringList failed;
};

void openModule(void *param, const obs_module_info2 *info)
{
	auto *list = static_cast<ModuleList *>(param);
	const QString name = QString::fromUtf8(info->name);
	if (!list->allowed.contains(name))
		return;
	obs_module_t *module = nullptr;
	if (obs_open_module(&module, info->bin_path, info->data_path) == MODULE_SUCCESS && obs_init_module(module))
		list->loaded << name;
	else
		list->failed << name;
}

QString stopReason(int code, const char *detail)
{
	QString why;
	switch (code) {
	case OBS_OUTPUT_BAD_PATH:
		why = "The server address is wrong. Check it in Settings > Stream.";
		break;
	case OBS_OUTPUT_CONNECT_FAILED:
		why = "Could not reach the streaming server. Check your internet, or the server address.";
		break;
	case OBS_OUTPUT_INVALID_STREAM:
		why = "The platform refused the stream key. Copy a fresh key from your dashboard.";
		break;
	case OBS_OUTPUT_DISCONNECTED:
		why = "Your internet dropped and the stream could not reconnect.";
		break;
	case OBS_OUTPUT_NO_SPACE:
		why = "The disk is full, so recording stopped. Free some space or pick another folder.";
		break;
	case OBS_OUTPUT_ENCODE_ERROR:
		why = "The video encoder stopped working. Try the x264 (CPU) encoder in Settings > Video.";
		break;
	case OBS_OUTPUT_UNSUPPORTED:
		why = "This encoder or format is not supported here.";
		break;
	default:
		why = "Something went wrong.";
	}
	if (detail && *detail)
		why += QString(" (%1)").arg(QString::fromUtf8(detail));
	return why;
}

obs_data_t *encoderSettings(const VideoPlan &plan, bool potato)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "rate_control", "CBR");
	obs_data_set_int(s, "bitrate", plan.bitrate);
	obs_data_set_int(s, "keyint_sec", 2);
	obs_data_set_string(s, "profile", "high");
	if (plan.encoder == "obs_x264") {
		obs_data_set_string(s, "preset", potato ? "superfast" : "veryfast");
		if (potato) // fewer frames held in memory: a 4 GB laptop needs the RAM more than the last bit of quality
			obs_data_set_string(s, "x264opts", "rc-lookahead=10 ref=1 threads=4");
	}
	else if (plan.encoder == "obs_nvenc_h264_tex")
		obs_data_set_string(s, "preset2", potato ? "p4" : "p5");
	else if (plan.encoder == "obs_qsv11_v2")
		obs_data_set_string(s, "target_usage", potato ? "speed" : "balanced");
	else if (plan.encoder == "h264_texture_amf")
		obs_data_set_string(s, "preset", potato ? "speed" : "balanced");
	return s;
}

double percent(uint64_t part, uint64_t whole)
{
	return whole ? 100.0 * double(part) / double(whole) : 0.0;
}
} // namespace

ObsCore::ObsCore(QObject *parent) : QObject(parent) {}

ObsCore::~ObsCore()
{
	shutdown();
}

bool ObsCore::start(QString *error)
{
	const QString config = Settings::dataDir() + "/plugin_config";
	QDir().mkpath(config);
	if (!obs_startup("en-US", config.toUtf8().constData(), nullptr)) {
		*error = "The video engine could not start.";
		return false;
	}
	m_started = true;

	obs_audio_info audio{};
	audio.samples_per_sec = 48000;
	audio.speakers = SPEAKERS_STEREO;
	if (!obs_reset_audio(&audio)) {
		*error = "The audio engine could not start.";
		return false;
	}

	// Start with the saved plan (or a safe 720p30), so the graphics are up before modules load.
	Settings &s = Settings::get();
	VideoPlan plan;
	plan.width = s.value("video/width", 1280).toInt();
	plan.height = s.value("video/height", 720).toInt();
	plan.fps = s.value("video/fps", 30).toInt();
	plan.bitrate = s.value("video/bitrate", 2500).toInt();
	plan.encoder = s.value("video/encoder", "obs_x264").toString();
	if (!resetVideo(plan, error))
		return false;
	if (!loadModules(error))
		return false;

	m_hw = probeHardware();
	if (!s.has("video/profile")) { // first launch: Auto-Tune
		const Profile profile = pickProfile(m_hw);
		const VideoPlan tuned = planFor(profile, m_hw);
		s.set("video/profile", profileKey(profile));
		if (!applyPlan(tuned, error))
			return false;
	} else if (plan.encoder != "obs_x264" && bestEncoder(m_hw) == "obs_x264") {
		plan.encoder = "obs_x264"; // the graphics card changed, so its encoder is gone
		applyPlan(plan, error);
	}

	createScene();
	createAudio();
	m_cpu = os_cpu_usage_info_start();
	return true;
}

bool ObsCore::loadModules(QString *error)
{
	// The exe lives in bin/64bit; modules are core/<name>/<name>.dll with their data beside them.
	obs_add_module_path("../../core/%module%", "../../core/%module%/data");
	ModuleList list;
	list.allowed = CORE_MODULES;
	obs_find_modules2(openModule, &list);
	obs_post_load_modules();
	obs_log_loaded_modules();
	m_modules = list.loaded;
	if (!list.loaded.contains("win-capture") || !list.loaded.contains("obs-x264")) {
		*error = "Some parts of Zunimi Studio are missing (" + list.failed.join(", ") +
			 "). Reinstalling should fix it.";
		return false;
	}
	return true;
}

bool ObsCore::resetVideo(const VideoPlan &plan, QString *error)
{
	obs_video_info ovi{};
	ovi.graphics_module = "libobs-d3d11";
	ovi.fps_num = uint32_t(plan.fps);
	ovi.fps_den = 1;
	ovi.base_width = ovi.output_width = uint32_t(plan.width);
	ovi.base_height = ovi.output_height = uint32_t(plan.height);
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	ovi.gpu_conversion = true;
	ovi.adapter = 0;
	const int result = obs_reset_video(&ovi);
	if (result != OBS_VIDEO_SUCCESS) {
		*error = result == OBS_VIDEO_NOT_SUPPORTED
				 ? "Your graphics card does not support DirectX 11, which Zunimi Studio needs."
				 : "The graphics could not start. Updating your graphics driver usually fixes this.";
		return false;
	}
	m_plan = plan;
	return true;
}

bool ObsCore::applyPlan(const VideoPlan &plan, QString *error)
{
	if (busy()) {
		*error = "Stop the stream and recording first.";
		return false;
	}
	const int oldWidth = m_plan.width;
	releaseEncoders();
	if (plan.width != m_plan.width || plan.height != m_plan.height || plan.fps != m_plan.fps) {
		if (!resetVideo(plan, error))
			return false;
		if (m_scene && oldWidth > 0)
			rescaleItems(double(plan.width) / double(oldWidth));
	}
	m_plan = plan;
	Settings &s = Settings::get();
	s.set("video/width", plan.width);
	s.set("video/height", plan.height);
	s.set("video/fps", plan.fps);
	s.set("video/bitrate", plan.bitrate);
	s.set("video/encoder", plan.encoder);
	return true;
}

void ObsCore::createScene()
{
	const QString path = Settings::dataDir() + "/scene.json";
	obs_data_t *root = obs_data_create_from_json_file_safe(path.toUtf8().constData(), "bak");
	if (root) {
		obs_data_array_t *sources = obs_data_get_array(root, "sources");
		if (sources) {
			obs_load_sources(sources, nullptr, nullptr);
			obs_data_array_release(sources);
		}
		obs_data_release(root);
		m_sceneSource = obs_get_source_by_name(SCENE_NAME); // keeps the reference
	}
	if (!m_sceneSource) {
		obs_scene_t *scene = obs_scene_create(SCENE_NAME);
		m_sceneSource = obs_scene_get_source(scene); // the creation reference is ours
	}
	m_scene = obs_scene_from_source(m_sceneSource);
	obs_set_output_source(0, m_sceneSource);
}

void ObsCore::createAudio()
{
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "device_id", "default");
	m_desktop = obs_source_create("wasapi_output_capture", DESKTOP_NAME, s, nullptr);
	m_mic = obs_source_create("wasapi_input_capture", MIC_NAME, s, nullptr);
	obs_data_release(s);
	if (m_desktop)
		obs_set_output_source(1, m_desktop);
	if (m_mic) {
		obs_set_output_source(3, m_mic);
		setMicCleanup(Settings::get().value("audio/micCleanup", true).toBool());
	}
	Settings &st = Settings::get();
	if (m_desktop) {
		obs_source_set_volume(m_desktop, float(st.value("audio/desktopVolume", 1.0).toDouble()));
		obs_source_set_muted(m_desktop, st.value("audio/desktopMuted", false).toBool());
	}
	if (m_mic) {
		obs_source_set_volume(m_mic, float(st.value("audio/micVolume", 1.0).toDouble()));
		obs_source_set_muted(m_mic, st.value("audio/micMuted", false).toBool());
	}
}

void ObsCore::setMicCleanup(bool on)
{
	// RNNoise: light, runs on the CPU, removes keyboard and fan noise from the mic.
	if (!m_mic)
		return;
	if (on && !m_micFilter) {
		obs_data_t *s = obs_data_create();
		obs_data_set_string(s, "method", "rnnoise");
		m_micFilter = obs_source_create_private("noise_suppress_filter", "Mic cleanup", s);
		obs_data_release(s);
		if (m_micFilter)
			obs_source_filter_add(m_mic, m_micFilter);
	} else if (!on && m_micFilter) {
		obs_source_filter_remove(m_mic, m_micFilter);
		obs_source_release(m_micFilter);
		m_micFilter = nullptr;
	}
	Settings::get().set("audio/micCleanup", on);
}

QString ObsCore::uniqueName(const QString &base) const
{
	QString name = base;
	for (int i = 2;; i++) {
		obs_source_t *existing = obs_get_source_by_name(name.toUtf8().constData());
		if (!existing)
			return name;
		obs_source_release(existing);
		name = QString("%1 %2").arg(base).arg(i);
	}
}

obs_sceneitem_t *ObsCore::find(const QString &name) const
{
	return m_scene ? obs_scene_find_source(m_scene, name.toUtf8().constData()) : nullptr;
}

obs_source_t *ObsCore::addSource(const char *kind, const QString &name, obs_data_t *settings, Fit fit)
{
	const QString unique = uniqueName(name);
	obs_source_t *source = obs_source_create(kind, unique.toUtf8().constData(), settings, nullptr);
	if (!source)
		return nullptr;
	obs_sceneitem_t *item = obs_scene_add(m_scene, source);
	const float w = float(m_plan.width), h = float(m_plan.height);
	vec2 pos{}, bounds{};
	switch (fit) {
	case Fit::Fill:
		obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
		obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
		vec2_set(&bounds, w, h);
		obs_sceneitem_set_bounds(item, &bounds);
		vec2_set(&pos, 0, 0);
		obs_sceneitem_set_pos(item, &pos);
		break;
	case Fit::Corner: {
		const float bw = w * 0.28f, bh = h * 0.28f, margin = h * 0.03f;
		obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
		obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_BOTTOM | OBS_ALIGN_RIGHT);
		vec2_set(&bounds, bw, bh);
		obs_sceneitem_set_bounds(item, &bounds);
		vec2_set(&pos, w - bw - margin, h - bh - margin);
		obs_sceneitem_set_pos(item, &pos);
		break;
	}
	case Fit::Center:
		obs_sceneitem_set_alignment(item, OBS_ALIGN_CENTER);
		vec2_set(&pos, w / 2, h / 2);
		obs_sceneitem_set_pos(item, &pos);
		break;
	}
	obs_source_release(source); // the scene keeps its own reference
	save();
	emit sceneChanged();
	return source;
}

void ObsCore::removeSource(const QString &name)
{
	if (obs_sceneitem_t *item = find(name)) {
		obs_source_t *source = obs_sceneitem_get_source(item);
		obs_sceneitem_remove(item);
		obs_source_remove(source);
		save();
		emit sceneChanged();
	}
}

void ObsCore::setVisible(const QString &name, bool visible)
{
	if (obs_sceneitem_t *item = find(name)) {
		obs_sceneitem_set_visible(item, visible);
		save();
	}
}

void ObsCore::moveSource(const QString &name, bool up)
{
	if (obs_sceneitem_t *item = find(name)) {
		obs_sceneitem_set_order(item, up ? OBS_ORDER_MOVE_UP : OBS_ORDER_MOVE_DOWN);
		save();
		emit sceneChanged();
	}
}

QVector<SceneItem> ObsCore::items() const
{
	QVector<SceneItem> list;
	if (!m_scene)
		return list;
	obs_scene_enum_items(
		m_scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *out = static_cast<QVector<SceneItem> *>(param);
			obs_source_t *source = obs_sceneitem_get_source(item);
			out->prepend({QString::fromUtf8(obs_source_get_name(source)),
				      QString::fromUtf8(obs_source_get_id(source)), obs_sceneitem_visible(item)});
			return true;
		},
		&list);
	return list;
}

void ObsCore::rescaleItems(double ratio)
{
	obs_scene_enum_items(
		m_scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			const float r = float(*static_cast<double *>(param));
			vec2 v{};
			obs_sceneitem_get_pos(item, &v);
			vec2_mulf(&v, &v, r);
			obs_sceneitem_set_pos(item, &v);
			if (obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE) {
				obs_sceneitem_get_bounds(item, &v);
				vec2_mulf(&v, &v, r);
				obs_sceneitem_set_bounds(item, &v);
			} else {
				obs_sceneitem_get_scale(item, &v);
				vec2_mulf(&v, &v, r);
				obs_sceneitem_set_scale(item, &v);
			}
			return true;
		},
		&ratio);
	save();
}

void ObsCore::save()
{
	if (!m_sceneSource)
		return;
	obs_data_array_t *sources = obs_save_sources_filtered(
		[](void *, obs_source_t *source) {
			const char *name = obs_source_get_name(source);
			return strcmp(name, DESKTOP_NAME) != 0 && strcmp(name, MIC_NAME) != 0;
		},
		nullptr);
	obs_data_t *root = obs_data_create();
	obs_data_set_array(root, "sources", sources);
	const QString path = Settings::dataDir() + "/scene.json";
	obs_data_save_json_safe(root, path.toUtf8().constData(), "tmp", "bak");
	obs_data_array_release(sources);
	obs_data_release(root);
}

// ---- encoders and outputs
bool ObsCore::ensureEncoders(QString *error)
{
	const bool potato = Settings::get().value("video/profile").toString() == "potato";
	if (!m_video) {
		obs_data_t *s = encoderSettings(m_plan, potato);
		m_video = obs_video_encoder_create(m_plan.encoder.toUtf8().constData(), "Zunimi video", s, nullptr);
		obs_data_release(s);
		if (!m_video) {
			*error = "The " + encoderLabel(m_plan.encoder) + " encoder is not available on this PC.";
			return false;
		}
		obs_encoder_set_video(m_video, obs_get_video());
	}
	if (!m_audio) {
		obs_data_t *s = obs_data_create();
		obs_data_set_int(s, "bitrate", 160);
		m_audio = obs_audio_encoder_create("ffmpeg_aac", "Zunimi audio", s, 0, nullptr);
		obs_data_release(s);
		if (!m_audio) {
			*error = "The audio encoder is missing.";
			return false;
		}
		obs_encoder_set_audio(m_audio, obs_get_audio());
	}
	return true;
}

void ObsCore::releaseEncoders()
{
	if (busy())
		return;
	if (m_video) {
		obs_encoder_release(m_video);
		m_video = nullptr;
	}
	if (m_audio) {
		obs_encoder_release(m_audio);
		m_audio = nullptr;
	}
}

bool ObsCore::startWithFallback(obs_output_t *output, QString *error)
{
	obs_output_set_video_encoder(output, m_video);
	obs_output_set_audio_encoder(output, m_audio, 0);
	if (obs_output_start(output))
		return true;
	// A hardware encoder that fails (old driver, busy GPU) falls back to x264 instead of failing.
	if (m_plan.encoder != "obs_x264" && !busy()) {
		const QString failed = encoderLabel(m_plan.encoder);
		VideoPlan plan = m_plan;
		plan.encoder = "obs_x264";
		QString ignored;
		if (applyPlan(plan, &ignored) && ensureEncoders(&ignored)) {
			obs_output_set_video_encoder(output, m_video);
			obs_output_set_audio_encoder(output, m_audio, 0);
			if (obs_output_start(output)) {
				emit notice(failed + " did not start, so Zunimi switched to x264 (CPU).");
				return true;
			}
		}
	}
	const char *detail = obs_output_get_last_error(output);
	*error = detail && *detail ? QString::fromUtf8(detail) : "The output could not start.";
	return false;
}

bool ObsCore::startStream(const QString &server, const QString &key, QString *error)
{
	if (streaming())
		return true;
	if (server.trimmed().isEmpty() || key.trimmed().isEmpty()) {
		*error = "Add your stream key first (Settings > Stream).";
		return false;
	}
	if (!ensureEncoders(error))
		return false;
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "server", server.trimmed().toUtf8().constData());
	obs_data_set_string(s, "key", key.trimmed().toUtf8().constData());
	if (m_service)
		obs_service_update(m_service, s);
	else
		m_service = obs_service_create("rtmp_custom", "Zunimi service", s, nullptr);
	obs_data_release(s);
	if (!m_stream) {
		m_stream = obs_output_create("rtmp_output", "Zunimi stream", nullptr, nullptr);
		obs_output_set_reconnect_settings(m_stream, 20, 3);
		signal_handler_t *sh = obs_output_get_signal_handler(m_stream);
		signal_handler_connect(sh, "start", onStreamStart, this);
		signal_handler_connect(sh, "stop", onStreamStop, this);
		signal_handler_connect(sh, "reconnect", onReconnect, this);
		signal_handler_connect(sh, "reconnect_success", onReconnected, this);
	}
	obs_output_set_service(m_stream, m_service);
	m_lastOutTotal = m_lastOutDropped = 0;
	return startWithFallback(m_stream, error);
}

void ObsCore::stopStream()
{
	if (m_stream && obs_output_active(m_stream))
		obs_output_stop(m_stream);
}

bool ObsCore::startRecording(const QString &folder, QString *error)
{
	if (recording())
		return true;
	if (!QDir().mkpath(folder)) {
		*error = "The recording folder cannot be used. Pick another one in Settings > Recording.";
		return false;
	}
	if (!ensureEncoders(error))
		return false;
	m_recordPath = QDir(folder).filePath(QDateTime::currentDateTime().toString("yyyy-MM-dd hh-mm-ss") + ".mp4");
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "path", QDir::toNativeSeparators(m_recordPath).toUtf8().constData());
	if (!m_record) {
		// Hybrid MP4: plays everywhere, and a crash or power cut does not ruin the file.
		m_record = obs_output_create("mp4_output", "Zunimi recording", s, nullptr);
		signal_handler_t *sh = obs_output_get_signal_handler(m_record);
		signal_handler_connect(sh, "start", onRecordStart, this);
		signal_handler_connect(sh, "stop", onRecordStop, this);
	} else {
		obs_output_update(m_record, s);
	}
	obs_data_release(s);
	return startWithFallback(m_record, error);
}

void ObsCore::stopRecording()
{
	if (m_record && obs_output_active(m_record))
		obs_output_stop(m_record);
}

bool ObsCore::streaming() const
{
	return m_stream && obs_output_active(m_stream);
}

bool ObsCore::recording() const
{
	return m_record && obs_output_active(m_record);
}

// libobs calls these on its own threads; everything crosses to the UI thread.
void ObsCore::onStreamStart(void *data, calldata_t *)
{
	auto *self = static_cast<ObsCore *>(data);
	QMetaObject::invokeMethod(self, [self] { emit self->streamingChanged(true); }, Qt::QueuedConnection);
}

void ObsCore::onStreamStop(void *data, calldata_t *cd)
{
	auto *self = static_cast<ObsCore *>(data);
	const int code = int(calldata_int(cd, "code"));
	const QString why = code == OBS_OUTPUT_SUCCESS ? QString()
						       : stopReason(code, obs_output_get_last_error(self->m_stream));
	QMetaObject::invokeMethod(
		self,
		[self, why] {
			emit self->streamingChanged(false);
			if (!why.isEmpty())
				emit self->problem(why);
		},
		Qt::QueuedConnection);
}

void ObsCore::onReconnect(void *data, calldata_t *)
{
	auto *self = static_cast<ObsCore *>(data);
	QMetaObject::invokeMethod(
		self, [self] { emit self->notice("Internet hiccup. Reconnecting the stream..."); },
		Qt::QueuedConnection);
}

void ObsCore::onReconnected(void *data, calldata_t *)
{
	auto *self = static_cast<ObsCore *>(data);
	QMetaObject::invokeMethod(self, [self] { emit self->notice("Back live."); }, Qt::QueuedConnection);
}

void ObsCore::onRecordStart(void *data, calldata_t *)
{
	auto *self = static_cast<ObsCore *>(data);
	QMetaObject::invokeMethod(
		self, [self] { emit self->recordingChanged(true, self->m_recordPath); }, Qt::QueuedConnection);
}

void ObsCore::onRecordStop(void *data, calldata_t *cd)
{
	auto *self = static_cast<ObsCore *>(data);
	const int code = int(calldata_int(cd, "code"));
	const QString why = code == OBS_OUTPUT_SUCCESS ? QString()
						       : stopReason(code, obs_output_get_last_error(self->m_record));
	const QString file = self->m_recordPath;
	QMetaObject::invokeMethod(
		self,
		[self, why, file] {
			emit self->recordingChanged(false, file);
			if (!why.isEmpty())
				emit self->problem(why);
		},
		Qt::QueuedConnection);
}

// ---- health
Health ObsCore::health()
{
	Health h;
	h.fps = obs_get_active_fps();
	if (m_cpu)
		h.cpu = os_cpu_usage_info_query(m_cpu);
	PROCESS_MEMORY_COUNTERS mem{};
	if (GetProcessMemoryInfo(GetCurrentProcess(), &mem, sizeof(mem)))
		h.ramMb = double(mem.WorkingSetSize) / (1024.0 * 1024.0);

	const uint32_t total = obs_get_total_frames(), lagged = obs_get_lagged_frames();
	h.renderLag = percent(lagged - m_lastLagged, total - m_lastTotal);
	m_lastTotal = total;
	m_lastLagged = lagged;

	video_t *video = obs_get_video();
	const uint32_t encTotal = video_output_get_total_frames(video), encSkipped = video_output_get_skipped_frames(video);
	h.encodeLag = busy() ? percent(encSkipped - m_lastEncSkipped, encTotal - m_lastEncTotal) : 0.0;
	m_lastEncTotal = encTotal;
	m_lastEncSkipped = encSkipped;

	if (streaming()) {
		const int outTotal = obs_output_get_total_frames(m_stream), outDropped = obs_output_get_frames_dropped(m_stream);
		h.dropped = percent(uint64_t(std::max(0, outDropped - m_lastOutDropped)),
				    uint64_t(std::max(0, outTotal - m_lastOutTotal)));
		m_lastOutTotal = outTotal;
		m_lastOutDropped = outDropped;
	}

	// The worst problem wins, explained in words a beginner understands.
	h.text = busy() ? "All smooth" : "Ready";
	auto flag = [&h](double value, const QString &warn, const QString &bad) {
		const int level = value >= 5.0 ? 2 : value >= 1.0 ? 1 : 0;
		if (level > h.level) {
			h.level = level;
			h.text = level == 2 ? bad : warn;
		}
	};
	flag(h.renderLag, "Your graphics card is busy. Capping your game's FPS gives the stream room.",
	     "Your graphics card is overloaded. Cap your game's FPS or use the Potato profile.");
	flag(h.encodeLag, "The encoder is struggling. A lighter profile would help.",
	     "Your PC can't encode fast enough. Switch to Potato or a hardware encoder in Settings.");
	flag(h.dropped, "Your internet dropped a few frames.",
	     "Your internet is dropping frames. Lower the bitrate in Settings > Video.");
	return h;
}

void ObsCore::shutdown()
{
	if (!m_started)
		return;
	stopStream();
	stopRecording();
	save();
	obs_set_output_source(0, nullptr);
	obs_set_output_source(1, nullptr);
	obs_set_output_source(3, nullptr);
	if (m_stream)
		obs_output_release(m_stream);
	if (m_record)
		obs_output_release(m_record);
	if (m_service)
		obs_service_release(m_service);
	m_stream = m_record = nullptr;
	m_service = nullptr;
	if (m_video)
		obs_encoder_release(m_video);
	if (m_audio)
		obs_encoder_release(m_audio);
	m_video = m_audio = nullptr;
	if (m_micFilter)
		obs_source_release(m_micFilter);
	if (m_mic)
		obs_source_release(m_mic);
	if (m_desktop)
		obs_source_release(m_desktop);
	if (m_sceneSource)
		obs_source_release(m_sceneSource);
	m_micFilter = m_mic = m_desktop = m_sceneSource = nullptr;
	m_scene = nullptr;
	if (m_cpu)
		os_cpu_usage_info_destroy(m_cpu);
	m_cpu = nullptr;
	obs_shutdown();
	m_started = false;
}
