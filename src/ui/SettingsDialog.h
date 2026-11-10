#pragma once
#include <QDialog>

#include <functional>

class ObsCore;
class WindowModes;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QTabWidget;
class QLabel;

class SettingsDialog : public QDialog {
	Q_OBJECT
public:
	enum Tab { Stream, Video, Recording, Window, About };
	SettingsDialog(ObsCore *core, WindowModes *modes, QWidget *parent);
	void openTab(Tab tab);
	std::function<void()> checkUpdates; // set by the main window

private:
	QWidget *streamTab();
	QWidget *videoTab();
	QWidget *recordingTab();
	QWidget *windowTab();
	QWidget *aboutTab();
	void platformChanged();
	void profileChanged();
	void markCustom();
	void save();

	ObsCore *m_core;
	WindowModes *m_modes;
	QTabWidget *m_tabs;
	QComboBox *m_platform, *m_profile, *m_resolution, *m_fps, *m_encoder, *m_mode;
	QLineEdit *m_server, *m_key, *m_folder;
	QSpinBox *m_bitrate;
	QCheckBox *m_pausePreview, *m_closeTray, *m_escTray, *m_autoUpdate;
	QLabel *m_serverHint, *m_profileHint;
	bool m_filling = false;
};
