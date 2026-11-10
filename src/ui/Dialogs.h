#pragma once
#include <QDialog>
#include <QWidget>

#include <obs.h>

#include "core/ObsCore.h"

class ObsCore;
class PreviewWidget;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSlider;
class QSpinBox;
class QTimer;

// Exact numbers for a source: position, size, rotation, crop and how it fills its box. Changes show live;
// Cancel puts everything back. One undo step either way.
class TransformDialog : public QDialog {
	Q_OBJECT
public:
	TransformDialog(ObsCore *core, const QString &name, QWidget *parent);
	void done(int result) override;

private:
	void load();
	void apply();
	ObsCore *m_core;
	QString m_name;
	QDoubleSpinBox *m_x, *m_y, *m_w, *m_h, *m_rot;
	QSpinBox *m_crop[4];
	QComboBox *m_fit;
	bool m_loading = false;
	ItemState m_before;
};

// Per-source sound: hear it in your headphones, line it up when it's out of sync, left/right balance, and which
// recording tracks it goes to.
class AudioDialog : public QDialog {
	Q_OBJECT
public:
	AudioDialog(ObsCore *core, obs_source_t *source, QWidget *parent);

private:
	ObsCore *m_core;
	obs_source_t *m_source;
};

// Everything the health bar sums up, in numbers: per output frames, drops, bitrate and congestion.
class StatsDialog : public QDialog {
	Q_OBJECT
public:
	StatsDialog(ObsCore *core, QWidget *parent);

private:
	void refresh();
	ObsCore *m_core;
	QLabel *m_text;
	QTimer *m_timer;
};

// A projector: the program (or one scene or source) fullscreen on another monitor, or in its own window.
// Double-click or Esc closes a fullscreen one.
class ProjectorWindow : public QWidget {
	Q_OBJECT
public:
	ProjectorWindow(ObsCore *core, obs_source_t *source, int screen, QWidget *parent = nullptr);
	~ProjectorWindow() override;

protected:
	void keyPressEvent(QKeyEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
	PreviewWidget *m_view;
};
