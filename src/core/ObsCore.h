#pragma once
#include <QObject>
#include <QPair>
#include <QRectF>
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
	int bitrate = 0;       // the encoder's bitrate right now (self-healing may lower it while live)
	int level = 0;
	QString text;
};

struct SceneItem {
	QString name;
	QString kind; // libobs source id
	bool visible = true;
	bool locked = false;
};

// Owns libobs: startup, the modules Zunimi allows, scenes and the transition between them, audio,
// encoders and the three outputs (stream, recording, replay buffer).
class ObsCore : public QObject {
	Q_OBJECT
public:
	enum class Fit { Fill, Stretch, Corner, Center, Left, Right };

	explicit ObsCore(QObject *parent = nullptr);
	~ObsCore() override;

	bool start(QString *error);
	void shutdown();

	const Hardware &hardware() const { return m_hw; }
	const VideoPlan &plan() const { return m_plan; }
	bool applyPlan(const VideoPlan &plan, QString *error); // only while nothing is encoding
	QSize canvas() const { return {m_plan.width, m_plan.height}; }
	QStringList modules() const { return m_modules; }

	// scenes
	QStringList scenes() const;
	QString currentScene() const;
	void setScene(const QString &name);
	QString addScene(const QString &name);
	QString duplicateScene(const QString &name);
	bool renameScene(const QString &from, const QString &to);
	bool removeScene(const QString &name); // the last scene always stays
	void moveScene(const QString &name, bool up);
	void setTransition(const QString &id, int ms);
	QString transitionId() const { return m_transitionId; }
	int transitionMs() const { return m_transitionMs; }

	// items of the current scene
	QVector<SceneItem> items() const; // top first
	obs_scene_t *scene() const { return m_scene; }
	obs_sceneitem_t *item(const QString &name) const;
	obs_source_t *addSource(const char *kind, const QString &name, obs_data_t *settings, Fit fit);
	obs_source_t *addSourceAt(const char *kind, const QString &name, obs_data_t *settings, const QRectF &box);
	bool addExisting(const QString &name);          // a source from another scene, shared
	QStringList otherScenesSources() const;         // sources not in this scene yet
	void removeSource(const QString &name);         // from this scene; deleted when no scene uses it
	bool renameSource(const QString &from, const QString &to);
	void setVisible(const QString &name, bool visible);
	void setLocked(const QString &name, bool locked);
	void moveSource(const QString &name, bool up);
	void fit(const QString &name, Fit fit);
	void place(obs_sceneitem_t *item, const QRectF &box); // fits the item inside box, keeping its shape
	void save();
	// Every screen Windows has: label ("DELL U2419: 1920x1080 @ 1920,0") and libobs monitor id.
	QVector<QPair<QString, QString>> screens() const;

	// audio
	obs_source_t *desktopAudio() const { return m_desktop; }
	obs_source_t *microphone() const { return m_mic; }
	QVector<obs_source_t *> audioSources() const; // desktop, mic, then this scene's sources that have sound
	void setMicCleanup(bool on);
	bool micCleanup() const;

	// outputs
	bool startStream(const QString &server, const QString &key, QString *error);
	void stopStream();
	bool startRecording(const QString &folder, QString *error);
	void stopRecording();
	bool startReplay(const QString &folder, int seconds, QString *error);
	void stopReplay();
	void saveReplay();
	bool streaming() const;
	bool recording() const;
	bool replaying() const;
	bool busy() const { return streaming() || recording() || replaying(); }

	Health health();

signals:
	void streamingChanged(bool on);
	void recordingChanged(bool on, const QString &file);
	void replayChanged(bool on);
	void replaySaved(const QString &file);
	void problem(const QString &message);
	void notice(const QString &message);
	void sceneChanged();       // items of the current scene changed
	void scenesChanged();      // the scene list or the current scene changed

private:
	bool loadModules(QString *error);
	bool resetVideo(const VideoPlan &plan, QString *error);
	void loadScenes();
	void createAudio();
	void setCurrent(obs_source_t *sceneSource, bool animate);
	bool ensureEncoders(QString *error);
	void releaseEncoders();
	void rescaleItems(double ratio);
	QString uniqueName(const QString &base) const;
	obs_source_t *sceneSource(const QString &name) const;
	bool usedByAnotherScene(obs_source_t *source, obs_scene_t *except) const;
	bool startWithFallback(obs_output_t *output, QString *error);
	void heal(Health &h);
	void setBitrate(int kbps);

	static void onStreamStart(void *data, calldata_t *cd);
	static void onStreamStop(void *data, calldata_t *cd);
	static void onReconnect(void *data, calldata_t *cd);
	static void onReconnected(void *data, calldata_t *cd);
	static void onRecordStart(void *data, calldata_t *cd);
	static void onRecordStop(void *data, calldata_t *cd);
	static void onReplayStart(void *data, calldata_t *cd);
	static void onReplayStop(void *data, calldata_t *cd);
	static void onReplaySaved(void *data, calldata_t *cd);

	Hardware m_hw;
	VideoPlan m_plan;
	QStringList m_modules;
	bool m_started = false;
	QVector<obs_source_t *> m_scenes; // our references, in the order shown
	obs_source_t *m_sceneSource = nullptr;
	obs_scene_t *m_scene = nullptr;
	obs_source_t *m_transition = nullptr;
	QString m_transitionId = "fade_transition";
	int m_transitionMs = 300;
	obs_source_t *m_desktop = nullptr;
	obs_source_t *m_mic = nullptr;
	obs_encoder_t *m_video = nullptr;
	obs_encoder_t *m_audio = nullptr;
	obs_service_t *m_service = nullptr;
	obs_output_t *m_stream = nullptr;
	obs_output_t *m_record = nullptr;
	obs_output_t *m_replay = nullptr;
	QString m_recordPath;

	os_cpu_usage_info *m_cpu = nullptr;
	uint32_t m_lastTotal = 0, m_lastLagged = 0, m_lastEncTotal = 0, m_lastEncSkipped = 0;
	int m_lastOutTotal = 0, m_lastOutDropped = 0;
	int m_bitrateNow = 0;   // the live bitrate; m_plan.bitrate is the target
	int m_badSeconds = 0;   // recent seconds with dropped frames
	int m_goodSeconds = 0;  // seconds in a row without
};
