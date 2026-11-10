#pragma once
#include <QJsonArray>
#include <QObject>
#include <QStringList>
#include <QVector>

#include <atomic>

#include <obs.h>

#include "core/ObsCore.h"

class QTimer;

// A problem found before or during a stream, in plain words, with an optional one-click fix.
struct Finding {
	int level = 1;   // 1 worth a look, 2 serious
	QString text;
	QString fix;     // id of the fix (empty: nothing to click)
	QString fixLabel;
};

// The things a streamer forgets or can't see while playing:
//  - Pre-flight: before Go Live, checks for an empty scene, a muted or silent mic, a black game capture, no
//    disk space, a heavy encoder for the PC.
//  - Live alarms: the mic has been silent for minutes, the picture went black.
//  - Stream Doctor: the health bar's problem with a fix to click.
//  - After-stream report: how it went, minute by minute.
//  - Automation: switch scenes by the app in front, hide captures while a private window is in front,
//    and notice when a laptop goes on battery.
class Assist : public QObject {
	Q_OBJECT
public:
	explicit Assist(ObsCore *core, QObject *parent = nullptr);
	~Assist() override;

	QVector<Finding> preflight(bool forLive);
	Finding doctor(const Health &h) const;
	void applyFix(const QString &id);
	void sample(const Health &h); // once a second from the main window

	// Mark moment: a chapter in the recording (and a saved replay when the buffer is on).
	void mark(const QString &label = {});
	struct Report {
		qint64 seconds = 0;
		double avgKbps = 0, droppedPercent = 0, peakCpu = 0, avgFps = 0;
		int heals = 0, worstMinute = -1, chats = 0;
		double worstDrop = 0;
		QStringList events;
	};
	Report report() const { return m_report; }

	static QString foregroundApp(QString *title = nullptr); // exe name and window title in front
	static QStringList runningApps();                       // exe names with a visible window

signals:
	void alarm(const QString &text);        // something to tell the streamer now (toast + tray)
	void reportReady(const QString &html);  // after a stream ends
	void marked(const QString &label, double recordingSeconds); // -1 when not recording

private:
	void tick();
	void automation();
	void blackCheck();
	void writeChapters(const QString &file);
	static void onMic(void *param, const float magnitude[MAX_AUDIO_CHANNELS], const float peak[MAX_AUDIO_CHANNELS],
			  const float inputPeak[MAX_AUDIO_CHANNELS]);

	ObsCore *m_core;
	QTimer *m_timer;
	obs_volmeter_t *m_meter = nullptr;
	std::atomic<float> m_micPeak{-100.0f};
	qint64 m_micQuietSince = 0, m_blackSince = 0;
	bool m_micWarned = false, m_blackWarned = false;
	std::atomic<double> m_luma{-1.0};
	QString m_lastApp;
	QStringList m_hiddenForPrivacy; // names of captures the privacy guard hid
	QString m_privacyScene;
	bool m_onBattery = false;
	// report
	Report m_report;
	QVector<double> m_minuteDrops;
	double m_kbpsSum = 0, m_fpsSum = 0;
	int m_samples = 0, m_lastHealBitrate = 0;
	bool m_live = false;
	// chapters
	QVector<QPair<qint64, QString>> m_marks; // seconds into the recording, label
	QString m_recording;
};
