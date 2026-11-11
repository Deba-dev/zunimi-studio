#include "core/ObsCore.h"
#include "core/I18n.h"

#include "core/Addons.h"
#include "core/Settings.h"
#include "overlays/Overlays.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <util/platform.h>

#include <algorithm>
#include <cmath>

namespace {
const char *DESKTOP_NAME = "Desktop audio";
const char *MIC_NAME = "Microphone";

// Only these modules are ever loaded. A module that is not listed costs no RAM and no CPU.
const QStringList CORE_MODULES = {"image-source", "obs-ffmpeg",      "obs-filters", "obs-nvenc",   "obs-outputs",
				  "obs-qsv11",    "obs-text",        "obs-transitions", "obs-x264",  "rtmp-services",
				  "win-capture",  "win-dshow",       "win-wasapi",  "obs-webrtc"};

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

QString nameOf(obs_source_t *source)
{
	return QString::fromUtf8(obs_source_get_name(source));
}
} // namespace

bool ItemState::operator==(const ItemState &o) const
{
	return name == o.name && memcmp(&info, &o.info, sizeof(info)) == 0 && memcmp(&crop, &o.crop, sizeof(crop)) == 0 &&
	       visible == o.visible && locked == o.locked && blend == o.blend && filter == o.filter;
}

ObsCore::ObsCore(QObject *parent) : QObject(parent) {}

ObsCore::~ObsCore()
{
	shutdown();
}

bool ObsCore::start(QString *error)
{
	const QString config = Settings::dataDir() + "/plugin_config";
	QDir().mkpath(config);
	if (!obs_startup(i18n::obsLocale().toUtf8().constData(), config.toUtf8().constData(), nullptr)) {
		*error = L("The video engine could not start.");
		return false;
	}
	m_started = true;

	obs_audio_info audio{};
	audio.samples_per_sec = 48000;
	audio.speakers = SPEAKERS_STEREO;
	if (!obs_reset_audio(&audio)) {
		*error = L("The audio engine could not start.");
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
	overlays::Engine::get().registerSource(); // Zunimi's own alerts, chat and widgets

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

	createAudio();
	applyMonitoringDevice();
	// 0.2 kept one scene file; it becomes the "Main" collection.
	QDir().mkpath(Settings::dataDir() + "/collections");
	const QString old = Settings::dataDir() + "/scene.json";
	if (QFile::exists(old) && !QFile::exists(collectionPath("Main"))) {
		QFile::rename(old, collectionPath("Main"));
		QFile::rename(old + ".bak", collectionPath("Main") + ".bak");
	}
	m_collection = s.value("scenes/collection", "Main").toString();
	if (!collections().contains(m_collection))
		m_collection = collections().isEmpty() ? "Main" : collections().first();
	loadScenes(collectionPath(m_collection));
	setMicCleanup(s.value("audio/micCleanup", true).toBool());
	m_cpu = os_cpu_usage_info_start();
	return true;
}

bool ObsCore::loadModules(QString *error)
{
	// The exe lives in bin/64bit; modules are core/<name>/<name>.dll with their data beside them.
	obs_add_module_path("../../core/%module%", "../../core/%module%/data");
	ModuleList list;
	list.allowed = CORE_MODULES;
	if (Addons::browserWanted())
		list.allowed << "obs-browser"; // the add-on: only when installed and switched on
	obs_find_modules2(openModule, &list);
	obs_post_load_modules();
	obs_log_loaded_modules();
	m_modules = list.loaded;
	if (!list.loaded.contains("win-capture") || !list.loaded.contains("obs-x264")) {
		*error = L("Some parts of Zunimi Studio are missing (") + list.failed.join(", ") +
			 L("). Reinstalling should fix it.");
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
				 ? L("Your graphics card does not support DirectX 11, which Zunimi Studio needs.")
				 : L("The graphics could not start. Updating your graphics driver usually fixes this.");
		return false;
	}
	m_plan = plan;
	return true;
}

bool ObsCore::applyPlan(const VideoPlan &plan, QString *error)
{
	if (busy()) {
		*error = L("Stop the stream, recording, replay buffer and virtual camera first.");
		return false;
	}
	const int oldWidth = m_plan.width;
	releaseEncoders();
	if (plan.width != m_plan.width || plan.height != m_plan.height || plan.fps != m_plan.fps) {
		if (!resetVideo(plan, error))
			return false;
		if (!m_scenes.isEmpty() && oldWidth > 0)
			rescaleItems(double(plan.width) / double(oldWidth));
	}
	m_plan = plan;
	if (m_vcanvas) { // the vertical canvas keeps its size, follows the frame rate
		obs_video_info ovi{};
		obs_get_video_info(&ovi);
		const QSize v = verticalSize();
		ovi.base_width = ovi.output_width = uint32_t(v.width());
		ovi.base_height = ovi.output_height = uint32_t(v.height());
		obs_canvas_reset_video(m_vcanvas, &ovi);
	}
	Settings &s = Settings::get();
	s.set("video/width", plan.width);
	s.set("video/height", plan.height);
	s.set("video/fps", plan.fps);
	s.set("video/bitrate", plan.bitrate);
	s.set("video/encoder", plan.encoder);
	return true;
}

// ---- scenes
QVector<QPair<QString, QString>> ObsCore::transitionKinds()
{
	return {{"fade_transition", L("Fade")},
		{"cut_transition", L("Cut")},
		{"swipe_transition", L("Swipe")},
		{"slide_transition", L("Slide")},
		{"fade_to_color_transition", L("Fade to colour")},
		{"wipe_transition", L("Luma wipe")},
		{"obs_stinger_transition", L("Stinger (your video)")}};
}

obs_source_t *ObsCore::sceneSource(const QString &name) const
{
	for (obs_source_t *s : m_scenes)
		if (nameOf(s) == name)
			return s;
	return nullptr;
}

QString ObsCore::collectionPath(const QString &name) const
{
	return Settings::dataDir() + "/collections/" + name + ".json";
}

obs_source_t *ObsCore::transitionFor(const QString &wanted)
{
	QString id = wanted;
	bool known = false;
	for (const auto &k : transitionKinds())
		known |= k.first == id;
	if (!known)
		id = "fade_transition";
	Settings &st = Settings::get();
	obs_data_t *s = obs_data_create();
	if (id == "obs_stinger_transition") {
		// A video that plays over the cut; the scene switches at the chosen moment.
		obs_data_set_string(s, "path", st.value("scenes/stingerPath").toString().toUtf8().constData());
		obs_data_set_int(s, "transition_point", st.value("scenes/stingerPoint", 500).toInt());
		obs_data_set_int(s, "audio_fade_style", 0);
	} else if (id == "fade_to_color_transition") {
		obs_data_set_int(s, "color", 0xFF000000);
	}
	for (obs_source_t *t : m_transitions) {
		if (QString::fromUtf8(obs_source_get_id(t)) == id) {
			obs_source_update(t, s);
			obs_data_release(s);
			return t;
		}
	}
	obs_source_t *t = obs_source_create_private(id.toUtf8().constData(), "Zunimi transition", s);
	obs_data_release(s);
	if (t)
		m_transitions << t;
	return t;
}

void ObsCore::loadScenes(const QString &path)
{
	Settings &st = Settings::get();
	m_transitionId = st.value("scenes/transition", "fade_transition").toString();
	m_transitionMs = std::clamp(st.value("scenes/transitionMs", 300).toInt(), 0, 20000);

	QStringList order;
	QString current, program;
	if (obs_data_t *root = obs_data_create_from_json_file_safe(path.toUtf8().constData(), "bak")) {
		if (obs_data_array_t *sources = obs_data_get_array(root, "sources")) {
			obs_load_sources(sources, nullptr, nullptr);
			obs_data_array_release(sources);
		}
		loadVertical(root);
		if (obs_data_array_t *names = obs_data_get_array(root, "scene_order")) {
			for (size_t i = 0; i < obs_data_array_count(names); i++) {
				obs_data_t *entry = obs_data_array_item(names, i);
				order << QString::fromUtf8(obs_data_get_string(entry, "name"));
				obs_data_release(entry);
			}
			obs_data_array_release(names);
		}
		current = QString::fromUtf8(obs_data_get_string(root, "current_scene"));
		program = QString::fromUtf8(obs_data_get_string(root, "current_program_scene"));
		const std::pair<const char *, obs_source_t *> audio[] = {{"desktop_filters", m_desktop},
									 {"mic_filters", m_mic}};
		for (const auto &[key, source] : audio) {
			obs_data_array_t *filters = obs_data_get_array(root, key);
			if (filters && source) {
				// the filters of the collection we came from go first
				obs_source_enum_filters(
					source,
					[](obs_source_t *parent, obs_source_t *filter, void *) {
						obs_source_filter_remove(parent, filter);
					},
					nullptr);
				obs_source_restore_filters(source, filters);
			}
			obs_data_array_release(filters);
		}
		obs_data_release(root);
	}
	for (const QString &name : order) {
		obs_source_t *source = obs_get_source_by_name(name.toUtf8().constData());
		if (source && obs_source_is_scene(source) && !obs_source_is_group(source) && source != m_vscene && !m_scenes.contains(source))
			m_scenes << source; // keeps the reference
		else
			obs_source_release(source);
	}
	obs_enum_scenes(
		[](void *param, obs_source_t *source) {
			auto *self = static_cast<ObsCore *>(param);
			obs_data_t *priv = obs_source_get_private_settings(source);
			const bool vertical = obs_data_get_bool(priv, "zunimi_vertical");
			obs_data_release(priv);
			if (!obs_source_is_group(source) && !vertical && source != self->m_vscene && !self->m_scenes.contains(source))
				self->m_scenes.append(obs_source_get_ref(source));
			return true;
		},
		this);
	for (obs_source_t *scene : m_scenes)
		if (nameOf(scene) == "Zunimi Main") // 0.1 had one hidden scene; it becomes the first visible one
			obs_source_set_name(scene, uniqueName("Main").toUtf8().constData());
	if (m_scenes.isEmpty())
		m_scenes << obs_scene_get_source(obs_scene_create("Main")); // the creation reference is ours

	if (!m_transition) {
		m_transition = transitionFor(m_transitionId);
		obs_set_output_source(0, m_transition);
	}
	obs_source_t *first = sceneSource(current);
	m_sceneSource = nullptr;
	m_program = nullptr;
	setCurrent(first ? first : m_scenes.first(), false);
	if (m_studio) {
		obs_source_t *onAir = sceneSource(program);
		startTransition(onAir ? onAir : m_sceneSource, false);
	}
}

void ObsCore::clearScenes()
{
	// Everything but the transition, desktop audio and mic goes; used when switching collections.
	obs_transition_set(m_transition, nullptr);
	if (m_editVertical)
		setEditVertical(false);
	destroyVertical();
	m_sceneSource = m_program = nullptr;
	m_scene = nullptr;
	QVector<obs_source_t *> scenes = m_scenes;
	m_scenes.clear();
	m_undo.clear();
	m_redo.clear();
	auto remove = [](void *param, obs_source_t *source) {
		auto *keep = static_cast<ObsCore *>(param);
		if (source != keep->m_desktop && source != keep->m_mic)
			obs_source_remove(source);
		return true;
	};
	obs_enum_scenes(remove, this);
	obs_enum_sources(remove, this);
	for (obs_source_t *s : scenes)
		obs_source_release(s);
	while (obs_wait_for_destroy_queue()) {
	}
	emit undoChanged();
}

void ObsCore::startTransition(obs_source_t *to, bool animate)
{
	m_program = to;
	followProgram(to);
	QString id = m_transitionId;
	int ms = m_transitionMs;
	const auto own = sceneTransition(nameOf(to));
	if (!own.first.isEmpty()) {
		id = own.first;
		ms = own.second;
	}
	obs_source_t *next = transitionFor(id);
	if (next && next != m_transition) {
		obs_transition_swap_begin(next, m_transition);
		obs_set_output_source(0, next);
		obs_transition_swap_end(next, m_transition);
		m_transition = next;
	}
	const bool fade = animate && (ms > 0 || id == "obs_stinger_transition") && id != "cut_transition";
	if (!fade || !obs_transition_start(m_transition, OBS_TRANSITION_MODE_AUTO, uint32_t(std::max(ms, 1)), to))
		obs_transition_set(m_transition, to);
}

void ObsCore::setCurrent(obs_source_t *source, bool animate)
{
	m_sceneSource = source;
	m_scene = obs_scene_from_source(source);
	if (!m_studio)
		startTransition(source, animate);
	emit scenesChanged();
	emit sceneChanged();
}

void ObsCore::setStudioMode(bool on)
{
	if (on == m_studio)
		return;
	m_studio = on;
	if (!on && m_sceneSource != m_program)
		startTransition(m_sceneSource, true); // what was being edited goes live
	Settings::get().set("scenes/studio", on);
	emit studioModeChanged(on);
	emit scenesChanged();
}

void ObsCore::transitionNow()
{
	if (m_sceneSource && m_sceneSource != m_program) {
		startTransition(m_sceneSource, true);
		save();
		emit scenesChanged();
	}
}

void ObsCore::setSceneTransition(const QString &scene, const QString &id, int ms)
{
	obs_source_t *source = sceneSource(scene);
	if (!source)
		return;
	obs_data_t *priv = obs_source_get_private_settings(source);
	obs_data_set_string(priv, "zunimi_transition", id.toUtf8().constData());
	obs_data_set_int(priv, "zunimi_transition_ms", std::clamp(ms, 0, 20000));
	obs_data_release(priv);
	save();
}

QPair<QString, int> ObsCore::sceneTransition(const QString &scene) const
{
	obs_source_t *source = sceneSource(scene);
	if (!source)
		return {};
	obs_data_t *priv = obs_source_get_private_settings(source);
	const QPair<QString, int> result{QString::fromUtf8(obs_data_get_string(priv, "zunimi_transition")),
					 int(obs_data_get_int(priv, "zunimi_transition_ms"))};
	obs_data_release(priv);
	return result;
}

QStringList ObsCore::scenes() const
{
	QStringList names;
	for (obs_source_t *s : m_scenes)
		names << nameOf(s);
	return names;
}

QString ObsCore::currentScene() const
{
	return m_sceneSource ? nameOf(m_sceneSource) : QString();
}

QString ObsCore::programScene() const
{
	return m_program ? nameOf(m_program) : QString();
}

void ObsCore::setScene(const QString &name)
{
	if (m_editVertical)
		setEditVertical(false); // picking a landscape scene goes back to editing landscape
	obs_source_t *source = sceneSource(name);
	if (!source || source == m_sceneSource)
		return;
	setCurrent(source, true);
	save();
}

QString ObsCore::addScene(const QString &name)
{
	const QString unique = uniqueName(name.trimmed().isEmpty() ? L("Scene") : name.trimmed());
	obs_source_t *source = obs_scene_get_source(obs_scene_create(unique.toUtf8().constData()));
	m_scenes << source;
	setCurrent(source, true);
	save();
	push(L("Add scene"), [this, unique] { removeScene(unique); }, [this, unique] { addScene(unique); });
	return unique;
}

QString ObsCore::duplicateScene(const QString &name)
{
	obs_source_t *from = sceneSource(name);
	if (!from)
		return {};
	const QString unique = uniqueName(name + L(" copy"));
	// Shared sources: the same webcam or game capture appears in both, which is what people expect.
	obs_scene_t *copy = obs_scene_duplicate(obs_scene_from_source(from), unique.toUtf8().constData(),
						OBS_SCENE_DUP_REFS);
	obs_source_t *source = obs_scene_get_source(copy);
	m_scenes.insert(m_scenes.indexOf(from) + 1, source);
	setCurrent(source, true);
	save();
	push(L("Copy scene"), [this, unique] { removeScene(unique); }, [this, name] { duplicateScene(name); });
	return unique;
}

bool ObsCore::renameScene(const QString &from, const QString &to)
{
	obs_source_t *source = sceneSource(from);
	const QString name = to.trimmed();
	if (!source || name.isEmpty())
		return false;
	if (name == from)
		return true;
	if (obs_source_t *taken = obs_get_source_by_name(name.toUtf8().constData())) {
		obs_source_release(taken);
		return false;
	}
	obs_source_set_name(source, name.toUtf8().constData());
	save();
	emit scenesChanged();
	push(L("Rename scene"), [this, from, name] { renameScene(name, from); }, [this, from, name] { renameScene(from, name); });
	return true;
}

bool ObsCore::removeScene(const QString &name)
{
	obs_source_t *source = sceneSource(name);
	if (!source || m_scenes.size() <= 1)
		return false;
	const int index = int(m_scenes.indexOf(source));
	QVector<obs_source_t *> used;
	obs_scene_enum_items(
		obs_scene_from_source(source),
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			static_cast<QVector<obs_source_t *> *>(param)->append(
				obs_source_get_ref(obs_sceneitem_get_source(item)));
			return true;
		},
		&used);

	// Undo brings back the scene and the sources only it used, exactly as they were.
	obs_data_t *sceneData = obs_save_source(source);
	obs_data_erase(sceneData, "uuid"); // the removed scene may still be finishing; the copy gets a new id
	obs_data_array_t *own = obs_data_array_create();
	for (obs_source_t *s : used) {
		if (!obs_source_is_scene(s) && !usedByAnotherScene(s, obs_scene_from_source(source))) {
			obs_data_t *d = obs_save_source(s);
			obs_data_erase(d, "uuid");
			obs_data_array_push_back(own, d);
			obs_data_release(d);
		}
	}

	if (source == m_sceneSource)
		setCurrent(m_scenes[index > 0 ? index - 1 : 1], false);
	if (source == m_program)
		startTransition(m_sceneSource, false);
	m_scenes.removeAt(index);
	obs_source_remove(source);
	obs_source_release(source);
	for (obs_source_t *s : used) { // sources only this scene used go with it
		if (!obs_source_is_scene(s) && !usedByAnotherScene(s, nullptr))
			obs_source_remove(s);
		obs_source_release(s);
	}
	save();
	emit scenesChanged();

	if (!m_replaying) {
		auto restoreScene = [this, sceneData, own, index] {
			for (size_t i = 0; i < obs_data_array_count(own); i++) {
				obs_data_t *d = obs_data_array_item(own, i);
				obs_source_t *exists = obs_get_source_by_name(obs_data_get_string(d, "name"));
				if (!exists)
					obs_source_release(obs_load_source(d));
				obs_source_release(exists);
				obs_data_release(d);
			}
			obs_source_t *back = obs_load_source(sceneData);
			if (!back)
				return;
			m_scenes.insert(std::min(index, int(m_scenes.size())), back);
			setCurrent(back, false);
			save();
		};
		auto shared = std::shared_ptr<void>(nullptr, [sceneData, own](void *) {
			obs_data_release(sceneData);
			obs_data_array_release(own);
		});
		push(L("Remove scene"), [restoreScene, shared] { restoreScene(); }, [this, name] { removeScene(name); });
	} else {
		obs_data_release(sceneData);
		obs_data_array_release(own);
	}
	return true;
}

void ObsCore::moveScene(const QString &name, bool up)
{
	obs_source_t *source = sceneSource(name);
	const int i = int(m_scenes.indexOf(source)), j = up ? i - 1 : i + 1;
	if (!source || j < 0 || j >= m_scenes.size())
		return;
	m_scenes.swapItemsAt(i, j);
	save();
	emit scenesChanged();
	push(L("Move scene"), [this, name, up] { moveScene(name, !up); }, [this, name, up] { moveScene(name, up); });
}

void ObsCore::setSceneOrder(const QStringList &names)
{
	const QStringList before = scenes();
	if (names == before || names.size() != before.size())
		return;
	QVector<obs_source_t *> order;
	for (const QString &n : names) {
		obs_source_t *source = sceneSource(n);
		if (!source || order.contains(source))
			return;
		order << source;
	}
	m_scenes = order;
	save();
	emit scenesChanged();
	push(L("Move scene"), [this, before] { setSceneOrder(before); }, [this, names] { setSceneOrder(names); });
}

void ObsCore::setTransition(const QString &id, int ms)
{
	m_transitionMs = std::clamp(ms, 0, 20000);
	m_transitionId = id;
	Settings::get().set("scenes/transitionMs", m_transitionMs);
	Settings::get().set("scenes/transition", id);
	transitionFor(id); // creates it now, so a missing stinger file shows up early in the log
}

bool ObsCore::usedByAnotherScene(obs_source_t *source, obs_scene_t *except) const
{
	const QByteArray name = obs_source_get_name(source);
	for (obs_source_t *s : m_scenes) {
		obs_scene_t *scene = obs_scene_from_source(s);
		if (scene != except && obs_scene_find_source_recursive(scene, name.constData()))
			return true;
	}
	if (m_vscene && obs_scene_from_source(m_vscene) != except &&
	    obs_scene_find_source_recursive(obs_scene_from_source(m_vscene), name.constData()))
		return true;
	return false;
}

// ---- audio
void ObsCore::createAudio()
{
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "device_id", "default");
	m_desktop = obs_source_create("wasapi_output_capture", DESKTOP_NAME, s, nullptr);
	m_mic = obs_source_create("wasapi_input_capture", MIC_NAME, s, nullptr);
	obs_data_release(s);
	if (m_desktop)
		obs_set_output_source(1, m_desktop);
	if (m_mic)
		obs_set_output_source(3, m_mic);
	applyAudioSettings(m_desktop, "desktop");
	applyAudioSettings(m_mic, "mic");
}

void ObsCore::applyAudioSettings(obs_source_t *source, const QString &key)
{
	if (!source)
		return;
	Settings &st = Settings::get();
	const QString k = "audio/" + key;
	obs_source_set_volume(source, float(st.value(k + "Volume", 1.0).toDouble()));
	obs_source_set_muted(source, st.value(k + "Muted", false).toBool());
	obs_source_set_sync_offset(source, st.value(k + "SyncMs", 0).toLongLong() * 1000000LL);
	obs_source_set_balance_value(source, float(st.value(k + "Balance", 0.5).toDouble()));
	obs_source_set_audio_mixers(source, st.value(k + "Tracks", 0x3F).toUInt());
	obs_source_set_monitoring_enabled(source, st.value(k + "Monitor", false).toBool());
}

void ObsCore::saveAudioSettings(obs_source_t *source)
{
	const QString key = source == m_desktop ? "desktop" : source == m_mic ? "mic" : QString();
	if (key.isEmpty()) {
		save();
		return;
	}
	Settings &st = Settings::get();
	const QString k = "audio/" + key;
	st.set(k + "Volume", double(obs_source_get_volume(source)));
	st.set(k + "Muted", obs_source_muted(source));
	st.set(k + "SyncMs", qlonglong(obs_source_get_sync_offset(source) / 1000000LL));
	st.set(k + "Balance", double(obs_source_get_balance_value(source)));
	st.set(k + "Tracks", obs_source_get_audio_mixers(source));
	st.set(k + "Monitor", obs_source_get_monitoring_enabled(source));
}

QVector<QPair<QString, QString>> ObsCore::monitoringDevices()
{
	QVector<QPair<QString, QString>> list;
	obs_enum_audio_monitoring_devices(
		[](void *param, const char *name, const char *id) {
			static_cast<QVector<QPair<QString, QString>> *>(param)->append(
				{QString::fromUtf8(name), QString::fromUtf8(id)});
			return true;
		},
		&list);
	return list;
}

void ObsCore::applyMonitoringDevice()
{
	Settings &st = Settings::get();
	const QString id = st.value("audio/monitorId", "default").toString();
	const QString name = st.value("audio/monitorName", "Default").toString();
	obs_set_audio_monitoring_device(name.toUtf8().constData(), id.toUtf8().constData());
}

bool ObsCore::micCleanup() const
{
	obs_source_t *filter = m_mic ? obs_source_get_filter_by_name(m_mic, "Mic cleanup") : nullptr;
	obs_source_release(filter);
	return filter != nullptr;
}

void ObsCore::setMicCleanup(bool on)
{
	// RNNoise: light, runs on the CPU, removes keyboard and fan noise from the mic.
	if (!m_mic)
		return;
	obs_source_t *filter = obs_source_get_filter_by_name(m_mic, "Mic cleanup");
	if (on && !filter) {
		obs_data_t *s = obs_data_create();
		obs_data_set_string(s, "method", "rnnoise");
		obs_source_t *created = obs_source_create_private("noise_suppress_filter", "Mic cleanup", s);
		obs_data_release(s);
		if (created) {
			obs_source_filter_add(m_mic, created);
			obs_source_release(created);
		}
	} else if (!on && filter) {
		obs_source_filter_remove(m_mic, filter);
	}
	obs_source_release(filter);
	Settings::get().set("audio/micCleanup", on);
	save();
}

QVector<obs_source_t *> ObsCore::audioSources() const
{
	QVector<obs_source_t *> list;
	if (m_desktop)
		list << m_desktop;
	if (m_mic)
		list << m_mic;
	if (!m_scene)
		return list;
	QVector<obs_source_t *> scene;
	obs_scene_enum_items(
		m_scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			obs_source_t *source = obs_sceneitem_get_source(item);
			const bool sound = (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) != 0;
			const QString id = QString::fromUtf8(obs_source_get_id(source));
			// a capture that is not sending sound (game/window audio off, a camera without a mic) is left out
			if (sound && (obs_source_audio_active(source) || id.startsWith("wasapi") || id == "ffmpeg_source"))
				static_cast<QVector<obs_source_t *> *>(param)->prepend(source);
			return true;
		},
		&scene);
	return list + scene;
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

// ---- items
obs_sceneitem_t *ObsCore::item(const QString &name) const
{
	// also inside groups
	return m_scene ? obs_scene_find_source_recursive(m_scene, name.toUtf8().constData()) : nullptr;
}

void ObsCore::place(obs_sceneitem_t *item, const QRectF &box)
{
	vec2 v{};
	obs_sceneitem_set_rot(item, 0.0f);
	obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	vec2_set(&v, 1.0f, 1.0f);
	obs_sceneitem_set_scale(item, &v);
	vec2_set(&v, float(box.width()), float(box.height()));
	obs_sceneitem_set_bounds(item, &v);
	vec2_set(&v, float(box.x()), float(box.y()));
	obs_sceneitem_set_pos(item, &v);
}

void ObsCore::fit(const QString &name, Fit how)
{
	const double w = canvas().width(), h = canvas().height();
	editItem(name, L("Size and place"), [&](obs_sceneitem_t *it) {
		switch (how) {
		case Fit::Fill:
			place(it, {0, 0, w, h});
			break;
		case Fit::Stretch:
			place(it, {0, 0, w, h});
			obs_sceneitem_set_bounds_type(it, OBS_BOUNDS_STRETCH);
			break;
		case Fit::Left:
			place(it, {0, 0, w / 2, h});
			break;
		case Fit::Right:
			place(it, {w / 2, 0, w / 2, h});
			break;
		case Fit::Corner: {
			const double bw = w * 0.28, bh = h * 0.28, margin = h * 0.03;
			place(it, {w - bw - margin, h - bh - margin, bw, bh});
			obs_sceneitem_set_bounds_alignment(it, OBS_ALIGN_BOTTOM | OBS_ALIGN_RIGHT);
			break;
		}
		case Fit::Center: {
			vec2 v{};
			obs_sceneitem_set_bounds_type(it, OBS_BOUNDS_NONE);
			obs_sceneitem_set_alignment(it, OBS_ALIGN_CENTER);
			vec2_set(&v, 1.0f, 1.0f);
			obs_sceneitem_set_scale(it, &v);
			vec2_set(&v, float(w / 2), float(h / 2));
			obs_sceneitem_set_pos(it, &v);
			break;
		}
		}
	});
}

namespace {
// Undo for "add": remove it again. Redo: bring back the same source (by its saved data) in the same place.
struct Saved {
	obs_data_t *data = nullptr;
	ItemState state;
	~Saved() { obs_data_release(data); }
};
} // namespace

obs_source_t *ObsCore::addSource(const char *kind, const QString &name, obs_data_t *settings, Fit how)
{
	const QString unique = uniqueName(name);
	obs_source_t *source = obs_source_create(kind, unique.toUtf8().constData(), settings, nullptr);
	if (!source)
		return nullptr;
	const bool was = m_replaying;
	m_replaying = true; // the fit below is part of adding, not its own step
	obs_scene_add(m_scene, source);
	obs_source_release(source); // the scene keeps its own reference
	fit(unique, how);
	m_replaying = was;
	emit sceneChanged();
	recordAdd(unique);
	return source;
}

void ObsCore::recordAdd(const QString &name)
{
	// Undo removes it, keeping a copy; redo brings back that same source in the same place.
	const QString scene = currentScene();
	auto saved = std::make_shared<Saved>();
	push(
		L("Add source"),
		[this, name, scene, saved] {
			setScene(scene);
			if (obs_sceneitem_t *it = item(name)) {
				obs_data_release(saved->data);
				saved->data = obs_save_source(obs_sceneitem_get_source(it));
				obs_data_erase(saved->data, "uuid");
				saved->state = itemState(it);
			}
			removeSource(name);
		},
		[this, name, scene, saved] {
			setScene(scene);
			obs_source_t *src = obs_get_source_by_name(name.toUtf8().constData());
			if (!src && saved->data)
				src = obs_load_source(saved->data);
			if (!src)
				return;
			obs_sceneitem_t *it = obs_scene_add(m_scene, src);
			obs_source_release(src);
			if (it && saved->data)
				setItemState(it, saved->state);
			save();
			emit sceneChanged();
		});
}

obs_source_t *ObsCore::addSourceAt(const char *kind, const QString &name, obs_data_t *settings, const QRectF &box)
{
	const QString unique = uniqueName(name);
	obs_source_t *source = obs_source_create(kind, unique.toUtf8().constData(), settings, nullptr);
	if (!source)
		return nullptr;
	place(obs_scene_add(m_scene, source), box);
	obs_source_release(source);
	save();
	emit sceneChanged();
	recordAdd(unique);
	return source;
}

QStringList ObsCore::otherScenesSources() const
{
	QStringList names;
	for (obs_source_t *s : m_scenes) {
		if (s == m_sceneSource)
			continue;
		obs_scene_enum_items(
			obs_scene_from_source(s),
			[](obs_scene_t *, obs_sceneitem_t *it, void *param) {
				obs_source_t *src = obs_sceneitem_get_source(it);
				if (obs_source_is_scene(src))
					return true;
				const QString n = nameOf(src);
				auto *list = static_cast<QStringList *>(param);
				if (!list->contains(n))
					list->append(n);
				return true;
			},
			&names);
	}
	names.erase(std::remove_if(names.begin(), names.end(), [this](const QString &n) { return item(n) != nullptr; }),
		    names.end());
	return names;
}

QStringList ObsCore::nestableScenes() const
{
	QStringList names;
	for (obs_source_t *s : m_scenes) {
		if (s == m_sceneSource || item(nameOf(s)))
			continue;
		// a scene that already shows this one would loop forever
		if (obs_scene_find_source_recursive(obs_scene_from_source(s), obs_source_get_name(m_sceneSource)))
			continue;
		names << nameOf(s);
	}
	return names;
}

bool ObsCore::addExisting(const QString &name)
{
	if (item(name))
		return false;
	obs_source_t *source = obs_get_source_by_name(name.toUtf8().constData());
	if (!source)
		return false;
	obs_transform_info info{};
	bool found = false;
	for (obs_source_t *s : m_scenes) { // same place as in the scene it comes from
		if (obs_sceneitem_t *other = obs_scene_find_source(obs_scene_from_source(s), name.toUtf8().constData())) {
			obs_sceneitem_get_info2(other, &info);
			found = true;
			break;
		}
	}
	obs_sceneitem_t *added = obs_scene_add(m_scene, source);
	obs_source_release(source);
	if (!added)
		return false; // a scene inside itself
	if (found)
		obs_sceneitem_set_info2(added, &info);
	else if (obs_source_is_scene(source))
		place(added, {0, 0, double(m_plan.width), double(m_plan.height)});
	save();
	emit sceneChanged();
	recordAdd(name);
	return true;
}

void ObsCore::removeSource(const QString &name)
{
	obs_sceneitem_t *it = item(name);
	if (!it)
		return;
	obs_source_t *source = obs_source_get_ref(obs_sceneitem_get_source(it));
	auto saved = std::make_shared<Saved>();
	saved->data = obs_save_source(source);
	obs_data_erase(saved->data, "uuid");
	saved->state = itemState(it);
	const QStringList order = [this] {
		QStringList names;
		for (const SceneItem &i : items())
			names << i.name;
		return names;
	}();
	const QString scene = currentScene();
	obs_sceneitem_remove(it);
	if (!obs_source_is_scene(source) && !usedByAnotherScene(source, nullptr))
		obs_source_remove(source);
	obs_source_release(source);
	save();
	emit sceneChanged();
	push(
		L("Remove source"),
		[this, saved, scene, name, order] {
			setScene(scene);
			obs_source_t *src = obs_get_source_by_name(name.toUtf8().constData());
			if (!src)
				src = obs_load_source(saved->data);
			if (!src)
				return;
			obs_sceneitem_t *back = obs_scene_add(m_scene, src);
			obs_source_release(src);
			if (!back)
				return;
			setItemState(back, saved->state);
			moveSourceTo(name, int(order.indexOf(name)));
			save();
			emit sceneChanged();
		},
		[this, scene, name] {
			setScene(scene);
			removeSource(name);
		});
}

bool ObsCore::renameSource(const QString &from, const QString &to)
{
	const QString name = to.trimmed();
	obs_sceneitem_t *it = item(from);
	if (!it || name.isEmpty())
		return false;
	if (name == from)
		return true;
	if (obs_source_t *taken = obs_get_source_by_name(name.toUtf8().constData())) {
		obs_source_release(taken);
		return false;
	}
	obs_source_t *src = obs_sceneitem_get_source(it);
	obs_source_set_name(src, name.toUtf8().constData());
	if (obs_source_is_scene(src))
		emit scenesChanged();
	save();
	emit sceneChanged();
	push(L("Rename"), [this, from, name] { renameSource(name, from); }, [this, from, name] { renameSource(from, name); });
	return true;
}

void ObsCore::setVisible(const QString &name, bool visible)
{
	editItem(name, visible ? L("Show") : L("Hide"), [visible](obs_sceneitem_t *it) { obs_sceneitem_set_visible(it, visible); });
}

void ObsCore::setLocked(const QString &name, bool locked)
{
	editItem(name, locked ? L("Lock") : L("Unlock"), [locked](obs_sceneitem_t *it) { obs_sceneitem_set_locked(it, locked); });
	emit sceneChanged();
}

void ObsCore::moveSource(const QString &name, bool up)
{
	editItem(name, L("Move"), [up](obs_sceneitem_t *it) {
		obs_sceneitem_set_order(it, up ? OBS_ORDER_MOVE_UP : OBS_ORDER_MOVE_DOWN);
	});
	emit sceneChanged();
}

void ObsCore::moveSourceTo(const QString &name, int indexFromTop)
{
	obs_sceneitem_t *it = item(name);
	if (!it || indexFromTop < 0)
		return;
	const int count = int(items().size());
	beginEdit(L("Move"));
	obs_sceneitem_set_order_position(it, std::clamp(count - 1 - indexFromTop, 0, count - 1));
	endEdit();
	emit sceneChanged();
}

void ObsCore::setSourceOrder(const QStringList &siblingsTopFirst)
{
	const int n = int(siblingsTopFirst.size());
	QVector<obs_sceneitem_t *> list;
	for (const QString &name : siblingsTopFirst) {
		obs_sceneitem_t *it = item(name);
		if (!it || (!list.isEmpty() && obs_sceneitem_get_scene(it) != obs_sceneitem_get_scene(list.first())))
			return; // not side by side
		list << it;
	}
	if (list.isEmpty())
		return;
	beginEdit(L("Move"));
	// placing each one at the bottom in turn, from the top one down, leaves them in exactly this order
	for (int i = 0; i < n; i++)
		obs_sceneitem_set_order(list[i], OBS_ORDER_MOVE_BOTTOM);
	endEdit();
	emit sceneChanged();
}

QVector<SceneItem> ObsCore::items() const
{
	QVector<SceneItem> list;
	if (!m_scene)
		return list;
	auto entry = [](obs_sceneitem_t *it, const QString &group) {
		obs_source_t *source = obs_sceneitem_get_source(it);
		SceneItem s{nameOf(source), QString::fromUtf8(obs_source_get_id(source)), obs_sceneitem_visible(it),
			    obs_sceneitem_locked(it)};
		s.isGroup = obs_sceneitem_is_group(it);
		s.group = group;
		return s;
	};
	struct Ctx {
		QVector<SceneItem> *out;
		decltype(entry) *make;
	} ctx{&list, &entry};
	// libobs lists bottom first; the list here is top first, each group followed by its own sources (top first)
	obs_scene_enum_items(
		m_scene,
		[](obs_scene_t *, obs_sceneitem_t *it, void *param) {
			auto *c = static_cast<Ctx *>(param);
			QVector<SceneItem> block{(*c->make)(it, {})};
			if (obs_sceneitem_is_group(it)) {
				struct Inner {
					QVector<SceneItem> *block;
					decltype(c->make) make;
					QString group;
				} inner{&block, c->make, block.first().name};
				obs_sceneitem_group_enum_items(
					it,
					[](obs_scene_t *, obs_sceneitem_t *child, void *p) {
						auto *in = static_cast<Inner *>(p);
						in->block->insert(1, (*in->make)(child, in->group));
						return true;
					},
					&inner);
			}
			for (int i = block.size() - 1; i >= 0; --i)
				c->out->prepend(block[i]);
			return true;
		},
		&ctx);
	return list;
}

QString ObsCore::groupItems(const QStringList &names, const QString &group)
{
	QVector<obs_sceneitem_t *> chosen;
	for (const QString &n : names) {
		obs_sceneitem_t *it = m_scene ? obs_scene_find_source(m_scene, n.toUtf8().constData()) : nullptr; // top level only
		if (it && !obs_sceneitem_is_group(it))
			chosen << it;
	}
	if (chosen.size() < 1)
		return {};
	const QString unique = uniqueName(group.trimmed().isEmpty() ? L("Group") : group.trimmed());
	if (!obs_scene_insert_group(m_scene, unique.toUtf8().constData(), chosen.data(), size_t(chosen.size())))
		return {};
	save();
	emit sceneChanged();
	push(L("Group"), [this, unique] { ungroup(unique); }, [this, names, unique] { groupItems(names, unique); });
	return unique;
}

bool ObsCore::ungroup(const QString &group)
{
	obs_sceneitem_t *it = m_scene ? obs_scene_find_source(m_scene, group.toUtf8().constData()) : nullptr;
	if (!it || !obs_sceneitem_is_group(it))
		return false;
	QStringList names;
	obs_sceneitem_group_enum_items(
		it,
		[](obs_scene_t *, obs_sceneitem_t *child, void *p) {
			static_cast<QStringList *>(p)->append(nameOf(obs_sceneitem_get_source(child)));
			return true;
		},
		&names);
	obs_sceneitem_group_ungroup(it);
	save();
	emit sceneChanged();
	push(L("Ungroup"), [this, names, group] { groupItems(names, group); }, [this, group] { ungroup(group); });
	return true;
}

void ObsCore::rescaleItems(double ratio)
{
	for (obs_source_t *s : m_scenes)
		obs_scene_enum_items(
			obs_scene_from_source(s),
			[](obs_scene_t *, obs_sceneitem_t *it, void *param) {
				const float r = float(*static_cast<double *>(param));
				vec2 v{};
				obs_sceneitem_get_pos(it, &v);
				vec2_mulf(&v, &v, r);
				obs_sceneitem_set_pos(it, &v);
				if (obs_sceneitem_get_bounds_type(it) != OBS_BOUNDS_NONE) {
					obs_sceneitem_get_bounds(it, &v);
					vec2_mulf(&v, &v, r);
					obs_sceneitem_set_bounds(it, &v);
				} else {
					obs_sceneitem_get_scale(it, &v);
					vec2_mulf(&v, &v, r);
					obs_sceneitem_set_scale(it, &v);
				}
				return true;
			},
			&ratio);
	save();
}

void ObsCore::save()
{
	if (!m_sceneSource || m_collection.isEmpty())
		return;
	obs_data_array_t *sources = obs_save_sources_filtered(
		[](void *param, obs_source_t *source) {
			const char *name = obs_source_get_name(source);
			return strcmp(name, DESKTOP_NAME) != 0 && strcmp(name, MIC_NAME) != 0 &&
			       source != static_cast<ObsCore *>(param)->m_vscene;
		},
		this);
	obs_data_t *root = obs_data_create();
	obs_data_set_string(root, "name", m_collection.toUtf8().constData());
	obs_data_set_array(root, "sources", sources);
	obs_data_array_t *order = obs_data_array_create();
	for (obs_source_t *s : m_scenes) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "name", obs_source_get_name(s));
		obs_data_array_push_back(order, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(root, "scene_order", order);
	obs_data_set_string(root, "current_scene", obs_source_get_name(m_editVertical && m_mainEdit ? m_mainEdit : m_sceneSource));
	saveVertical(root);
	if (m_program)
		obs_data_set_string(root, "current_program_scene", obs_source_get_name(m_program));
	const std::pair<const char *, obs_source_t *> audio[] = {{"desktop_filters", m_desktop}, {"mic_filters", m_mic}};
	for (const auto &[key, source] : audio) {
		if (!source)
			continue;
		obs_data_array_t *filters = obs_source_backup_filters(source);
		obs_data_set_array(root, key, filters);
		obs_data_array_release(filters);
	}
	obs_data_save_json_safe(root, collectionPath(m_collection).toUtf8().constData(), "tmp", "bak");
	obs_data_array_release(order);
	obs_data_array_release(sources);
	obs_data_release(root);
}

QVector<QPair<QString, QString>> ObsCore::screens() const
{
	QVector<QPair<QString, QString>> list;
	obs_properties_t *props = obs_get_source_properties("monitor_capture");
	obs_property_t *p = props ? obs_properties_get(props, "monitor_id") : nullptr;
	for (size_t i = 0; p && i < obs_property_list_item_count(p); i++) {
		const QString id = QString::fromUtf8(obs_property_list_item_string(p, i));
		if (!id.isEmpty() && id != "DUMMY")
			list.append({QString::fromUtf8(obs_property_list_item_name(p, i)), id});
	}
	if (props)
		obs_properties_destroy(props);
	return list;
}

void ObsCore::shutdown()
{
	if (!m_started)
		return;
	stopStream();
	stopRecording();
	stopReplay();
	stopVirtualCam();
	stopVerticalOutputs(true, true);
	save();
	if (m_editVertical)
		setEditVertical(false);
	destroyVertical();
	overlays::Engine::get().shutdown();
	obs_set_output_source(0, nullptr);
	obs_set_output_source(1, nullptr);
	obs_set_output_source(3, nullptr);
	for (obs_output_t **o : {&m_stream, &m_record, &m_replay, &m_vcam}) {
		if (*o)
			obs_output_release(*o);
		*o = nullptr;
	}
	if (m_service)
		obs_service_release(m_service);
	m_service = nullptr;
	if (m_video)
		obs_encoder_release(m_video);
	if (m_recVideo)
		obs_encoder_release(m_recVideo);
	if (m_opus)
		obs_encoder_release(m_opus);
	for (obs_encoder_t *&a : m_audio) {
		if (a)
			obs_encoder_release(a);
		a = nullptr;
	}
	m_video = m_recVideo = m_opus = nullptr;
	if (m_mic)
		obs_source_release(m_mic);
	if (m_desktop)
		obs_source_release(m_desktop);
	for (obs_source_t *t : m_transitions)
		obs_source_release(t);
	m_transitions.clear();
	for (obs_source_t *scene : m_scenes)
		obs_source_release(scene);
	m_scenes.clear();
	m_undo.clear();
	m_redo.clear();
	m_mic = m_desktop = m_transition = m_sceneSource = m_program = nullptr;
	m_scene = nullptr;
	if (m_cpu)
		os_cpu_usage_info_destroy(m_cpu);
	m_cpu = nullptr;
	// The same teardown as OBS: remove every scene and source, then let libobs's destroy thread finish before
	// shutting down, so nothing is freed twice (a race that showed up when quitting right after starting).
	auto remove = [](void *, obs_source_t *source) {
		obs_source_remove(source);
		return true;
	};
	obs_enum_scenes(remove, nullptr);
	obs_enum_sources(remove, nullptr);
	while (obs_wait_for_destroy_queue()) {
	}
	obs_shutdown();
	m_started = false;
}
