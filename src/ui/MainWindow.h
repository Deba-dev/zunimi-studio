#pragma once
#include <QElapsedTimer>
#include <QMainWindow>

#include <obs.h>

#include "core/Updater.h"

class Assist;
class Hotkeys;
class Mixer;
class Remote;
struct SetupChoices;
class ObsCore;
class PreviewWidget;
class WindowModes;
class QAction;
class QComboBox;
class QDockWidget;
class QLabel;
class QListWidget;
class QMenu;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTimer;
class QVBoxLayout;

class MainWindow : public QMainWindow {
	Q_OBJECT
public:
	explicit MainWindow(ObsCore *core);
	WindowModes *modes() const { return m_modes; }
	void checkForUpdates();
	Remote *remote() const { return m_remote; }
	void applyRemote();
	void runSetup();                          // the first-time setup (also from Menu)
	void applySetup(const SetupChoices &c);
	static bool simple();                     // Simple mode: only what's needed to go live and record
	bool selftestDocks();                     // --selftest-docks: move, float, hide, save, restore, reset, lock
	void selftestMedia(const QStringList &files); // --selftest-media: every option on a media file, clicked
	void selftestOptions(const QStringList &files); // --selftest-options: every source kind, every option widget
	int dockArea(const QString &id);           // for self-tests: where a dock sits (Qt::DockWidgetArea)
	void raiseDock(const QString &id);         // brings a tabbed dock to the front (--demo-dock, screenshots)
	void setSimple(bool on);

protected:
	void keyPressEvent(QKeyEvent *event) override;
	void closeEvent(QCloseEvent *event) override;

private:
	void buildMenuBar();
	QWidget *buildScenes();
	QWidget *buildSources();
	QWidget *buildPreview();
	QWidget *buildTransitions();
	QWidget *buildControls();
	QWidget *buildSession();
	QWidget *buildGoLive();                  // Go Live to: every destination with a switch and a status light
	void refreshGoLive(bool rebuild);
	void addDestinationDialog();
	void pickYouTubeStream();                // sign in to YouTube and pick a scheduled stream
	QWidget *buildUpdateBar();
	// docks: every panel can be dragged, floated, tabbed together, resized and closed, like OBS
	QDockWidget *addDock(const QString &id, const QString &title, QWidget *content, Qt::DockWidgetArea area, bool pro = false);
	void fillDocksMenu(QMenu *menu);
	void resetDocks();
	void setDocksLocked(bool locked);
	void setFullHeightDocks(bool on);
	void saveDocks();
	void buildTray();
	void fillAddMenu(QMenu *menu);
	void fillFileMenu(QMenu *menu);
	void fillEditMenu(QMenu *menu);
	void fillViewMenu(QMenu *menu);
	void fillToolsMenu(QMenu *menu);
	void fillHelpMenu(QMenu *menu);
	void fillCollectionsMenu(QMenu *menu);
	void fillProfilesMenu(QMenu *menu);
	void fillProjectorMenu(QMenu *menu, obs_source_t *source);

	// scenes
	void refreshScenes();
	void renameScene();
	void sceneMenu(const QPoint &global);
	void chooseTransition();
	void updateStudio();
	void updateCanvasSwitch();
	void verticalSettings();
	void importCollection();
	void importFromObs();
	// sources
	void refreshSources();
	void addSource(const QString &kind);
	void addScreen(const QString &id, const QString &label);
	void addAllScreens();
	void addOverlay(const QString &kind, bool quiet = false);
	void addBrowser(bool overlayLink);
	bool ensureBrowser();
	void restartApp();
	QString selectedSource() const;
	void selectSource(const QString &name);
	void sourceMenu(const QString &name, const QPoint &global);
	void openProperties(const QString &name, bool justAdded = false);
	void openFilters(obs_source_t *source);
	void renameSource(const QString &name);
	void openTransform(const QString &name);
	void openAudio(obs_source_t *source);
	void openProjector(obs_source_t *source, int screen);
	void screenshot(obs_source_t *source = nullptr);
	// outputs
	void toggleStream();
	bool preflightOk();
	void toggleRecord();
	void toggleReplay();
	void saveReplay();
	void togglePause();
	void toggleVirtualCam();
	void hotkey(const QString &action);
	void applyHotkeys();
	void openSettings(int tab = 0);
	void tick();
	void updatePreview();
	void updateButtons();
	void say(const QString &text, bool bad = false);
	void applyLevel();
	QString quitWouldStop() const;
	// updates
	void showUpdate(const QString &text, const QString &go, const QString &later);
	void updateClicked();

	ObsCore *m_core;
	Assist *m_assist = nullptr;
	Remote *m_remote = nullptr;
	QPushButton *m_fix = nullptr;
	QString m_fixId;
	WindowModes *m_modes;
	Hotkeys *m_hotkeys;
	PreviewWidget *m_preview = nullptr;
	PreviewWidget *m_program = nullptr;     // studio mode: what is on air
	QWidget *m_programBox = nullptr;
	QLabel *m_previewTitle = nullptr;
	QWidget *m_canvasSwitch = nullptr;
	QPushButton *m_landscapeButton = nullptr, *m_verticalButton = nullptr;
	QStackedWidget *m_previewStack = nullptr;
	QListWidget *m_scenes = nullptr;
	QComboBox *m_transition = nullptr;
	QSpinBox *m_transitionMs = nullptr;
	QListWidget *m_sources = nullptr;
	QLabel *m_empty = nullptr;
	Mixer *m_mixer = nullptr;
	QLabel *m_health = nullptr;
	QLabel *m_healthDot = nullptr;
	QLabel *m_cpu = nullptr;
	QLabel *m_fps = nullptr;
	QLabel *m_liveTime = nullptr;
	QLabel *m_recTime = nullptr;
	QLabel *m_tune = nullptr;
	QPushButton *m_studio = nullptr;
	QPushButton *m_hero = nullptr;      // the Go Live card
	QLabel *m_slogan = nullptr;
	QByteArray m_savedDocks;           // what was last written, so the layout is saved only when it changes
	struct GoLiveRow {
		QLabel *dot = nullptr, *status = nullptr;
	};
	QVBoxLayout *m_goLiveRows = nullptr;
	QVector<GoLiveRow> m_goLive; // [0] is the main stream, then destinations() in order
	QList<QDockWidget *> m_docks;
	QByteArray m_defaultDocks;
	QElapsedTimer m_recSince;
	QLabel *m_profile = nullptr;
	QLabel *m_banner = nullptr;
	QPushButton *m_live = nullptr;
	QPushButton *m_rec = nullptr;
	QPushButton *m_replay = nullptr;
	QPushButton *m_saveReplay = nullptr;
	QPushButton *m_pause = nullptr;
	QPushButton *m_vcam = nullptr;
	QAction *m_trayLive = nullptr;
	QAction *m_trayRec = nullptr;
	QAction *m_trayReplay = nullptr;
	bool m_pttMuted = false; // the mic's mute before push-to-talk/mute took over
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
