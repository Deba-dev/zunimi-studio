#pragma once
#include <QObject>
#include <QPair>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <optional>

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
	bool isGroup = false;
	QString group; // the group it is in, or empty
};

// Everything about how one item sits in its scene, so an edit can be undone exactly.
struct ItemState {
	QString name;
	obs_transform_info info{};
	obs_sceneitem_crop crop{};
	bool visible = true;
	bool locked = false;
	obs_blending_type blend = OBS_BLEND_NORMAL;
	obs_scale_type filter = OBS_SCALE_DISABLE;
	bool operator==(const ItemState &o) const;
};

// Running totals since the stream or recording started, for the stats window and the after-stream report.
struct OutputStats {
	bool active = false;
	qint64 bytes = 0;
	int totalFrames = 0;
	int droppedFrames = 0;
	double kbps = 0;
	int congestion = 0; // 0-100
	qint64 seconds = 0;
};

// Owns libobs: startup, the modules Zunimi allows, scenes and the transition between them, audio,
// encoders and the outputs (stream, recording, replay buffer, virtual camera).
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
	QSize canvas() const { return m_editVertical ? verticalSize() : QSize(m_plan.width, m_plan.height); }
	QStringList modules() const { return m_modules; }
	bool moduleLoaded(const QString &name) const { return m_modules.contains(name); }

	// vertical canvas (9:16 for Shorts, Reels, TikTok): its own scene and outputs, from the same sources
	bool verticalOn() const { return m_vscene != nullptr; }
	bool setVertical(bool on, QString *error);
	QSize verticalSize() const;
	void setEditVertical(bool on); // the editor (and source list) work on the vertical scene
	bool editingVertical() const { return m_editVertical; }
	obs_source_t *verticalScene() const { return m_vscene; }

	// scenes
	QStringList scenes() const;
	QString currentScene() const;      // the scene being edited (in studio mode: the preview)
	QString programScene() const;      // the scene on air
	void setScene(const QString &name);
	QString addScene(const QString &name);
	QString duplicateScene(const QString &name);
	bool renameScene(const QString &from, const QString &to);
	bool removeScene(const QString &name); // the last scene always stays
	void moveScene(const QString &name, bool up);
	void setSceneOrder(const QStringList &names); // dragged in the Scenes list; must be every scene
	void setTransition(const QString &id, int ms);
	QString transitionId() const { return m_transitionId; }
	int transitionMs() const { return m_transitionMs; }
	static QVector<QPair<QString, QString>> transitionKinds(); // id, label
	// A scene can bring its own transition (empty id: use the default one).
	void setSceneTransition(const QString &scene, const QString &id, int ms);
	QPair<QString, int> sceneTransition(const QString &scene) const;

	// studio mode: edit a scene in the preview, then send it live with transitionNow()
	bool studioMode() const { return m_studio; }
	void setStudioMode(bool on);
	void transitionNow();
	obs_source_t *editSceneSource() const { return m_sceneSource; } // UI thread only

	// items of the current scene
	QVector<SceneItem> items() const; // top first; a group's sources follow it
	// groups: sources that move, hide and lock together
	QString groupItems(const QStringList &names, const QString &group); // the new group's name, or empty
	bool ungroup(const QString &group);
	obs_scene_t *scene() const { return m_scene; }
	obs_sceneitem_t *item(const QString &name) const;
	obs_source_t *addSource(const char *kind, const QString &name, obs_data_t *settings, Fit fit);
	obs_source_t *addSourceAt(const char *kind, const QString &name, obs_data_t *settings, const QRectF &box);
	bool addExisting(const QString &name);          // a source from another scene, shared
	QStringList otherScenesSources() const;         // sources not in this scene yet
	QStringList nestableScenes() const;             // scenes that can go inside this one
	void removeSource(const QString &name);         // from this scene; deleted when no scene uses it
	bool renameSource(const QString &from, const QString &to);
	void setVisible(const QString &name, bool visible);
	void setLocked(const QString &name, bool locked);
	void moveSource(const QString &name, bool up);
	void moveSourceTo(const QString &name, int indexFromTop);
	// Dragged in the Sources list: sources that sit side by side (the scene's top level, or one group's
	// sources), top first. One undo step.
	void setSourceOrder(const QStringList &siblingsTopFirst);
	void fit(const QString &name, Fit fit);
	void place(obs_sceneitem_t *item, const QRectF &box); // fits the item inside box, keeping its shape
	void save();
	// Every screen Windows has: label ("DELL U2419: 1920x1080 @ 1920,0") and libobs monitor id.
	QVector<QPair<QString, QString>> screens() const;

	// editing (each records an undo step)
	QRectF itemBox(obs_sceneitem_t *item) const; // canvas rectangle the item covers
	void rotate(const QString &name, float degrees); // adds to the current angle, keeps the centre
	void flip(const QString &name, bool horizontal);
	void setCrop(const QString &name, const obs_sceneitem_crop &crop);
	void setBlend(const QString &name, obs_blending_type type);
	void setScaleFilter(const QString &name, obs_scale_type type);
	void setShowHide(const QString &name, const QString &transitionId, int ms); // empty id: none
	QString showHide(const QString &name) const;
	void resetTransform(const QString &name);
	void copyTransform(const QString &name);
	bool pasteTransform(const QString &name);
	bool hasCopiedTransform() const { return m_copied.has_value(); }
	ItemState itemState(obs_sceneitem_t *item) const;
	void setItemState(obs_sceneitem_t *item, const ItemState &state);

	// undo: beginEdit/endEdit wrap anything that changes items of the current scene
	void beginEdit(const QString &label);
	void endEdit();
	bool canUndo() const { return !m_undo.isEmpty(); }
	bool canRedo() const { return !m_redo.isEmpty(); }
	QString undoLabel() const;
	QString redoLabel() const;
	void undo();
	void redo();

	// scene collections (whole sets of scenes) and profiles (stream, video and recording settings)
	QStringList collections() const;
	QString collection() const { return m_collection; }
	bool switchCollection(const QString &name, QString *error);
	bool newCollection(const QString &name, bool copyCurrent, QString *error);
	bool renameCollection(const QString &from, const QString &to, QString *error);
	bool removeCollection(const QString &name, QString *error);
	// Brings in an OBS Studio (or Streamlabs/overlay pack) scene collection .json. Missing files are looked
	// for next to the .json and in searchFolder. Returns the new collection's name.
	QString importCollection(const QString &jsonPath, const QString &searchFolder, QStringList *notes,
				 QString *error);
	QStringList profiles() const;
	QString profile() const;
	bool switchProfile(const QString &name, QString *error);
	bool newProfile(const QString &name, QString *error);
	bool removeProfile(const QString &name, QString *error);

	// audio
	obs_source_t *desktopAudio() const { return m_desktop; }
	obs_source_t *microphone() const { return m_mic; }
	QVector<obs_source_t *> audioSources() const; // desktop, mic, then this scene's sources that have sound
	void setMicCleanup(bool on);
	bool micCleanup() const;
	void saveAudioSettings(obs_source_t *source); // desktop and mic keep theirs in settings
	void outputSettingsChanged(); // encoders and outputs are rebuilt on the next start
	static QVector<QPair<QString, QString>> monitoringDevices(); // name, id
	void applyMonitoringDevice();

	// outputs
	bool startStream(const QString &server, const QString &key, QString *error);
	void stopStream();
	// Multistream: the same encode goes to more platforms at once (costs upload, not CPU).
	struct Destination {
		QString name, server, key;
		bool on = true;
		bool vertical = false; // sends the 9:16 picture (the vertical canvas) instead of the 16:9 one
	};
	static QVector<Destination> destinations();
	static void setDestinations(const QVector<Destination> &list);
	static void moveOldVerticalStream(); // the vertical canvas's own server/key (before 0.5.1) becomes a destination
	// What the Go Live to panel shows for each destination, in list order
	struct DestinationState {
		QString state; // "off", "connecting", "live", "reconnecting", "failed"
		QString error;
	};
	QVector<DestinationState> destinationStates() const;
	QString mainState() const;
	void setDestinationOn(int index, bool on); // saved; while live it also starts or stops just that one
	QStringList extraStatus() const; // "YouTube: live", "Kick: failed (...)"
	bool startRecording(const QString &folder, QString *error);
	void stopRecording();
	bool pauseRecording(bool pause);
	bool recordingPaused() const;
	bool startReplay(const QString &folder, int seconds, QString *error);
	void stopReplay();
	void saveReplay();
	bool streaming() const;
	bool recording() const;
	bool replaying() const;
	bool busy() const { return streaming() || recording() || replaying() || virtualCam() || (m_vstream && obs_output_active(m_vstream)); }
	OutputStats streamStats();
	OutputStats recordStats();

	// virtual camera: other apps (Discord, Zoom, Meet) see the program as a webcam
	static bool virtualCamInstalled();
	static bool installVirtualCam(QString *error); // asks Windows for admin rights once
	bool startVirtualCam(QString *error);
	void stopVirtualCam();
	bool virtualCam() const;

	// a PNG of what is on air (or of one source), written to folder
	void screenshot(const QString &folder, obs_source_t *source = nullptr);

	Health health();

signals:
	void streamingChanged(bool on);
	void recordingChanged(bool on, const QString &file);
	void recordingPausedChanged(bool paused);
	void replayChanged(bool on);
	void replaySaved(const QString &file);
	void virtualCamChanged(bool on);
	void screenshotSaved(const QString &file);
	void problem(const QString &message);
	void notice(const QString &message);
	void sceneChanged();       // items of the current scene changed
	void scenesChanged();      // the scene list or the current scene changed
	void studioModeChanged(bool on);
	void undoChanged();
	void collectionChanged();
	void verticalChanged(bool on);
	void verticalEditChanged(bool on);

private:
	struct Action {
		QString label;
		std::function<void()> undo, redo;
	};

	bool loadModules(QString *error);
	bool resetVideo(const VideoPlan &plan, QString *error);
	QString collectionPath(const QString &name) const;
	void loadScenes(const QString &path);
	void clearScenes();
	void createAudio();
	void applyAudioSettings(obs_source_t *source, const QString &key);
	void setCurrent(obs_source_t *sceneSource, bool animate);
	void startTransition(obs_source_t *to, bool animate);
	obs_source_t *transitionFor(const QString &id);
	bool ensureEncoders(QString *error);
	bool ensureRecordEncoders(QString *error);
	void releaseEncoders();
	void rescaleItems(double ratio);
	QString uniqueName(const QString &base) const;
	obs_source_t *sceneSource(const QString &name) const;
	bool usedByAnotherScene(obs_source_t *source, obs_scene_t *except) const;
	bool startWithFallback(obs_output_t *output, QString *error, bool record);
	void attachEncoders(obs_output_t *output, bool record);
	void heal(Health &h);
	void setBitrate(int kbps);
	QVector<ItemState> snapshot(obs_scene_t *scene) const; // bottom first
	void restore(const QString &sceneName, const QVector<ItemState> &states);
	void push(const QString &label, std::function<void()> undo, std::function<void()> redo);
	void editItem(const QString &name, const QString &label, const std::function<void(obs_sceneitem_t *)> &change);
	void keepCentre(obs_sceneitem_t *item, const QPointF &centre);
	void recordAdd(const QString &name);
	bool createVerticalCanvas(QString *error);
	void destroyVertical();
	void loadVertical(obs_data_t *root);
	void saveVertical(obs_data_t *root);
	void followProgram(obs_source_t *program);
	bool ensureVerticalEncoder(QString *error);
	void startVerticalOutputs(bool stream, bool record, const QString &folder);
	void stopVerticalOutputs(bool stream, bool record);

	static void onStreamStart(void *data, calldata_t *cd);
	static void onStreamStop(void *data, calldata_t *cd);
	static void onReconnect(void *data, calldata_t *cd);
	static void onReconnected(void *data, calldata_t *cd);
	static void onRecordStart(void *data, calldata_t *cd);
	static void onRecordStop(void *data, calldata_t *cd);
	static void onReplayStart(void *data, calldata_t *cd);
	static void onReplayStop(void *data, calldata_t *cd);
	static void onReplaySaved(void *data, calldata_t *cd);
	static void onVirtualCamStop(void *data, calldata_t *cd);

	Hardware m_hw;
	VideoPlan m_plan;
	QStringList m_modules;
	bool m_started = false;
	QString m_collection;
	QVector<obs_source_t *> m_scenes; // our references, in the order shown
	obs_source_t *m_sceneSource = nullptr; // edited (and, outside studio mode, on air)
	obs_scene_t *m_scene = nullptr;
	obs_source_t *m_program = nullptr;     // on air (not a reference; always one of m_scenes)
	bool m_studio = false;
	obs_source_t *m_transition = nullptr;
	QVector<obs_source_t *> m_transitions; // cache, our references
	QString m_transitionId = "fade_transition";
	int m_transitionMs = 300;
	obs_source_t *m_desktop = nullptr;
	obs_source_t *m_mic = nullptr;
	obs_encoder_t *m_video = nullptr;
	obs_encoder_t *m_audio[6] = {};          // one AAC encoder per audio track
	obs_encoder_t *m_recVideo = nullptr;     // only when recordings use their own quality
	obs_service_t *m_service = nullptr;
	obs_output_t *m_stream = nullptr;
	obs_output_t *m_record = nullptr;
	obs_output_t *m_replay = nullptr;
	obs_output_t *m_vcam = nullptr;
	obs_encoder_t *m_opus = nullptr;          // WHIP (WebRTC) needs Opus audio
	struct Extra {
		int index = -1; // in destinations()
		QString name;
		obs_service_t *service = nullptr;
		obs_output_t *output = nullptr;
		QString error;
	};
	QVector<Extra> m_extras;
	obs_canvas_t *m_vcanvas = nullptr;
	obs_source_t *m_vscene = nullptr;   // our reference
	obs_source_t *m_mainEdit = nullptr; // the landscape scene being edited while the vertical one is
	bool m_editVertical = false;
	obs_encoder_t *m_vvideo = nullptr;
	obs_output_t *m_vstream = nullptr, *m_vrecord = nullptr;
	obs_service_t *m_vservice = nullptr;
	void startExtras();
	void stopExtras();
	void startExtra(int index, const Destination &d);
	void stopExtra(int index);
	QString m_recordPath;
	qint64 m_streamStart = 0, m_recordStart = 0;

	QVector<Action> m_undo, m_redo;
	bool m_replaying = false;               // applying an undo or redo: record nothing
	QString m_editLabel, m_editScene;
	QVector<ItemState> m_editBefore;
	int m_editDepth = 0;
	std::optional<ItemState> m_copied;

	os_cpu_usage_info *m_cpu = nullptr;
	uint32_t m_lastTotal = 0, m_lastLagged = 0, m_lastEncTotal = 0, m_lastEncSkipped = 0;
	int m_lastOutTotal = 0, m_lastOutDropped = 0;
	int m_bitrateNow = 0;   // the live bitrate; m_plan.bitrate is the target
	int m_badSeconds = 0;   // recent seconds with dropped frames
	int m_goodSeconds = 0;  // seconds in a row without
};
