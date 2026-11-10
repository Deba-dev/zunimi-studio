#pragma once
#include <QDialog>
#include <QString>

class ObsCore;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;

// What the first-time setup decided; the main window carries it out.
struct SetupChoices {
	bool simple = true;
	bool importObs = false;
	QString platform; // twitch, youtube, kick, custom, or empty: recording only for now
	QString server, key;
	QString channel; // for chat and alerts
	bool game = false;
	QString screenId, screenLabel;
	QString webcamId, webcamName;
	bool alerts = false, chat = false;
};

// Four short steps for someone opening Zunimi for the first time: how much of the app to show, where they stream,
// what viewers should see, and a last look before going live. Every step can be skipped.
class SetupWizard : public QDialog {
	Q_OBJECT
public:
	SetupWizard(ObsCore *core, QWidget *parent);
	SetupChoices choices() const;
	static bool obsFound(); // OBS Studio scene collections on this PC
	static QVector<QPair<QString, QString>> webcams(); // name, device id

private:
	QWidget *levelPage();
	QWidget *platformPage();
	QWidget *showPage();
	QWidget *readyPage();
	void go(int page);
	void platformChanged();
	void fillReady();

	ObsCore *m_core;
	QStackedWidget *m_pages;
	QPushButton *m_back, *m_next, *m_skip;
	QLabel *m_step;
	QButtonGroup *m_level, *m_platform;
	QCheckBox *m_import;
	QLineEdit *m_server, *m_key, *m_channel;
	QLabel *m_serverLabel, *m_keyHelp, *m_channelLabel;
	QCheckBox *m_game, *m_screen, *m_webcam, *m_alerts, *m_chat;
	QComboBox *m_screens, *m_webcams;
	QLabel *m_summary;
};
