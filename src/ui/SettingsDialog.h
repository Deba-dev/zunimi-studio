#pragma once
#include <QDialog>
#include <QList>
#include <QPair>

#include <functional>

class ObsCore;
class WindowModes;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QTabWidget;
class QLabel;
class QKeySequenceEdit;

class SettingsDialog : public QDialog {
	Q_OBJECT
public:
	enum Tab { Stream, Video, Recording, Audio, Connections, Automation, Remote, Hotkeys, Window, About };
	SettingsDialog(ObsCore *core, WindowModes *modes, QWidget *parent);
	void openTab(Tab tab);
	std::function<void()> checkUpdates; // set by the main window

private:
	QWidget *streamTab();
	QWidget *videoTab();
	QWidget *recordingTab();
	QWidget *audioTab();
	QWidget *connectionsTab();
	QWidget *automationTab();
	QWidget *remoteTab();
	void showRemoteInfo();
	QWidget *hotkeysTab();
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
	QCheckBox *m_pausePreview, *m_closeTray, *m_escTray, *m_autoUpdate, *m_autoHeal, *m_replayAuto;
	QCheckBox *m_escAsk, *m_autoRecord, *m_tracks[6];
	QComboBox *m_codec, *m_rate, *m_preset, *m_format, *m_recQuality, *m_recCodec, *m_monitor, *m_streamTrack;
	QSpinBox *m_quality, *m_keyint, *m_delay, *m_splitMinutes, *m_splitMb, *m_audioKbps;
	QLineEdit *m_nameFormat;
	QLineEdit *m_twitchChannel, *m_youtube, *m_kick, *m_kickRoom, *m_seJwt, *m_slToken, *m_twitchClient;
	QComboBox *m_currency;
	QLabel *m_twitchLogin, *m_kickRelay;
	QLineEdit *m_kofiLink, *m_kofiToken;
	class QTableWidget *m_destinations, *m_rules;
	QCheckBox *m_autoOn, *m_privacyOn, *m_privacyLive, *m_blackAlarm, *m_preflight, *m_report;
	QSpinBox *m_micMinutes;
	class QPlainTextEdit *m_patterns;
	QSpinBox *m_replaySeconds;
	QList<QPair<QString, QKeySequenceEdit *>> m_keys;
	QLabel *m_serverHint, *m_profileHint;
	QComboBox *m_language, *m_accent;
	QCheckBox *m_remoteOn, *m_remoteLan;
	QSpinBox *m_remotePort;
	QLineEdit *m_remotePw;
	QLabel *m_remoteDeck, *m_remoteQr, *m_remoteLink;
	bool m_filling = false;
};
