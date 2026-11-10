// Encoders and outputs: stream (RTMP/RTMPS, SRT, RIST, WHIP), recording, replay buffer, virtual camera,
// screenshots, live health and self-healing.
#include "core/ObsCore.h"
#include "core/I18n.h"

#include "core/Settings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>

#include <util/platform.h>

#include <algorithm>
#include <cmath>

#include <windows.h>
#include <psapi.h>
#include <shellapi.h>

namespace {
const char *VCAM_CLSID = "{A3FCE0F5-3493-419F-958A-ABA1250EC20B}"; // the OBS virtual camera's id

QString stopReason(int code, const char *detail)
{
	QString why;
	switch (code) {
	case OBS_OUTPUT_BAD_PATH:
		why = L("The server address is wrong. Check it in Settings > Stream.");
		break;
	case OBS_OUTPUT_CONNECT_FAILED:
		why = L("Could not reach the streaming server. Check your internet, or the server address.");
		break;
	case OBS_OUTPUT_INVALID_STREAM:
		why = L("The platform refused the stream key. Copy a fresh key from your dashboard.");
		break;
	case OBS_OUTPUT_DISCONNECTED:
		why = L("Your internet dropped and the stream could not reconnect.");
		break;
	case OBS_OUTPUT_NO_SPACE:
		why = L("The disk is full, so recording stopped. Free some space or pick another folder.");
		break;
	case OBS_OUTPUT_ENCODE_ERROR:
		why = L("The video encoder stopped working. Try the x264 (CPU) encoder in Settings > Video.");
		break;
	case OBS_OUTPUT_UNSUPPORTED:
		why = L("This encoder or format is not supported here. HEVC and AV1 need a platform that accepts them.");
		break;
	default:
		why = L("Something went wrong.");
	}
	if (detail && *detail)
		why += QString(" (%1)").arg(QString::fromUtf8(detail));
	return why;
}

// Speed vs quality, in each encoder's own words. 0 fastest, 1 balanced, 2 best quality.
void setPreset(obs_data_t *s, const QString &id, int level)
{
	if (id == "obs_x264") {
		const char *p[] = {"superfast", "veryfast", "faster"};
		obs_data_set_string(s, "preset", p[level]);
	} else if (id.startsWith("obs_nvenc")) {
		const char *p[] = {"p3", "p5", "p7"};
		obs_data_set_string(s, "preset2", p[level]);
	} else if (id.startsWith("obs_qsv11")) {
		const char *p[] = {"speed", "balanced", "quality"};
		obs_data_set_string(s, "target_usage", p[level]);
	} else if (id.contains("_amf")) {
		const char *p[] = {"speed", "balanced", "quality"};
		obs_data_set_string(s, "preset", p[level]);
	}
}

// Constant quality: x264 calls it CRF, the hardware encoders CQP. Lower is better and bigger.
void setQuality(obs_data_t *s, const QString &id, int q)
{
	if (id == "obs_x264") {
		obs_data_set_string(s, "rate_control", "CRF");
		obs_data_set_int(s, "crf", q);
	} else {
		obs_data_set_string(s, "rate_control", "CQP");
		obs_data_set_int(s, "cqp", q);
	}
}

obs_data_t *streamEncoderSettings(const VideoPlan &plan, const QString &id, bool potato)
{
	Settings &st = Settings::get();
	obs_data_t *s = obs_data_create();
	const QString rc = st.value("video/rateControl", "CBR").toString();
	if (rc == "CQP")
		setQuality(s, id, st.value("video/quality", 23).toInt());
	else
		obs_data_set_string(s, "rate_control", rc == "VBR" ? "VBR" : "CBR");
	obs_data_set_int(s, "bitrate", plan.bitrate);
	if (rc == "VBR")
		obs_data_set_int(s, "max_bitrate", plan.bitrate * 3 / 2);
	obs_data_set_int(s, "keyint_sec", std::clamp(st.value("video/keyint", 2).toInt(), 1, 10));
	if (id == "obs_x264" || id.contains("h264") || id == "obs_qsv11_v2")
		obs_data_set_string(s, "profile", "high");
	else
		obs_data_set_string(s, "profile", "main");
	const int level = potato ? 0 : std::clamp(st.value("video/preset", 1).toInt(), 0, 2);
	setPreset(s, id, level);
	if (id == "obs_x264" && potato) // fewer frames held in memory: a 4 GB laptop needs the RAM more than the last bit of quality
		obs_data_set_string(s, "x264opts", "rc-lookahead=10 ref=1 threads=4");
	const int bframes = st.value("video/bframes", -1).toInt();
	if (bframes >= 0)
		obs_data_set_int(s, "bf", bframes);
	return s;
}

double percent(uint64_t part, uint64_t whole)
{
	return whole ? 100.0 * double(part) / double(whole) : 0.0;
}

// A recording file name from the template: Qt date words (yyyy, MM, dd, hh, mm, ss) plus {scene} and {profile}.
QString fileName(const QString &pattern, const QString &scene)
{
	QString name = QDateTime::currentDateTime().toString(pattern.isEmpty() ? "yyyy-MM-dd hh-mm-ss" : pattern);
	name.replace("{scene}", scene);
	name.replace("{profile}", Settings::get().value("profile/current", "Default").toString());
	static const QString bad = "\\/:*?\"<>|";
	for (QChar &c : name)
		if (bad.contains(c))
			c = '-';
	return name.trimmed().isEmpty() ? "Recording" : name.trimmed();
}

struct Format {
	const char *key, *output, *ext;
};
const Format FORMATS[] = {
	{"hybrid_mp4", "mp4_output", "mp4"}, // survives a crash, plays everywhere
	{"hybrid_mov", "mov_output", "mov"},
	{"mkv", "ffmpeg_muxer", "mkv"},
	{"mp4", "ffmpeg_muxer", "mp4"},
	{"mov", "ffmpeg_muxer", "mov"},
	{"flv", "ffmpeg_muxer", "flv"},
	{"ts", "ffmpeg_muxer", "ts"},
};

const Format &recordFormat()
{
	const QString key = Settings::get().value("record/format", "hybrid_mp4").toString();
	for (const Format &f : FORMATS)
		if (key == f.key)
			return f;
	return FORMATS[0];
}

uint32_t recordTracks()
{
	const uint32_t t = Settings::get().value("record/tracks", 1).toUInt() & 0x3F;
	return t ? t : 1;
}
} // namespace

void ObsCore::outputSettingsChanged()
{
	if (!busy())
		releaseEncoders();
}

bool ObsCore::ensureEncoders(QString *error)
{
	Settings &st = Settings::get();
	const bool potato = st.value("video/profile").toString() == "potato";
	if (!m_video) {
		const QString id = encoderFor(m_plan.encoder, st.value("video/codec", "h264").toString());
		obs_data_t *s = streamEncoderSettings(m_plan, id, potato);
		m_video = obs_video_encoder_create(id.toUtf8().constData(), "Zunimi video", s, nullptr);
		obs_data_release(s);
		if (!m_video) {
			*error = L("The ") + encoderLabel(id) + L(" encoder is not available on this PC.");
			return false;
		}
		obs_encoder_set_video(m_video, obs_get_video());
		m_bitrateNow = m_plan.bitrate;
	}
	const int audioKbps = std::clamp(st.value("audio/bitrate", 160).toInt(), 64, 320);
	for (int i = 0; i < 6; i++) {
		if (m_audio[i])
			continue;
		obs_data_t *s = obs_data_create();
		obs_data_set_int(s, "bitrate", audioKbps);
		m_audio[i] = obs_audio_encoder_create("ffmpeg_aac", QString("Zunimi audio %1").arg(i + 1).toUtf8().constData(),
						      s, size_t(i), nullptr);
		obs_data_release(s);
		if (!m_audio[i]) {
			*error = L("The audio encoder is missing.");
			return false;
		}
		obs_encoder_set_audio(m_audio[i], obs_get_audio());
	}
	return true;
}

bool ObsCore::ensureRecordEncoders(QString *error)
{
	if (!ensureEncoders(error))
		return false;
	Settings &st = Settings::get();
	const bool own = st.value("record/quality", "same").toString() != "same";
	if (!own || m_recVideo)
		return true;
	// Recordings in their own, higher quality: a constant-quality encode independent of the stream's bitrate.
	const QString id = encoderFor(m_plan.encoder, st.value("record/codec", "h264").toString());
	obs_data_t *s = obs_data_create();
	const QString quality = st.value("record/quality").toString();
	setQuality(s, id, quality == "lossless" ? (id == "obs_x264" ? 12 : 14) : quality == "small" ? 26 : 20);
	obs_data_set_int(s, "keyint_sec", 2);
	setPreset(s, id, 1);
	m_recVideo = obs_video_encoder_create(id.toUtf8().constData(), "Zunimi recording video", s, nullptr);
	obs_data_release(s);
	if (!m_recVideo) {
		*error = L("The ") + encoderLabel(id) + L(" encoder could not make a second encode for recordings.");
		return false;
	}
	obs_encoder_set_video(m_recVideo, obs_get_video());
	return true;
}

void ObsCore::releaseEncoders()
{
	if (busy())
		return;
	for (obs_encoder_t **e : {&m_video, &m_recVideo, &m_opus}) {
		if (*e)
			obs_encoder_release(*e);
		*e = nullptr;
	}
	for (obs_encoder_t *&a : m_audio) {
		if (a)
			obs_encoder_release(a);
		a = nullptr;
	}
}

void ObsCore::attachEncoders(obs_output_t *output, bool record)
{
	const QString id = QString::fromUtf8(obs_output_get_id(output));
	if (id == "whip_output") {
		obs_output_set_video_encoder(output, m_video);
		obs_output_set_audio_encoder(output, m_opus, 0);
		return;
	}
	obs_output_set_video_encoder(output, record && m_recVideo ? m_recVideo : m_video);
	if (record) {
		// every chosen track becomes its own audio stream in the file (game, mic, Discord... apart)
		const uint32_t tracks = recordTracks();
		size_t index = 0;
		for (int i = 0; i < 6; i++)
			if (tracks & (1u << i))
				obs_output_set_audio_encoder(output, m_audio[i], index++);
	} else {
		const int track = std::clamp(Settings::get().value("stream/track", 1).toInt(), 1, 6) - 1;
		obs_output_set_audio_encoder(output, m_audio[track], 0);
	}
}

bool ObsCore::startWithFallback(obs_output_t *output, QString *error, bool record)
{
	attachEncoders(output, record);
	if (obs_output_start(output))
		return true;
	// A hardware encoder that fails (old driver, busy GPU) falls back to x264 instead of failing.
	if (m_plan.encoder != "obs_x264" && !busy()) {
		const QString failed = encoderLabel(m_plan.encoder);
		VideoPlan plan = m_plan;
		plan.encoder = "obs_x264";
		QString ignored;
		if (applyPlan(plan, &ignored) && (record ? ensureRecordEncoders(&ignored) : ensureEncoders(&ignored))) {
			attachEncoders(output, record);
			if (obs_output_start(output)) {
				emit notice(failed + L(" did not start, so Zunimi switched to x264 (CPU)."));
				return true;
			}
		}
	}
	const char *detail = obs_output_get_last_error(output);
	*error = detail && *detail ? QString::fromUtf8(detail) : L("The output could not start.");
	return false;
}

bool ObsCore::startStream(const QString &server, const QString &key, QString *error)
{
	if (streaming())
		return true;
	Settings &st = Settings::get();
	const bool whip = st.value("stream/platform").toString() == "whip";
	if (server.trimmed().isEmpty() || (!whip && key.trimmed().isEmpty())) {
		*error = L("Add your stream key first (Settings > Stream).");
		return false;
	}
	if (!ensureEncoders(error))
		return false;
	const QString url = server.trimmed();
	// RTMP and RTMPS go out as FLV; SRT and RIST as MPEG-TS; WHIP as WebRTC (lowest delay).
	const char *serviceId = whip ? "whip_custom" : "rtmp_custom";
	const char *outputId = whip                                                     ? "whip_output"
			       : url.startsWith("srt://") || url.startsWith("rist://") ? "ffmpeg_mpegts_muxer"
										 : "rtmp_output";
	if (whip && !moduleLoaded("obs-webrtc")) {
		*error = L("WHIP (WebRTC) is not available in this build.");
		return false;
	}
	if (whip && !m_opus) {
		obs_data_t *s = obs_data_create();
		obs_data_set_int(s, "bitrate", 160);
		m_opus = obs_audio_encoder_create("ffmpeg_opus", "Zunimi opus", s, 0, nullptr);
		obs_data_release(s);
		if (!m_opus) {
			*error = L("The Opus audio encoder WHIP needs is missing.");
			return false;
		}
		obs_encoder_set_audio(m_opus, obs_get_audio());
	}
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "server", url.toUtf8().constData());
	obs_data_set_string(s, whip ? "bearer_token" : "key", key.trimmed().toUtf8().constData());
	if (m_service && strcmp(obs_service_get_id(m_service), serviceId) != 0) {
		obs_service_release(m_service);
		m_service = nullptr;
	}
	if (m_service)
		obs_service_update(m_service, s);
	else
		m_service = obs_service_create(serviceId, "Zunimi service", s, nullptr);
	obs_data_release(s);
	if (m_stream && strcmp(obs_output_get_id(m_stream), outputId) != 0) {
		obs_output_release(m_stream);
		m_stream = nullptr;
	}
	if (!m_stream) {
		m_stream = obs_output_create(outputId, "Zunimi stream", nullptr, nullptr);
		if (!m_stream) {
			*error = L("This kind of server address is not supported.");
			return false;
		}
		obs_output_set_reconnect_settings(m_stream, 20, 3);
		signal_handler_t *sh = obs_output_get_signal_handler(m_stream);
		signal_handler_connect(sh, "start", onStreamStart, this);
		signal_handler_connect(sh, "stop", onStreamStop, this);
		signal_handler_connect(sh, "reconnect", onReconnect, this);
		signal_handler_connect(sh, "reconnect_success", onReconnected, this);
	}
	obs_output_set_service(m_stream, m_service);
	// A delay hides your screen from stream snipers; it keeps the delay across reconnects.
	const int delay = std::clamp(st.value("stream/delaySec", 0).toInt(), 0, 600);
	obs_output_set_delay(m_stream, uint32_t(delay), OBS_OUTPUT_DELAY_PRESERVE);
	m_lastOutTotal = m_lastOutDropped = 0;
	m_badSeconds = m_goodSeconds = 0;
	return startWithFallback(m_stream, error, false);
}

void ObsCore::stopStream()
{
	if (m_stream && obs_output_active(m_stream))
		obs_output_stop(m_stream);
	stopExtras();
	stopVerticalOutputs(true, false);
}

// ---- multistream
QVector<ObsCore::Destination> ObsCore::destinations()
{
	QVector<Destination> list;
	Settings &st = Settings::get();
	const QJsonArray arr = QJsonDocument::fromJson(st.value("multistream/list").toByteArray()).array();
	for (int i = 0; i < arr.size(); i++) {
		const QJsonObject o = arr[i].toObject();
		list.append({o.value("name").toString(), o.value("server").toString(), st.secret(QString("multistream/key%1").arg(i)),
			     o.value("on").toBool(true)});
	}
	return list;
}

void ObsCore::setDestinations(const QVector<Destination> &list)
{
	Settings &st = Settings::get();
	QJsonArray arr;
	for (int i = 0; i < list.size(); i++) {
		arr.append(QJsonObject{{"name", list[i].name}, {"server", list[i].server}, {"on", list[i].on}});
		st.setSecret(QString("multistream/key%1").arg(i), list[i].key);
	}
	st.set("multistream/list", QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

void ObsCore::startExtras()
{
	stopExtras();
	const int track = std::clamp(Settings::get().value("stream/track", 1).toInt(), 1, 6) - 1;
	for (const Destination &d : destinations()) {
		if (!d.on || d.server.trimmed().isEmpty())
			continue;
		Extra e;
		e.name = d.name.isEmpty() ? d.server : d.name;
		obs_data_t *s = obs_data_create();
		obs_data_set_string(s, "server", d.server.trimmed().toUtf8().constData());
		obs_data_set_string(s, "key", d.key.trimmed().toUtf8().constData());
		e.service = obs_service_create("rtmp_custom", ("Zunimi extra " + e.name).toUtf8().constData(), s, nullptr);
		obs_data_release(s);
		const bool ts = d.server.startsWith("srt://") || d.server.startsWith("rist://");
		e.output = obs_output_create(ts ? "ffmpeg_mpegts_muxer" : "rtmp_output", ("Zunimi extra " + e.name).toUtf8().constData(),
					     nullptr, nullptr);
		if (!e.service || !e.output) {
			e.error = L("not supported");
		} else {
			obs_output_set_reconnect_settings(e.output, 20, 3);
			obs_output_set_service(e.output, e.service);
			obs_output_set_video_encoder(e.output, m_video);
			obs_output_set_audio_encoder(e.output, m_audio[track], 0);
			if (!obs_output_start(e.output)) {
				const char *why = obs_output_get_last_error(e.output);
				e.error = why && *why ? QString::fromUtf8(why) : L("could not connect");
				emit problem(QString(L("Multistream to %1 didn't start: %2")).arg(e.name, e.error));
			}
		}
		m_extras << e;
	}
}

void ObsCore::stopExtras()
{
	for (Extra &e : m_extras) {
		if (e.output) {
			if (obs_output_active(e.output))
				obs_output_stop(e.output);
			obs_output_release(e.output);
		}
		if (e.service)
			obs_service_release(e.service);
	}
	m_extras.clear();
}

QStringList ObsCore::extraStatus() const
{
	QStringList out;
	for (const Extra &e : m_extras)
		out << e.name + ": " + (e.output && obs_output_active(e.output) ? QString("live") : "off" + (e.error.isEmpty() ? QString() : " (" + e.error + ")"));
	return out;
}

bool ObsCore::startRecording(const QString &folder, QString *error)
{
	if (recording())
		return true;
	if (!QDir().mkpath(folder)) {
		*error = L("The recording folder cannot be used. Pick another one in Settings > Recording.");
		return false;
	}
	if (!ensureRecordEncoders(error))
		return false;
	Settings &st = Settings::get();
	const Format &f = recordFormat();
	const QString base = fileName(st.value("record/nameFormat", "yyyy-MM-dd hh-mm-ss").toString(), programScene());
	m_recordPath = QDir(folder).filePath(base + "." + f.ext);
	for (int i = 2; QFileInfo::exists(m_recordPath); i++)
		m_recordPath = QDir(folder).filePath(QString("%1 (%2).%3").arg(base).arg(i).arg(f.ext));
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "path", QDir::toNativeSeparators(m_recordPath).toUtf8().constData());
	// Split long recordings into parts by time or size (0 = never).
	const int minutes = st.value("record/splitMinutes", 0).toInt(), mb = st.value("record/splitMb", 0).toInt();
	if (minutes > 0 || mb > 0) {
		obs_data_set_bool(s, "split_file", true);
		obs_data_set_int(s, "max_time_sec", minutes * 60);
		obs_data_set_int(s, "max_size_mb", mb);
		obs_data_set_bool(s, "allow_overwrite", false);
	}
	if (m_record && strcmp(obs_output_get_id(m_record), f.output) != 0) {
		obs_output_release(m_record);
		m_record = nullptr;
	}
	if (!m_record) {
		m_record = obs_output_create(f.output, "Zunimi recording", s, nullptr);
		signal_handler_t *sh = obs_output_get_signal_handler(m_record);
		signal_handler_connect(sh, "start", onRecordStart, this);
		signal_handler_connect(sh, "stop", onRecordStop, this);
	} else {
		obs_output_update(m_record, s);
	}
	obs_data_release(s);
	return startWithFallback(m_record, error, true);
}

void ObsCore::stopRecording()
{
	if (m_record && obs_output_active(m_record))
		obs_output_stop(m_record);
	stopVerticalOutputs(false, true);
}

bool ObsCore::pauseRecording(bool pause)
{
	if (!recording() || obs_output_paused(m_record) == pause)
		return false;
	if (!obs_output_pause(m_record, pause))
		return false;
	emit recordingPausedChanged(pause);
	return true;
}

bool ObsCore::recordingPaused() const
{
	return recording() && obs_output_paused(m_record);
}

bool ObsCore::startReplay(const QString &folder, int seconds, QString *error)
{
	if (replaying())
		return true;
	if (!QDir().mkpath(folder)) {
		*error = L("The replay folder cannot be used. Pick another one in Settings > Recording.");
		return false;
	}
	if (!ensureRecordEncoders(error))
		return false;
	seconds = std::clamp(seconds, 5, 600);
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "directory", QDir::toNativeSeparators(folder).toUtf8().constData());
	obs_data_set_string(s, "format", "Replay %CCYY-%MM-%DD %hh-%mm-%ss");
	obs_data_set_string(s, "extension", "mp4");
	obs_data_set_bool(s, "allow_spaces", true);
	obs_data_set_int(s, "max_time_sec", seconds);
	// memory cap: twice what the bitrate needs for that long, so the time limit is what normally applies
	const int kbps = Settings::get().value("record/quality", "same").toString() == "same" ? m_plan.bitrate : 40000;
	obs_data_set_int(s, "max_size_mb", std::max(64, (kbps + 160) * seconds / 8 / 1024 * 2));
	if (!m_replay) {
		m_replay = obs_output_create("replay_buffer", "Zunimi replay", s, nullptr);
		if (!m_replay) {
			obs_data_release(s);
			*error = L("The replay buffer is not available.");
			return false;
		}
		signal_handler_t *sh = obs_output_get_signal_handler(m_replay);
		signal_handler_connect(sh, "start", onReplayStart, this);
		signal_handler_connect(sh, "stop", onReplayStop, this);
		signal_handler_connect(sh, "saved", onReplaySaved, this);
	} else {
		obs_output_update(m_replay, s);
	}
	obs_data_release(s);
	return startWithFallback(m_replay, error, true);
}

void ObsCore::stopReplay()
{
	if (m_replay && obs_output_active(m_replay))
		obs_output_stop(m_replay);
}

void ObsCore::saveReplay()
{
	if (!replaying())
		return;
	calldata_t cd{};
	proc_handler_call(obs_output_get_proc_handler(m_replay), "save", &cd);
	calldata_free(&cd);
}

bool ObsCore::replaying() const
{
	return m_replay && obs_output_active(m_replay);
}

bool ObsCore::streaming() const
{
	return m_stream && obs_output_active(m_stream);
}

bool ObsCore::recording() const
{
	return m_record && obs_output_active(m_record);
}

namespace {
OutputStats statsOf(obs_output_t *output, qint64 startMs, qint64 &lastBytes, qint64 &lastMs)
{
	OutputStats s;
	if (!output || !obs_output_active(output))
		return s;
	s.active = true;
	s.bytes = qint64(obs_output_get_total_bytes(output));
	s.totalFrames = obs_output_get_total_frames(output);
	s.droppedFrames = obs_output_get_frames_dropped(output);
	s.congestion = int(std::lround(obs_output_get_congestion(output) * 100.0f));
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	s.seconds = startMs ? (now - startMs) / 1000 : 0;
	if (lastMs && now > lastMs)
		s.kbps = double(s.bytes - lastBytes) * 8.0 / double(now - lastMs);
	lastBytes = s.bytes;
	lastMs = now;
	return s;
}
} // namespace

OutputStats ObsCore::streamStats()
{
	static qint64 bytes = 0, ms = 0;
	return statsOf(m_stream, m_streamStart, bytes, ms);
}

OutputStats ObsCore::recordStats()
{
	static qint64 bytes = 0, ms = 0;
	return statsOf(m_record, m_recordStart, bytes, ms);
}

// ---- virtual camera
bool ObsCore::virtualCamInstalled()
{
	// registered for everyone (regsvr32 as admin) or just for this user
	for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
		HKEY key = nullptr;
		const QString path = QString("SOFTWARE\\Classes\\CLSID\\%1").arg(VCAM_CLSID);
		if (RegOpenKeyExW(root, reinterpret_cast<const wchar_t *>(path.utf16()), 0, KEY_READ, &key) == ERROR_SUCCESS) {
			RegCloseKey(key);
			return true;
		}
	}
	return false;
}

bool ObsCore::installVirtualCam(QString *error)
{
	// The camera driver ships with the app; Windows needs admin rights once to register it for every program.
	const QString dir = QDir(QCoreApplication::applicationDirPath() + "/../../core/win-dshow/data").absolutePath();
	QString args;
	for (const char *bits : {"64", "32"}) {
		const QString dll = QDir::toNativeSeparators(dir + QString("/obs-virtualcam-module%1.dll").arg(bits));
		if (QFileInfo::exists(dll))
			args += QString("regsvr32.exe /i /s \"%1\" & ").arg(dll);
	}
	if (args.isEmpty()) {
		*error = L("The virtual camera files are missing. Reinstalling Zunimi Studio should fix it.");
		return false;
	}
	args = "/c " + args + "exit";
	SHELLEXECUTEINFOW info{sizeof(info)};
	info.fMask = SEE_MASK_NOCLOSEPROCESS;
	info.lpVerb = L"runas";
	info.lpFile = L"cmd.exe";
	info.lpParameters = reinterpret_cast<const wchar_t *>(args.utf16());
	info.nShow = SW_HIDE;
	if (!ShellExecuteExW(&info)) {
		*error = L("Windows did not allow it. Click Yes when Windows asks, to install the virtual camera.");
		return false;
	}
	if (info.hProcess) {
		WaitForSingleObject(info.hProcess, 20000);
		CloseHandle(info.hProcess);
	}
	if (!virtualCamInstalled()) {
		*error = L("The virtual camera could not be installed.");
		return false;
	}
	return true;
}

bool ObsCore::startVirtualCam(QString *error)
{
	if (virtualCam())
		return true;
	if (!virtualCamInstalled() && !installVirtualCam(error))
		return false;
	if (!m_vcam) {
		m_vcam = obs_output_create("virtualcam_output", "Zunimi virtual camera", nullptr, nullptr);
		if (!m_vcam) {
			*error = L("The virtual camera is not available in this build.");
			return false;
		}
		signal_handler_connect(obs_output_get_signal_handler(m_vcam), "stop", onVirtualCamStop, this);
	}
	if (!obs_output_start(m_vcam)) {
		const char *detail = obs_output_get_last_error(m_vcam);
		*error = detail && *detail ? QString::fromUtf8(detail) : L("The virtual camera could not start.");
		return false;
	}
	emit virtualCamChanged(true);
	return true;
}

void ObsCore::stopVirtualCam()
{
	if (m_vcam && obs_output_active(m_vcam))
		obs_output_stop(m_vcam);
}

bool ObsCore::virtualCam() const
{
	return m_vcam && obs_output_active(m_vcam);
}

void ObsCore::onVirtualCamStop(void *data, calldata_t *)
{
	auto *self = static_cast<ObsCore *>(data);
	QMetaObject::invokeMethod(self, [self] { emit self->virtualCamChanged(false); }, Qt::QueuedConnection);
}

// ---- screenshots (drawn on the graphics thread, saved on the UI thread)
namespace {
struct Shot {
	ObsCore *core;
	obs_source_t *source; // null: the whole program
	QString path;
	uint32_t cx, cy;
};

void takeShot(void *param)
{
	auto *shot = static_cast<Shot *>(param);
	QImage image;
	obs_enter_graphics();
	gs_texrender_t *tex = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(shot->cx, shot->cy, GS_RGBA);
	if (tex && stage && gs_texrender_begin(tex, shot->cx, shot->cy)) {
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, float(shot->cx), 0.0f, float(shot->cy), -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		if (shot->source)
			obs_source_video_render(shot->source);
		else
			obs_render_main_texture();
		gs_blend_state_pop();
		gs_texrender_end(tex);
		gs_stage_texture(stage, gs_texrender_get_texture(tex));
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(stage, &data, &linesize)) {
			image = QImage(int(shot->cx), int(shot->cy), QImage::Format_RGBA8888);
			for (uint32_t y = 0; y < shot->cy; y++)
				memcpy(image.scanLine(int(y)), data + y * linesize, shot->cx * 4);
			gs_stagesurface_unmap(stage);
		}
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(tex);
	obs_leave_graphics();
	obs_source_release(shot->source);
	ObsCore *core = shot->core;
	const QString path = shot->path;
	delete shot;
	QMetaObject::invokeMethod(
		core,
		[core, image, path] {
			if (!image.isNull() && image.save(path, "PNG"))
				emit core->screenshotSaved(path);
			else
				emit core->problem(L("The screenshot could not be saved."));
		},
		Qt::QueuedConnection);
}
} // namespace

void ObsCore::screenshot(const QString &folder, obs_source_t *source)
{
	QDir().mkpath(folder);
	auto *shot = new Shot{this, source ? obs_source_get_ref(source) : nullptr, {}, 0, 0};
	shot->cx = source ? obs_source_get_width(source) : uint32_t(m_plan.width);
	shot->cy = source ? obs_source_get_height(source) : uint32_t(m_plan.height);
	if (!shot->cx || !shot->cy) {
		obs_source_release(shot->source);
		delete shot;
		emit problem(L("That source has no picture yet."));
		return;
	}
	static int taken = 0; // two in the same moment (a scene and a source) must not share a name
	shot->path = QDir(folder).filePath(QString("Screenshot %1 (%2).png")
						   .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh-mm-ss"))
						   .arg(++taken));
	obs_queue_task(OBS_TASK_GRAPHICS, takeShot, shot, false);
}

// libobs calls these on its own threads; everything crosses to the UI thread.
void ObsCore::onStreamStart(void *data, calldata_t *)
{
	auto *self = static_cast<ObsCore *>(data);
	QMetaObject::invokeMethod(
		self,
		[self] {
			self->m_streamStart = QDateTime::currentMSecsSinceEpoch();
			self->startExtras();
			self->startVerticalOutputs(true, false, {});
			emit self->streamingChanged(true);
		},
		Qt::QueuedConnection);
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
			if (self->m_video && self->m_bitrateNow != self->m_plan.bitrate)
				self->setBitrate(self->m_plan.bitrate); // self-healing only lasts for that stream
			self->m_streamStart = 0;
			self->stopExtras();
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
		self, [self] { emit self->notice(L("Internet hiccup. Reconnecting the stream...")); },
		Qt::QueuedConnection);
}

void ObsCore::onReconnected(void *data, calldata_t *)
{
	auto *self = static_cast<ObsCore *>(data);
	QMetaObject::invokeMethod(self, [self] { emit self->notice(L("Back live.")); }, Qt::QueuedConnection);
}

void ObsCore::onRecordStart(void *data, calldata_t *)
{
	auto *self = static_cast<ObsCore *>(data);
	QMetaObject::invokeMethod(
		self,
		[self] {
			self->m_recordStart = QDateTime::currentMSecsSinceEpoch();
			self->startVerticalOutputs(false, true, QFileInfo(self->m_recordPath).absolutePath());
			emit self->recordingChanged(true, self->m_recordPath);
		},
		Qt::QueuedConnection);
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
			self->m_recordStart = 0;
			emit self->recordingChanged(false, file);
			if (!why.isEmpty())
				emit self->problem(why);
		},
		Qt::QueuedConnection);
}

void ObsCore::onReplayStart(void *data, calldata_t *)
{
	auto *self = static_cast<ObsCore *>(data);
	QMetaObject::invokeMethod(self, [self] { emit self->replayChanged(true); }, Qt::QueuedConnection);
}

void ObsCore::onReplayStop(void *data, calldata_t *cd)
{
	auto *self = static_cast<ObsCore *>(data);
	const int code = int(calldata_int(cd, "code"));
	const QString why = code == OBS_OUTPUT_SUCCESS ? QString()
						       : stopReason(code, obs_output_get_last_error(self->m_replay));
	QMetaObject::invokeMethod(
		self,
		[self, why] {
			emit self->replayChanged(false);
			if (!why.isEmpty())
				emit self->problem(why);
		},
		Qt::QueuedConnection);
}

void ObsCore::onReplaySaved(void *data, calldata_t *)
{
	auto *self = static_cast<ObsCore *>(data);
	QMetaObject::invokeMethod(
		self,
		[self] {
			if (!self->m_replay)
				return;
			calldata_t cd{};
			proc_handler_call(obs_output_get_proc_handler(self->m_replay), "get_last_replay", &cd);
			const QString path = QString::fromUtf8(calldata_string(&cd, "path"));
			calldata_free(&cd);
			emit self->replaySaved(path);
		},
		Qt::QueuedConnection);
}

void ObsCore::setBitrate(int kbps)
{
	if (!m_video)
		return;
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "bitrate", kbps);
	obs_encoder_update(m_video, s);
	obs_data_release(s);
	m_bitrateNow = kbps;
}

// Self-healing: when the internet can't keep up, lower the bitrate step by step instead of dropping frames,
// then raise it again once it has been steady for a minute. Never below 40% of the chosen bitrate.
void ObsCore::heal(Health &h)
{
	h.bitrate = m_bitrateNow ? m_bitrateNow : m_plan.bitrate;
	if (!streaming() || !Settings::get().value("stream/autoHeal", true).toBool() ||
	    Settings::get().value("video/rateControl", "CBR").toString() == "CQP") {
		m_badSeconds = m_goodSeconds = 0;
		return;
	}
	if (h.dropped >= 2.0) {
		m_badSeconds++;
		m_goodSeconds = 0;
	} else if (++m_goodSeconds >= 10) {
		m_badSeconds = 0;
	}
	const int floor = std::max(600, m_plan.bitrate * 40 / 100);
	if (m_badSeconds >= 4 && m_bitrateNow > floor) {
		setBitrate(std::max(floor, m_bitrateNow * 75 / 100));
		m_badSeconds = 0;
		emit notice(QString(L("Your internet is struggling, so Zunimi lowered the bitrate to %1 kbps to keep you "
				    "live. It goes back up when things settle."))
				    .arg(m_bitrateNow));
	} else if (m_goodSeconds >= 60 && m_bitrateNow < m_plan.bitrate) {
		setBitrate(std::min(m_plan.bitrate, m_bitrateNow * 115 / 100));
		m_goodSeconds = 0;
		if (m_bitrateNow == m_plan.bitrate)
			emit notice(QString(L("Internet is steady again: bitrate back to %1 kbps.")).arg(m_bitrateNow));
	}
	h.bitrate = m_bitrateNow;
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

	heal(h);
	// The worst problem wins, explained in words a beginner understands.
	h.text = busy() ? L("All smooth") : L("Ready");
	auto flag = [&h](double value, const QString &warn, const QString &bad) {
		const int level = value >= 5.0 ? 2 : value >= 1.0 ? 1 : 0;
		if (level > h.level) {
			h.level = level;
			h.text = level == 2 ? bad : warn;
		}
	};
	flag(h.renderLag, L("Your graphics card is busy. Capping your game's FPS gives the stream room."),
	     L("Your graphics card is overloaded. Cap your game's FPS or use the Potato profile."));
	flag(h.encodeLag, L("The encoder is struggling. A lighter profile would help."),
	     L("Your PC can't encode fast enough. Switch to Potato or a hardware encoder in Settings."));
	flag(h.dropped, L("Your internet dropped a few frames."),
	     L("Your internet is dropping frames. Lower the bitrate in Settings > Video."));
	if (h.level == 0 && streaming() && h.bitrate < m_plan.bitrate)
		h.text = QString(L("Live at a safer %1 kbps while your internet recovers")).arg(h.bitrate);
	return h;
}
