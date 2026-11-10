#pragma once
#include <QElapsedTimer>
#include <QMainWindow>

#include <obs.h>

#include "core/Updater.h"

class Hotkeys;
class Mixer;
class ObsCore;
class PreviewWidget;
class WindowModes;
class QAction;
class QComboBox;
class QLabel;
class QListWidget;
class QMenu;
class QPushButton;
class QSpinBox;
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
	QWidget *buildScenes();
	QWidget *buildSources();
	QWidget *buildPreview();
	QWidget *buildBottomBar();
	QWidget *buildUpdateBar();
	void buildTray();
	void fillAddMenu(QMenu *menu);

	// scenes
	void refreshScenes();
	void renameScene();
	// sources
	void refreshSources();
	void addSource(const QString &kind);
	void addScreen(const QString &id, const QString &label);
	void addAllScreens();
	QString selectedSource() const;
	void selectSource(const QString &name);
	void sourceMenu(const QString &name, const QPoint &global);
	void openProperties(const QString &name, bool justAdded = false);
	void openFilters(obs_source_t *source);
	void renameSource(const QString &name);
	// outputs
	void toggleStream();
	void toggleRecord();
	void toggleReplay();
	void saveReplay();
	void hotkey(const QString &action);
	void applyHotkeys();
	void openSettings(int tab = 0);
	void tick();
	void updatePreview();
	void updateButtons();
	void say(const QString &text, bool bad = false);
	QString quitWouldStop() const;
	// updates
	void showUpdate(const QString &text, const QString &go, const QString &later);
	void updateClicked();

	ObsCore *m_core;
	WindowModes *m_modes;
	Hotkeys *m_hotkeys;
	PreviewWidget *m_preview = nullptr;
	QStackedWidget *m_previewStack = nullptr;
	QListWidget *m_scenes = nullptr;
	QComboBox *m_transition = nullptr;
	QSpinBox *m_transitionMs = nullptr;
	QListWidget *m_sources = nullptr;
	QLabel *m_empty = nullptr;
	Mixer *m_mixer = nullptr;
	QLabel *m_health = nullptr;
	QLabel *m_healthDot = nullptr;
	QLabel *m_stats = nullptr;
	QLabel *m_profile = nullptr;
	QLabel *m_banner = nullptr;
	QLabel *m_clock = nullptr;
	QPushButton *m_live = nullptr;
	QPushButton *m_rec = nullptr;
	QPushButton *m_replay = nullptr;
	QPushButton *m_saveReplay = nullptr;
	QAction *m_trayLive = nullptr;
	QAction *m_trayRec = nullptr;
	QAction *m_trayReplay = nullptr;
	QTimer *m_ticker = nullptr;
	QTimer *m_bannerTimer = nullptr;
	QElapsedTimer m_liveSince;
	bool m_previewWanted = true;

	Updater *m_updater = nullptr;
	UpdateInfo m_update;
	QString m_installer;
	QWidget *m_updateBar = nullptr;
	QLabel *m_updateText = nullptr;
	QPushButton *m_updateGo = nullptr;
	QPushButton *m_updateLater = nullptr;
};
