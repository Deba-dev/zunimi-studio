// The vertical canvas: a second 9:16 picture (Shorts, Reels, TikTok) made from the same sources, with its own
// stream and recording. Built on libobs canvases, so it is a real second mix, not a crop of the encoded video.
#include "core/ObsCore.h"

#include "core/Settings.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

namespace {
const char *VSCENE = "Zunimi Vertical";
}

QSize ObsCore::verticalSize() const
{
	// 720x1280 on Potato and 720p canvases, 1080x1920 otherwise
	const bool small = m_plan.height <= 720 || Settings::get().value("video/profile").toString() == "potato";
	const QString chosen = Settings::get().value("vertical/size", small ? "720" : "1080").toString();
	return chosen == "720" ? QSize(720, 1280) : QSize(1080, 1920);
}

bool ObsCore::createVerticalCanvas(QString *error)
{
	if (m_vcanvas)
		return true;
	obs_video_info ovi{};
	obs_get_video_info(&ovi);
	const QSize size = verticalSize();
	ovi.base_width = ovi.output_width = uint32_t(size.width());
	ovi.base_height = ovi.output_height = uint32_t(size.height());
	m_vcanvas = obs_canvas_create("Vertical", &ovi, PROGRAM);
	if (!m_vcanvas) {
		if (error)
			*error = "This PC could not make a second canvas.";
		return false;
	}
	return true;
}

bool ObsCore::setVertical(bool on, QString *error)
{
	if (on == verticalOn())
		return true;
	if (!on) {
		if (m_vstream && obs_output_active(m_vstream))
			obs_output_stop(m_vstream);
		if (m_vrecord && obs_output_active(m_vrecord))
			obs_output_stop(m_vrecord);
		if (m_editVertical)
			setEditVertical(false);
		destroyVertical();
		Settings::get().set("vertical/on", false);
		save();
		emit verticalChanged(false);
		return true;
	}
	if (!createVerticalCanvas(error))
		return false;
	obs_scene_t *scene = obs_canvas_scene_create(m_vcanvas, VSCENE);
	m_vscene = obs_scene_get_source(scene);
	obs_data_t *priv = obs_source_get_private_settings(m_vscene);
	obs_data_set_bool(priv, "zunimi_vertical", true);
	obs_data_release(priv);
	// to begin with: whatever scene is on air, centre-cropped to fill 9:16, following scene switches
	if (m_program) {
		obs_sceneitem_t *it = obs_scene_add(scene, m_program);
		if (it) {
			const QSize size = verticalSize();
			obs_sceneitem_set_alignment(it, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
			obs_sceneitem_set_bounds_type(it, OBS_BOUNDS_SCALE_OUTER);
			obs_sceneitem_set_bounds_alignment(it, OBS_ALIGN_CENTER);
			vec2 v{};
			vec2_set(&v, float(size.width()), float(size.height()));
			obs_sceneitem_set_bounds(it, &v);
			vec2_zero(&v);
			obs_sceneitem_set_pos(it, &v);
			obs_data_t *ip = obs_sceneitem_get_private_settings(it);
			obs_data_set_bool(ip, "zunimi_follow", true);
			obs_data_release(ip);
		}
	}
	obs_canvas_set_channel(m_vcanvas, 0, m_vscene);
	Settings::get().set("vertical/on", true);
	save();
	emit verticalChanged(true);
	return true;
}

void ObsCore::destroyVertical()
{
	if (m_vvideo) {
		obs_encoder_release(m_vvideo);
		m_vvideo = nullptr;
	}
	for (obs_output_t **o : {&m_vstream, &m_vrecord}) {
		if (*o)
			obs_output_release(*o);
		*o = nullptr;
	}
	if (m_vservice) {
		obs_service_release(m_vservice);
		m_vservice = nullptr;
	}
	if (m_vcanvas) {
		obs_canvas_set_channel(m_vcanvas, 0, nullptr);
		if (m_vscene)
			obs_source_remove(m_vscene);
		obs_canvas_remove(m_vcanvas);
		obs_canvas_release(m_vcanvas);
	}
	if (m_vscene)
		obs_source_release(m_vscene);
	m_vscene = nullptr;
	m_vcanvas = nullptr;
}

void ObsCore::loadVertical(obs_data_t *root)
{
	// The vertical scene is saved with the other sources (so what only it shows loads with it); here it moves
	// onto its own canvas.
	obs_data_t *v = obs_data_get_obj(root, "vertical");
	if (!v)
		return;
	const QString name = QString::fromUtf8(obs_data_get_string(v, "name"));
	obs_data_release(v);
	obs_source_t *scene = obs_get_source_by_name((name.isEmpty() ? QString(VSCENE) : name).toUtf8().constData());
	QString error;
	if (!scene || !obs_source_is_scene(scene) || !createVerticalCanvas(&error)) {
		obs_source_release(scene);
		return;
	}
	// Rebuild it on the vertical canvas (a scene born there, not moved, so shutdown stays clean), item by item.
	obs_source_set_name(scene, (QString(VSCENE) + " (loading)").toUtf8().constData());
	obs_scene_t *fresh = obs_canvas_scene_create(m_vcanvas, VSCENE);
	m_vscene = obs_scene_get_source(fresh);
	obs_data_t *priv = obs_source_get_private_settings(m_vscene);
	obs_data_set_bool(priv, "zunimi_vertical", true);
	obs_data_release(priv);
	obs_scene_enum_items(
		obs_scene_from_source(scene),
		[](obs_scene_t *, obs_sceneitem_t *it, void *param) {
			obs_sceneitem_t *copy = obs_scene_add(static_cast<obs_scene_t *>(param), obs_sceneitem_get_source(it));
			if (!copy)
				return true;
			obs_transform_info info{};
			obs_sceneitem_get_info2(it, &info);
			obs_sceneitem_set_info2(copy, &info);
			obs_sceneitem_crop crop{};
			obs_sceneitem_get_crop(it, &crop);
			obs_sceneitem_set_crop(copy, &crop);
			obs_sceneitem_set_visible(copy, obs_sceneitem_visible(it));
			obs_sceneitem_set_locked(copy, obs_sceneitem_locked(it));
			obs_sceneitem_set_blending_mode(copy, obs_sceneitem_get_blending_mode(it));
			obs_sceneitem_set_scale_filter(copy, obs_sceneitem_get_scale_filter(it));
			obs_data_t *from = obs_sceneitem_get_private_settings(it);
			obs_data_t *to = obs_sceneitem_get_private_settings(copy);
			obs_data_apply(to, from);
			obs_data_release(from);
			obs_data_release(to);
			return true;
		},
		fresh);
	obs_source_remove(scene);
	obs_source_release(scene);
	obs_canvas_set_channel(m_vcanvas, 0, m_vscene);
}

void ObsCore::saveVertical(obs_data_t *root)
{
	if (!m_vscene)
		return;
	// with the other sources, so its own sources and widgets load in the same pass
	obs_data_array_t *sources = obs_data_get_array(root, "sources");
	obs_data_t *scene = obs_save_source(m_vscene);
	if (sources) {
		obs_data_array_push_back(sources, scene);
		obs_data_array_release(sources);
	}
	obs_data_release(scene);
	obs_data_t *v = obs_data_create();
	obs_data_set_string(v, "name", obs_source_get_name(m_vscene));
	obs_data_set_obj(root, "vertical", v);
	obs_data_release(v);
}

void ObsCore::setEditVertical(bool on)
{
	if (on && !m_vscene)
		return;
	if (on == m_editVertical)
		return;
	m_editVertical = on;
	if (on) {
		m_mainEdit = m_sceneSource;
		m_sceneSource = m_vscene;
		m_scene = obs_scene_from_source(m_vscene);
	} else {
		m_sceneSource = m_mainEdit ? m_mainEdit : m_program;
		m_scene = obs_scene_from_source(m_sceneSource);
	}
	emit verticalEditChanged(on);
	emit sceneChanged();
}

void ObsCore::followProgram(obs_source_t *program)
{
	// The vertical picture keeps showing whatever scene is on air: its "follow" item swaps to the new scene,
	// keeping its size and place.
	if (!m_vscene || !program)
		return;
	struct Ctx {
		obs_sceneitem_t *item = nullptr;
	} ctx;
	obs_scene_enum_items(
		obs_scene_from_source(m_vscene),
		[](obs_scene_t *, obs_sceneitem_t *it, void *param) {
			obs_data_t *p = obs_sceneitem_get_private_settings(it);
			if (obs_data_get_bool(p, "zunimi_follow"))
				static_cast<Ctx *>(param)->item = it;
			obs_data_release(p);
			return true;
		},
		&ctx);
	if (!ctx.item || obs_sceneitem_get_source(ctx.item) == program)
		return;
	obs_transform_info info{};
	obs_sceneitem_get_info2(ctx.item, &info);
	obs_sceneitem_crop crop{};
	obs_sceneitem_get_crop(ctx.item, &crop);
	const int64_t index = obs_sceneitem_get_order_position(ctx.item);
	obs_sceneitem_remove(ctx.item);
	obs_sceneitem_t *fresh = obs_scene_add(obs_scene_from_source(m_vscene), program);
	if (!fresh)
		return;
	obs_sceneitem_set_info2(fresh, &info);
	obs_sceneitem_set_crop(fresh, &crop);
	obs_sceneitem_set_order_position(fresh, int(index));
	obs_data_t *p = obs_sceneitem_get_private_settings(fresh);
	obs_data_set_bool(p, "zunimi_follow", true);
	obs_data_release(p);
}

bool ObsCore::ensureVerticalEncoder(QString *error)
{
	if (m_vvideo)
		return true;
	if (!ensureEncoders(error))
		return false;
	Settings &st = Settings::get();
	const QString id = encoderFor(m_plan.encoder, st.value("video/codec", "h264").toString());
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "rate_control", "CBR");
	obs_data_set_int(s, "bitrate", st.value("vertical/bitrate", std::max(2500, m_plan.bitrate * 3 / 4)).toInt());
	obs_data_set_int(s, "keyint_sec", 2);
	m_vvideo = obs_video_encoder_create(id.toUtf8().constData(), "Zunimi vertical video", s, nullptr);
	obs_data_release(s);
	if (!m_vvideo) {
		*error = "A second encoder for the vertical canvas isn't available on this PC.";
		return false;
	}
	obs_encoder_set_video(m_vvideo, obs_canvas_get_video(m_vcanvas));
	return true;
}

void ObsCore::startVerticalOutputs(bool stream, bool record, const QString &folder)
{
	if (!m_vcanvas)
		return;
	Settings &st = Settings::get();
	QString error;
	const int track = std::clamp(st.value("stream/track", 1).toInt(), 1, 6) - 1;
	if (stream && st.value("vertical/stream", false).toBool() && !st.value("vertical/server").toString().isEmpty()) {
		if (!ensureVerticalEncoder(&error)) {
			emit problem("Vertical stream: " + error);
		} else {
			obs_data_t *s = obs_data_create();
			obs_data_set_string(s, "server", st.value("vertical/server").toString().toUtf8().constData());
			obs_data_set_string(s, "key", st.secret("vertical/key").toUtf8().constData());
			if (!m_vservice)
				m_vservice = obs_service_create("rtmp_custom", "Zunimi vertical service", s, nullptr);
			else
				obs_service_update(m_vservice, s);
			obs_data_release(s);
			if (!m_vstream) {
				m_vstream = obs_output_create("rtmp_output", "Zunimi vertical stream", nullptr, nullptr);
				obs_output_set_reconnect_settings(m_vstream, 20, 3);
			}
			obs_output_set_service(m_vstream, m_vservice);
			obs_output_set_video_encoder(m_vstream, m_vvideo);
			obs_output_set_audio_encoder(m_vstream, m_audio[track], 0);
			if (!obs_output_start(m_vstream))
				emit problem("The vertical stream didn't start: " + QString::fromUtf8(obs_output_get_last_error(m_vstream)));
		}
	}
	if (record && st.value("vertical/record", true).toBool()) {
		if (!ensureVerticalEncoder(&error)) {
			emit problem("Vertical recording: " + error);
			return;
		}
		const QString path = QDir(folder).filePath(QFileInfo(m_recordPath).completeBaseName() + " (vertical).mp4");
		obs_data_t *s = obs_data_create();
		obs_data_set_string(s, "path", QDir::toNativeSeparators(path).toUtf8().constData());
		if (!m_vrecord)
			m_vrecord = obs_output_create("mp4_output", "Zunimi vertical recording", s, nullptr);
		else
			obs_output_update(m_vrecord, s);
		obs_data_release(s);
		obs_output_set_video_encoder(m_vrecord, m_vvideo);
		obs_output_set_audio_encoder(m_vrecord, m_audio[track], 0);
		if (!obs_output_start(m_vrecord))
			emit problem("The vertical recording didn't start.");
	}
}

void ObsCore::stopVerticalOutputs(bool stream, bool record)
{
	if (stream && m_vstream && obs_output_active(m_vstream))
		obs_output_stop(m_vstream);
	if (record && m_vrecord && obs_output_active(m_vrecord))
		obs_output_stop(m_vrecord);
}
