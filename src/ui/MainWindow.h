#pragma once
#include <QElapsedTimer>
#include <QMainWindow>

#include "core/Updater.h"

#include <obs.h>

class ObsCore;
class PreviewWidget;
class WindowModes;
class QAction;
class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
class QStackedWidget;
class QTimer;

class MainWindow : public QMainWindow {
	Q_OBJECT
public:
	explicit MainWindow(ObsCore *core);
	WindowModes *modes() const { return m_modes; }
	void checkForUpdates();

protected:
	void keyPressEvent(QKeyEvent *event) override;
	void closeEvent(QCloseEvent *event) override;

private:
	QWidget *buildTopBar();
	QWidget *buildSources();
	QWidget *buildPreview();
	QWidget *buildBottomBar();
	QWidget *audioControl(const QString &label, obs_source_t *source, const QString &key);
	void buildTray();
	QWidget *buildUpdateBar();
	void showUpdate(const QString &text, const QString &go, const QString &later);
	void updateClicked();

	void addSource(const QString &kind);
	bool pick(obs_source_t *source, const char *property, const QString &title);
	void changeSelected();
	void refreshSources();
	void toggleStream();
	void toggleRecord();
	void openSettings(int tab = 0);
	void tick();
	void updatePreview();
	void updateButtons();
	void say(const QString &text, bool bad = false);
	QString quitWouldStop() const;

	ObsCore *m_core;
	WindowModes *m_modes;
	PreviewWidget *m_preview = nullptr;
	QStackedWidget *m_previewStack = nullptr;
	QListWidget *m_sources = nullptr;
	QLabel *m_empty = nullptr;
	QLabel *m_health = nullptr;
	QLabel *m_healthDot = nullptr;
	QLabel *m_stats = nullptr;
	QLabel *m_profile = nullptr;
	QLabel *m_banner = nullptr;
	QLabel *m_clock = nullptr;
	QPushButton *m_live = nullptr;
	QPushButton *m_rec = nullptr;
	QAction *m_trayLive = nullptr;
	QAction *m_trayRec = nullptr;
	QTimer *m_ticker = nullptr;
	QTimer *m_bannerTimer = nullptr;
	QElapsedTimer m_liveSince;
	bool m_previewWanted = true;
	bool m_shown = false;

	Updater *m_updater = nullptr;
	UpdateInfo m_update;
	QString m_installer;
	QWidget *m_updateBar = nullptr;
	QLabel *m_updateText = nullptr;
	QPushButton *m_updateGo = nullptr;
	QPushButton *m_updateLater = nullptr;
};
