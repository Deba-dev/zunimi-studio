#pragma once
#include <QWidget>

#include <atomic>
#include <vector>

#include <obs.h>

class ObsCore;
class QVBoxLayout;
class QTimer;

// A live level meter: green, amber near the top, red when it clips. Fed from libobs's audio thread.
class LevelMeter : public QWidget {
	Q_OBJECT
public:
	explicit LevelMeter(obs_source_t *source, QWidget *parent = nullptr);
	~LevelMeter() override;
	void tick(); // called ~30 times a second by the mixer

protected:
	void paintEvent(QPaintEvent *) override;

private:
	static void onLevels(void *param, const float magnitude[MAX_AUDIO_CHANNELS],
			     const float peak[MAX_AUDIO_CHANNELS], const float inputPeak[MAX_AUDIO_CHANNELS]);
	obs_volmeter_t *m_meter = nullptr;
	std::atomic<float> m_level{-100.0f}; // dBFS, newest peak
	float m_shown = -100.0f;
	float m_hold = -100.0f;
	int m_holdTicks = 0;
};

// Every source with sound: meter, volume, mute and its filters. Rebuilt when the scene changes.
class Mixer : public QWidget {
	Q_OBJECT
public:
	explicit Mixer(ObsCore *core, QWidget *parent = nullptr);
	void refresh(bool force = false); // force: also when the list is the same (a hotkey muted something)

signals:
	void filtersRequested(obs_source_t *source);
	void audioRequested(obs_source_t *source); // sync, balance, monitoring, tracks

private:
	QWidget *channel(obs_source_t *source);

	ObsCore *m_core;
	QVBoxLayout *m_list;
	QTimer *m_timer;
	std::vector<obs_source_t *> m_shown; // what the rows were built for
	std::vector<LevelMeter *> m_meters;
};
