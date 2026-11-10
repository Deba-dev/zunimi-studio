#pragma once
#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#include <obs.h>

#include "core/Hardware.h"

struct os_cpu_usage_info;

// Live health, in plain language. level: 0 smooth, 1 struggling, 2 failing.
struct Health {
	double fps = 0;
	double cpu = 0;        // this app's share of the whole CPU, in percent
	double ramMb = 0;      // this app's memory
	double renderLag = 0;  // percent of frames the GPU missed
	double encodeLag = 0;  // percent of frames the encoder skipped
	double dropped = 0;    // percent of frames the network dropped (live only)
	int level = 0;
	QString text;
};

struct SceneItem {
	QString name;
	QString kind; // libobs source id
	bool visible = true;
};

// Owns libobs: startup, the modules Zunimi allows, one scene, audio, encoders and the two outputs.
class ObsCore : public QObject {
	Q_OBJECT
public:
	enum class Fit { Fill, Corner, Center };

	explicit ObsCore(QObject *parent = nullptr);
	~ObsCore() override;

	bool start(QString *error);
	void shutdown();

	const Hardware &hardware() const { return m_hw; }
	const VideoPlan &plan() const { return m_plan; }
	bool applyPlan(const VideoPlan &plan, QString *error); // only while not live or recording
	QSize canvas() const { return {m_plan.width, m_plan.height}; }
	QStringList modules() const { return m_modules; }

	// scene
	QVector<SceneItem> items() const; // top first
	obs_source_t *addSource(const char *kind, const QString &name, obs_data_t *settings, Fit fit);
	void removeSource(const QString &name);
	void setVisible(const QString &name, bool visible);
	void moveSource(const QString &name, bool up);
	void save();

	// audio
	obs_source_t *desktopAudio() const { return m_desktop; }
	obs_source_t *microphone() const { return m_mic; }
	void setMicCleanup(bool on);

	// outputs
	bool startStream(const QString &server, const QString &key, QString *error);
	void stopStream();
	bool startRecording(const QString &folder, QString *error);
	void stopRecording();
	bool streaming() const;
	bool recording() const;
	bool busy() const { return streaming() || recording(); }

	Health health();

signals:
	void streamingChanged(bool on);
	void recordingChanged(bool on, const QString &file);
	void problem(const QString &message);
	void notice(const QString &message);
	void sceneChanged();

private:
	bool loadModules(QString *error);
	bool resetVideo(const VideoPlan &plan, QString *error);
	void createScene();
	void createAudio();
	bool ensureEncoders(QString *error);
	void releaseEncoders();
	void rescaleItems(double ratio);
	QString uniqueName(const QString &base) const;
	obs_sceneitem_t *find(const QString &name) const;
	bool startWithFallback(obs_output_t *output, QString *error);

	static void onStreamStart(void *data, calldata_t *cd);
	static void onStreamStop(void *data, calldata_t *cd);
	static void onReconnect(void *data, calldata_t *cd);
	static void onReconnected(void *data, calldata_t *cd);
	static void onRecordStart(void *data, calldata_t *cd);
	static void onRecordStop(void *data, calldata_t *cd);

	Hardware m_hw;
	VideoPlan m_plan;
	QStringList m_modules;
	bool m_started = false;
	obs_source_t *m_sceneSource = nullptr;
	obs_scene_t *m_scene = nullptr;
	obs_source_t *m_desktop = nullptr;
	obs_source_t *m_mic = nullptr;
	obs_source_t *m_micFilter = nullptr;
	obs_encoder_t *m_video = nullptr;
	obs_encoder_t *m_audio = nullptr;
	obs_service_t *m_service = nullptr;
	obs_output_t *m_stream = nullptr;
	obs_output_t *m_record = nullptr;
	QString m_recordPath;

	os_cpu_usage_info *m_cpu = nullptr;
	uint32_t m_lastTotal = 0, m_lastLagged = 0, m_lastEncTotal = 0, m_lastEncSkipped = 0;
	int m_lastOutTotal = 0, m_lastOutDropped = 0;
};
