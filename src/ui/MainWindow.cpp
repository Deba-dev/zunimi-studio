#include "ui/MainWindow.h"
#include "core/I18n.h"

#include "core/Addons.h"
#include "core/Assist.h"
#include "core/ObsCore.h"
#include "core/Settings.h"
#include "events/Events.h"
#include "net/Remote.h"
#include "overlays/Overlays.h"
#include "ui/AlertDesigner.h"
#include "ui/Dialogs.h"
#include "ui/LivePanels.h"
#include "ui/FiltersDialog.h"
#include "ui/Hotkeys.h"
#include "ui/Mixer.h"
#include "ui/PreviewWidget.h"
#include "ui/PropertiesView.h"
#include "ui/SettingsDialog.h"
#include "ui/SetupWizard.h"
#include "ui/Templates.h"
#include "ui/Theme.h"
#include "ui/WindowModes.h"
#include "version.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QMenuBar>
#include <QToolButton>
#include <QJsonDocument>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGuiApplication>
#include <QScreen>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QShortcut>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextBrowser>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
constexpr int DOCKS_VERSION = 1; // bump when the docks change so an old saved layout is not applied

struct Kind {
	const char *id, *label, *name;
	ObsCore::Fit fit;
	bool ask; // open its options right after adding
};
const Kind KINDS[] = {
	{"game_capture", "Game  (whichever game is fullscreen)", "Game", ObsCore::Fit::Fill, false},
	{"window_capture", "Window", "Window", ObsCore::Fit::Fill, true},
	{"dshow_input", "Webcam", "Webcam", ObsCore::Fit::Corner, true},
	{"wasapi_process_output_capture", "App audio  (sound of one app only)", "App audio", ObsCore::Fit::Fill, true},
	{"image_source", "Image", "Image", ObsCore::Fit::Center, true},
	{"slideshow", "Slideshow", "Slideshow", ObsCore::Fit::Fill, true},
	{"ffmpeg_source", "Video or sound file", "Media", ObsCore::Fit::Fill, true},
	{"text_gdiplus", "Text", "Text", ObsCore::Fit::Center, true},
	{"color_source", "Colour background", "Colour", ObsCore::Fit::Fill, true},
	{"wasapi_input_capture", "Another microphone", "Microphone", ObsCore::Fit::Fill, true},
	{"wasapi_output_capture", "Another speaker or headset", "Speakers", ObsCore::Fit::Fill, true},
};

const Kind *kindOf(const QString &id)
{
	for (const Kind &k : KINDS)
		if (id == k.id)
			return &k;
	return nullptr;
}

QWidget *panel(QLayout *layout)
{
	auto *w = new QWidget;
	w->setObjectName("panel");
	w->setAttribute(Qt::WA_StyledBackground);
	w->setLayout(layout);
	return w;
}

QPushButton *button(const QString &text, const QString &object = {}, const QString &tip = {})
{
	auto *b = new QPushButton(text);
	if (!object.isEmpty())
		b->setObjectName(object);
	if (!tip.isEmpty())
		b->setToolTip(tip);
	b->setCursor(Qt::PointingHandCursor);
	return b;
}

void restyle(QWidget *w)
{
	w->style()->unpolish(w);
	w->style()->polish(w);
}

QString defaultFolder()
{
	return QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + L("/Zunimi Studio");
}

// "KA222Q H: 1920x1080 @ 0,0 (Primary Monitor)" -> "Screen 1: KA222Q H (main)"; a screen with no name is
// almost always a laptop's own panel.
QString screenLabel(int number, const QString &raw)
{
	QString name = raw.section(':', 0, 0).trimmed();
	if (name.isEmpty())
		name = L("Built-in display");
	const auto size = QRegularExpression("(\\d+)x(\\d+)").match(raw);
	QString label = QString(L("Screen %1: %2")).arg(number).arg(name);
	if (size.hasMatch())
		label += QString("  %1×%2").arg(size.captured(1), size.captured(2));
	if (raw.contains("Primary"))
		label += L("  (main)");
	return label;
}

QRectF screenRect(const QString &raw)
{
	const auto m = QRegularExpression("(\\d+)x(\\d+) @ (-?\\d+),(-?\\d+)").match(raw);
	if (!m.hasMatch())
		return {};
	return QRectF(m.captured(3).toDouble(), m.captured(4).toDouble(), m.captured(1).toDouble(),
		      m.captured(2).toDouble());
}
} // namespace

MainWindow::MainWindow(ObsCore *core) : m_core(core)
{
	setWindowTitle(ZUNIMI_NAME);
	resize(1360, 820);
	setMinimumSize(1040, 600);
	m_modes = new WindowModes(this, ZUNIMI_NAME);
	ObsCore::moveOldVerticalStream();
	m_assist = new Assist(m_core, this);
	m_modes->busy = [this] { return quitWouldStop(); };

	// The preview sits in the middle; everything else is a dock that can be dragged, floated, tabbed together,
	// resized or closed (Docks menu brings it back), and the layout is remembered.
	setDockNestingEnabled(true);
	setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks);
	auto *central = new QWidget;
	auto *root = new QVBoxLayout(central);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(8);
	root->addWidget(buildPreview(), 1);
	m_banner = new QLabel;
	m_banner->setWordWrap(true);
	m_banner->hide();
	root->addWidget(m_banner);
	root->addWidget(buildUpdateBar());
	setCentralWidget(central);

	m_mixer = new Mixer(m_core);
	auto *chat = new ChatPanel;
	auto *events = new EventsPanel;
	connect(chat, &ChatPanel::setupRequested, this, [this] { openSettings(SettingsDialog::Connections); });
	connect(events, &EventsPanel::setupRequested, this, [this] { openSettings(SettingsDialog::Connections); });
	QDockWidget *scenes = addDock("scenes", L("Scenes"), buildScenes(), Qt::LeftDockWidgetArea);
	QDockWidget *sources = addDock("sources", L("Sources"), buildSources(), Qt::LeftDockWidgetArea);
	splitDockWidget(scenes, sources, Qt::Vertical);
	QDockWidget *controls = addDock("controls", L("Controls"), buildControls(), Qt::RightDockWidgetArea);
	QDockWidget *session = addDock("session", L("Stats"), buildSession(), Qt::RightDockWidgetArea, true);
	splitDockWidget(controls, session, Qt::Vertical);
	QDockWidget *chatDock = addDock("chat", L("Chat"), chat, Qt::RightDockWidgetArea);
	QDockWidget *eventsDock = addDock("events", L("Events"), events, Qt::RightDockWidgetArea, true);
	QDockWidget *goLive = addDock("golive", L("Go Live to"), buildGoLive(), Qt::RightDockWidgetArea);
	tabifyDockWidget(session, goLive);
	tabifyDockWidget(goLive, chatDock);
	tabifyDockWidget(chatDock, eventsDock);
	session->raise();
	QDockWidget *mixer = addDock("mixer", L("Audio Mixer"), m_mixer, Qt::BottomDockWidgetArea);
	QDockWidget *transitions = addDock("transitions", L("Scene Transitions"), buildTransitions(), Qt::BottomDockWidgetArea, true);
	splitDockWidget(mixer, transitions, Qt::Horizontal);
	resizeDocks({scenes, sources}, {220, 380}, Qt::Vertical);
	resizeDocks({scenes, controls}, {280, 300}, Qt::Horizontal);
	resizeDocks({controls, session}, {470, 300}, Qt::Vertical);
	resizeDocks({mixer, transitions}, {700, 280}, Qt::Horizontal);
	resizeDocks({mixer}, {200}, Qt::Vertical);
	buildMenuBar();
	m_defaultDocks = saveState(DOCKS_VERSION);
	{
		Settings &st = Settings::get();
		setFullHeightDocks(st.value("docks/fullHeight", true).toBool());
		const QByteArray saved = QByteArray::fromBase64(st.value("docks/state").toString().toLatin1());
		if (!saved.isEmpty())
			restoreState(saved, DOCKS_VERSION);
		m_savedDocks = saved.isEmpty() ? m_defaultDocks : saved;
		setDocksLocked(st.value("docks/locked", false).toBool());
	}
	connect(qApp, &QCoreApplication::aboutToQuit, this, &MainWindow::saveDocks);
	// also every few seconds when it changed: a crash or a forced close must not lose the layout
	auto *dockSaver = new QTimer(this);
	connect(dockSaver, &QTimer::timeout, this, [this] {
		if (isVisible() && saveState(DOCKS_VERSION) != m_savedDocks)
			saveDocks();
	});
	dockSaver->start(3000);
	buildTray();

	m_bannerTimer = new QTimer(this);
	m_bannerTimer->setSingleShot(true);
	connect(m_bannerTimer, &QTimer::timeout, m_banner, &QLabel::hide);
	m_ticker = new QTimer(this);
	connect(m_ticker, &QTimer::timeout, this, &MainWindow::tick);
	m_ticker->start(1000);

	connect(m_core, &ObsCore::streamingChanged, this, [this](bool on) {
		Settings &st = Settings::get();
		st.set("events/liveSince", on ? QDateTime::currentMSecsSinceEpoch() : 0);
		if (on) {
			m_liveSince.start();
			say(L("You're live!"));
			if (st.value("stream/autoRecord", false).toBool() && !m_core->recording()) {
				toggleRecord();
				setProperty("autoRecording", true);
			}
		} else if (property("autoRecording").toBool()) {
			setProperty("autoRecording", false);
			if (st.value("stream/autoRecordStop", true).toBool() && m_core->recording())
				m_core->stopRecording();
		}
		updateButtons();
		updatePreview();
	});
	connect(m_core, &ObsCore::streamingChanged, this, [this] { refreshGoLive(false); });
	connect(m_core, &ObsCore::recordingChanged, this, [this](bool on, const QString &file) {
		if (on)
			m_recSince.start();
		if (!on && QFileInfo::exists(file))
			say(L("Recording saved: ") + QFileInfo(file).fileName());
		updateButtons();
		updatePreview();
	});
	connect(m_core, &ObsCore::replayChanged, this, [this](bool on) {
		if (on)
			say(QString(L("Replay buffer on: Save replay keeps the last %1 seconds."))
				    .arg(Settings::get().value("replay/seconds", 30).toInt()));
		updateButtons();
	});
	connect(m_core, &ObsCore::replaySaved, this, [this](const QString &file) {
		say(L("Replay saved: ") + QFileInfo(file).fileName());
	});
	connect(m_core, &ObsCore::problem, this, [this](const QString &text) { say(text, true); });
	connect(m_core, &ObsCore::notice, this, [this](const QString &text) { say(text); });
	connect(m_core, &ObsCore::sceneChanged, this, &MainWindow::refreshSources);
	m_remote = new Remote(m_core, this);
	m_remote->action = [this](const QString &action) { hotkey(action); };
	connect(m_remote, &Remote::audioChanged, this, [this] { m_mixer->refresh(true); });
	connect(qApp, &QCoreApplication::aboutToQuit, m_remote, [this] { m_remote->stop(true); });
	applyRemote();
	connect(m_assist, &Assist::marked, m_remote, &Remote::announceMark); // Clip Studio clips these
	connect(m_core, &ObsCore::scenesChanged, this, &MainWindow::refreshScenes);
	connect(m_modes, &WindowModes::visibilityChanged, this, [this](bool) { updatePreview(); });
	connect(m_mixer, &Mixer::filtersRequested, this, &MainWindow::openFilters);
	connect(m_mixer, &Mixer::audioRequested, this, &MainWindow::openAudio);
	connect(m_assist, &Assist::alarm, this, [this](const QString &text) {
		say(text, text.contains("silent") || text.contains("black"));
		if (!isActiveWindow())
			m_modes->notify(text);
	});
	connect(m_assist, &Assist::reportReady, this, [this](const QString &html) {
		if (!Settings::get().value("report/show", true).toBool())
			return;
		auto *dialog = new QDialog(this);
		dialog->setAttribute(Qt::WA_DeleteOnClose);
		dialog->setWindowTitle(L("How your stream went"));
		auto *view = new QTextBrowser;
		view->setHtml(html);
		view->setStyleSheet("QTextBrowser { background: transparent; border: none; }");
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
		connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::accept);
		auto *l = new QVBoxLayout(dialog);
		l->addWidget(view);
		l->addWidget(buttons);
		dialog->resize(520, 420);
		dialog->show();
	});
	connect(m_core, &ObsCore::recordingPausedChanged, this, [this](bool paused) {
		say(paused ? L("Recording paused. Nothing is written until you resume.") : L("Recording resumed."));
		updateButtons();
	});
	connect(m_core, &ObsCore::virtualCamChanged, this, [this](bool on) {
		if (on)
			say(L("Virtual camera on: pick \"OBS Virtual Camera\" as the camera in Discord, Zoom, Meet or any app."));
		updateButtons();
	});
	connect(m_core, &ObsCore::screenshotSaved, this,
		[this](const QString &file) { say(L("Screenshot saved: ") + QFileInfo(file).fileName()); });
	connect(m_core, &ObsCore::studioModeChanged, this, [this] { updateStudio(); });
	connect(m_core, &ObsCore::verticalChanged, this, [this](bool on) {
		updateCanvasSwitch();
		if (on)
			say(L("Vertical canvas on. Switch to \"Vertical 9:16\" above the preview to arrange it; it follows the "
			    "scene that's on air, cropped to the middle."));
	});
	connect(m_core, &ObsCore::verticalEditChanged, this, [this](bool on) {
		updateCanvasSwitch();
		m_preview->setSource(on ? m_core->verticalScene() : (m_core->studioMode() ? m_core->editSceneSource() : nullptr));
		m_scenes->setEnabled(!on);
		refreshSources();
	});
	connect(m_core, &ObsCore::collectionChanged, this, [this] {
		refreshScenes();
		refreshSources();
		say(L("Scene collection: ") + m_core->collection());
	});
	// Ctrl+Z / Ctrl+Y undo and redo; Ctrl+C / Ctrl+V copy a source's size and place to another
	auto shortcut = [this](const QKeySequence &keys, auto fn) {
		auto *sc = new QShortcut(keys, this);
		sc->setContext(Qt::WindowShortcut);
		connect(sc, &QShortcut::activated, this, fn);
	};
	shortcut(QKeySequence::Undo, [this] { m_core->undo(); });
	shortcut(QKeySequence("Ctrl+Y"), [this] { m_core->redo(); });
	shortcut(QKeySequence("Ctrl+Shift+Z"), [this] { m_core->redo(); });
	shortcut(QKeySequence::Copy, [this] {
		if (!selectedSource().isEmpty()) {
			m_core->copyTransform(selectedSource());
			say(L("Size and place copied. Select another source and press Ctrl+V."));
		}
	});
	shortcut(QKeySequence::Paste, [this] {
		if (!selectedSource().isEmpty())
			m_core->pasteTransform(selectedSource());
	});
	shortcut(QKeySequence("Ctrl+R"), [this] {
		if (!selectedSource().isEmpty())
			m_core->rotate(selectedSource(), 90);
	});
	refreshScenes();
	refreshSources();
	updateButtons();
	if (Settings::get().value("scenes/studio", false).toBool())
		m_core->setStudioMode(true);
	updateStudio();
	updateCanvasSwitch();

	overlays::Engine::get().openDesigner = [this](obs_source_t *source) {
		AlertDesigner designer(source, this);
		designer.exec();
		m_core->save();
	};
	{
		const QStringList args = QCoreApplication::arguments();
		if (std::none_of(args.begin(), args.end(), [](const QString &a) { return a.startsWith("--selftest"); }))
			EventHub::get().restart(); // chat and alerts connect in the background
	}

	m_hotkeys = new Hotkeys(this);
	connect(m_hotkeys, &Hotkeys::triggered, this, &MainWindow::hotkey);
	applyHotkeys();

	Settings &s = Settings::get();
	applyLevel();
	{
		const QStringList a = QCoreApplication::arguments();
		const bool test = std::any_of(a.begin(), a.end(), [](const QString &x) { return x.startsWith("--selftest") || x.startsWith("--demo"); });
		if (!s.value("setup/done", false).toBool()) {
			if (s.value("welcome/shown", false).toBool())
				s.set("setup/done", true); // set up before the setup existed: keep everything as it is
			else if (!test)
				QTimer::singleShot(400, this, [this] { runSetup(); });
		}
	}
	if (!s.value("welcome/shown", false).toBool()) {
		const VideoPlan &p = m_core->plan();
		say(QString(L("Auto-Tune set this PC to %1: %2p %3 fps with %4. Add a Game or Screen source to begin."))
			    .arg(profileName(profileFromKey(s.value("video/profile").toString())))
			    .arg(p.height)
			    .arg(p.fps)
			    .arg(encoderLabel(p.encoder)));
		s.set("welcome/shown", true);
	}

	// Updates: a quiet check a little after start, never during a self-test.
	m_updater = new Updater(this);
	connect(m_updater, &Updater::found, this, [this](const UpdateInfo &info) {
		m_update = info;
		m_installer.clear();
		showUpdate(QString(L("<b>Zunimi Studio %1 is out.</b> %2")).arg(info.version.toHtmlEscaped(),
									     info.notes.toHtmlEscaped()),
			   Updater::installed() ? L("Update now") : L("Download page"), L("Later"));
	});
	connect(m_updater, &Updater::nothing, this,
		[this] { say(QString(L("You have the newest version (%1).")).arg(ZUNIMI_VERSION)); });
	connect(m_updater, &Updater::progress, this, [this](double f) {
		m_updateText->setText(QString(L("Downloading Zunimi Studio %1... %2%"))
					      .arg(m_update.version.toHtmlEscaped())
					      .arg(int(f * 100)));
	});
	connect(m_updater, &Updater::ready, this, [this](const QString &path) {
		m_installer = path;
		showUpdate(QString(L("<b>Zunimi Studio %1 is ready to install.</b> It takes a few seconds and opens "
				   "again by itself."))
				   .arg(m_update.version.toHtmlEscaped()),
			   L("Restart and update"), L("Later"));
	});
	connect(m_updater, &Updater::failed, this, [this](const QString &reason) {
		if (m_update.version.isEmpty())
			say(reason, true);
		else
			showUpdate(reason.toHtmlEscaped(), L("Try again"), L("Close"));
	});
	const QString leftover = m_updater->afterStart();
	if (!leftover.isEmpty())
		say(leftover, true);
	const QStringList args = QCoreApplication::arguments();
	const bool selftest = std::any_of(args.begin(), args.end(), [](const QString &a) { return a.startsWith("--selftest"); });
	if (!selftest && s.value("update/auto", true).toBool())
		QTimer::singleShot(8000, m_updater, [this] { m_updater->check(false); });
	if (!selftest && s.value("replay/autoStart", false).toBool())
		QTimer::singleShot(1500, this, [this] {
			if (!m_core->replaying())
				toggleReplay();
		});
}

void MainWindow::checkForUpdates()
{
	m_updater->check(true);
}

// ---- layout
void MainWindow::buildMenuBar()
{
	QMenuBar *bar = menuBar();
	bar->setObjectName("menubar");
	// left: logo, name and version; right: stream health, Fix it and the Auto-Tune profile
	auto *brandBox = new QWidget;
	auto *brandRow = new QHBoxLayout(brandBox);
	brandRow->setContentsMargins(10, 6, 14, 6);
	brandRow->setSpacing(8);
	auto *logo = new QLabel;
	logo->setPixmap(QPixmap(":/logo-64.png").scaled(26, 26, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	auto *brand = new QLabel(QString("Zunimi <span style='color:%1'>Stream+</span>").arg(Theme::Accent2.name()));
	brand->setObjectName("brand");
	brand->setTextFormat(Qt::RichText);
	auto *version = new QLabel("v" ZUNIMI_VERSION);
	version->setObjectName("pill");
	brandRow->addWidget(logo);
	brandRow->addWidget(brand);
	brandRow->addWidget(version);
	bar->setCornerWidget(brandBox, Qt::TopLeftCorner);

	auto *status = new QWidget;
	auto *row = new QHBoxLayout(status);
	row->setContentsMargins(8, 4, 10, 4);
	row->setSpacing(8);
	m_healthDot = new QLabel;
	m_healthDot->setFixedSize(10, 10);
	m_health = new QLabel(L("Ready"));
	m_health->setObjectName("mute");
	m_fix = button(L("Fix it"), "primary", L("Stream Doctor: one click to fix what the health bar shows"));
	m_fix->hide();
	connect(m_fix, &QPushButton::clicked, this, [this] {
		m_assist->applyFix(m_fixId);
		m_fix->hide();
	});
	m_profile = new QLabel;
	m_profile->setObjectName("chip");
	row->addWidget(m_healthDot);
	row->addWidget(m_health);
	row->addWidget(m_fix);
	row->addWidget(m_profile);
	bar->setCornerWidget(status, Qt::TopRightCorner);

	// each menu is filled when it opens, so it always shows the current state (undo text, collections...)
	auto menu = [this, bar](const QString &title, void (MainWindow::*fill)(QMenu *)) {
		QMenu *m = bar->addMenu(title);
		connect(m, &QMenu::aboutToShow, this, [this, m, fill] {
			m->clear();
			(this->*fill)(m);
		});
		return m;
	};
	menu(L("File"), &MainWindow::fillFileMenu);
	menu(L("Edit"), &MainWindow::fillEditMenu);
	menu(L("View"), &MainWindow::fillViewMenu);
	menu(L("Docks"), &MainWindow::fillDocksMenu);
	QMenu *profile = menu(L("Profile"), &MainWindow::fillProfilesMenu);
	QMenu *collection = menu(L("Scene Collection"), &MainWindow::fillCollectionsMenu);
	profile->menuAction()->setProperty("proMenu", true);
	collection->menuAction()->setProperty("proMenu", true);
	menu(L("Tools"), &MainWindow::fillToolsMenu);
	menu(L("Help"), &MainWindow::fillHelpMenu);
}

QDockWidget *MainWindow::addDock(const QString &id, const QString &title, QWidget *content, Qt::DockWidgetArea area,
				 bool pro)
{
	auto *dock = new QDockWidget(title, this);
	dock->setObjectName("dock_" + id); // saveState/restoreState find docks by this name
	auto *body = new QWidget;
	body->setObjectName("dockBody");
	body->setAttribute(Qt::WA_StyledBackground);
	auto *col = new QVBoxLayout(body);
	col->setContentsMargins(10, 8, 10, 10);
	col->addWidget(content);
	dock->setWidget(body);
	dock->setProperty("proDock", pro);
	dock->toggleViewAction()->setText(title);
	addDockWidget(area, dock);
	m_docks << dock;
	return dock;
}

void MainWindow::fillDocksMenu(QMenu *menu)
{
	const bool easy = simple();
	for (QDockWidget *dock : m_docks) {
		if (easy && dock->property("proDock").toBool())
			continue;
		menu->addAction(dock->toggleViewAction());
	}
	menu->addSeparator();
	QAction *lock = menu->addAction(L("Lock docks"), this, [this](bool on) { setDocksLocked(on); });
	lock->setCheckable(true);
	lock->setChecked(Settings::get().value("docks/locked", false).toBool());
	lock->setToolTip(L("Docks can't be moved or closed while locked"));
	QAction *full = menu->addAction(L("Full-height side docks"), this, [this](bool on) { setFullHeightDocks(on); });
	full->setCheckable(true);
	full->setChecked(Settings::get().value("docks/fullHeight", true).toBool());
	menu->addSeparator();
	menu->addAction(L("Reset docks"), this, &MainWindow::resetDocks);
}

void MainWindow::resetDocks()
{
	setFullHeightDocks(true);
	restoreState(m_defaultDocks, DOCKS_VERSION);
	for (QDockWidget *dock : m_docks) {
		dock->setFloating(false);
		dock->setVisible(!(simple() && dock->property("proDock").toBool()));
	}
	saveDocks();
	say(L("Docks are back in their places."));
}

void MainWindow::setDocksLocked(bool locked)
{
	Settings::get().set("docks/locked", locked);
	const auto features = locked ? QDockWidget::NoDockWidgetFeatures
				     : QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
					       QDockWidget::DockWidgetClosable;
	for (QDockWidget *dock : m_docks)
		dock->setFeatures(features);
}

void MainWindow::setFullHeightDocks(bool on)
{
	Settings::get().set("docks/fullHeight", on);
	// on: the side docks run the whole height and the bottom docks sit between them, under the preview
	setCorner(Qt::BottomLeftCorner, on ? Qt::LeftDockWidgetArea : Qt::BottomDockWidgetArea);
	setCorner(Qt::BottomRightCorner, on ? Qt::RightDockWidgetArea : Qt::BottomDockWidgetArea);
	setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
	setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
}

void MainWindow::saveDocks()
{
	m_savedDocks = saveState(DOCKS_VERSION);
	Settings::get().set("docks/state", QString::fromLatin1(m_savedDocks.toBase64()));
}

bool MainWindow::selftestDocks()
{
	bool all = true;
	auto check = [&all](bool ok, const QString &what) {
		all &= ok;
		blog(ok ? LOG_INFO : LOG_ERROR, "selftest docks: %s %s", ok ? "ok  " : "FAIL", what.toUtf8().constData());
	};
	auto dock = [this](const char *id) { return findChild<QDockWidget *>(QString("dock_") + id); };
	const auto free = QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable;
	check(m_docks.size() == 9, QString("%1 docks").arg(m_docks.size()));
	for (QDockWidget *d : m_docks)
		check((d->features() & free) == free, d->windowTitle() + " can be moved, floated and closed");
	QDockWidget *mixer = dock("mixer"), *transitions = dock("transitions"), *events = dock("events"), *scenes = dock("scenes");
	check(mixer && transitions && events && scenes, "docks are found by name");
	if (!(mixer && transitions && events && scenes))
		return false;
	check(dockWidgetArea(scenes) == Qt::LeftDockWidgetArea && dockWidgetArea(mixer) == Qt::BottomDockWidgetArea,
	      "default places: Scenes left, Audio Mixer below the preview");

	// the user rearranges: mixer to the right, transitions floating, events closed
	addDockWidget(Qt::RightDockWidgetArea, mixer);
	transitions->setFloating(true);
	events->hide();
	saveDocks();
	const QByteArray mine = saveState(DOCKS_VERSION);
	resetDocks();
	check(dockWidgetArea(mixer) == Qt::BottomDockWidgetArea && !transitions->isFloating() && !events->isHidden(),
	      "Reset docks puts every dock back and shows it");
	restoreState(mine, DOCKS_VERSION);
	check(dockWidgetArea(mixer) == Qt::RightDockWidgetArea && transitions->isFloating() && events->isHidden(),
	      "a saved layout comes back exactly (moved, floating, closed)");
	resetDocks();

	setDocksLocked(true);
	bool locked = true;
	for (QDockWidget *d : m_docks)
		locked &= d->features() == QDockWidget::NoDockWidgetFeatures;
	check(locked, "Lock docks stops moving and closing");
	setDocksLocked(false);
	check((scenes->features() & free) == free, "unlocking frees them again");

	setSimple(true);
	check(transitions->isHidden() && events->isHidden() && !scenes->isHidden(), "Simple mode hides the Pro docks only");
	setSimple(false);
	check(!transitions->isHidden() && !events->isHidden(), "Pro mode brings them back where they were");

	setFullHeightDocks(false);
	check(corner(Qt::BottomLeftCorner) == Qt::BottomDockWidgetArea, "full-height side docks can be turned off");
	setFullHeightDocks(true);
	check(corner(Qt::BottomLeftCorner) == Qt::LeftDockWidgetArea, "and on");
	// leave one change behind, like a user would: the auto-save must keep it even if the app dies
	addDockWidget(Qt::RightDockWidgetArea, mixer);
	blog(LOG_INFO, "selftest docks: %s", all ? "all passed" : "SOME FAILED");
	return all;
}

void MainWindow::raiseDock(const QString &id)
{
	if (QDockWidget *d = findChild<QDockWidget *>("dock_" + id)) {
		d->show();
		d->raise();
	}
}

int MainWindow::dockArea(const QString &id)
{
	QDockWidget *d = findChild<QDockWidget *>("dock_" + id);
	return d ? int(dockWidgetArea(d)) : -1;
}

QWidget *MainWindow::buildScenes()
{
	auto *col = new QVBoxLayout;
	col->setContentsMargins(0, 0, 0, 0);
	col->setSpacing(8);
	m_scenes = new QListWidget;
	m_scenes->setToolTip(L("Click to switch. Double-click to rename. Hotkeys for scenes 1-9 are in Settings > Hotkeys."));
	m_scenes->setMinimumHeight(90);
	auto *add = button("+", "icon", L("New scene"));
	auto *copy = button("⧉", "icon", L("Duplicate this scene (sources are shared)"));
	auto *rename = button("✎", "icon", L("Rename"));
	auto *remove = button("✕", "icon", L("Remove this scene"));
	auto *up = button("↑", "icon", L("Move up"));
	auto *down = button("↓", "icon", L("Move down"));
	for (QPushButton *b : {copy, up, down})
		b->setProperty("pro", true);
	auto *tools = new QHBoxLayout;
	tools->setSpacing(4);
	for (QPushButton *b : {add, copy, rename, remove})
		tools->addWidget(b);
	tools->addStretch();
	tools->addWidget(up);
	tools->addWidget(down);

	connect(m_scenes, &QListWidget::currentRowChanged, this, [this](int row) {
		if (row >= 0)
			m_core->setScene(m_scenes->item(row)->data(Qt::UserRole).toString());
	});
	m_scenes->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(m_scenes, &QListWidget::customContextMenuRequested, this,
		[this](const QPoint &pos) { sceneMenu(m_scenes->viewport()->mapToGlobal(pos)); });
	connect(m_scenes, &QListWidget::itemDoubleClicked, this, [this] { renameScene(); });
	auto *addMenu = new QMenu(add);
	addMenu->addAction(L("Empty scene..."), this, [this] {
		bool ok = false;
		const QString name = QInputDialog::getText(this, L("New scene"), L("Name"), QLineEdit::Normal,
							   QString(L("Scene %1")).arg(m_core->scenes().size() + 1), &ok);
		if (ok)
			m_core->addScene(name);
	});
	addMenu->addAction(L("From a template...  (Starting soon, BRB, Gaming...)"), this, [this] {
		TemplatesDialog dialog(m_core, this);
		if (dialog.exec() == QDialog::Accepted)
			say(QString(L("\"%1\" added. Double-click the title to change its words.")).arg(dialog.added()));
	});
	add->setMenu(addMenu);
	connect(copy, &QPushButton::clicked, this, [this] { m_core->duplicateScene(m_core->currentScene()); });
	connect(rename, &QPushButton::clicked, this, [this] { renameScene(); });
	connect(remove, &QPushButton::clicked, this, [this] {
		const QString name = m_core->currentScene();
		if (m_core->scenes().size() <= 1) {
			say(L("A scene is always kept. Add another before removing this one."));
			return;
		}
		if (QMessageBox::question(this, L("Remove scene"),
					  QString(L("Remove the scene \"%1\"? Sources that only this scene uses go too."))
						  .arg(name)) == QMessageBox::Yes)
			m_core->removeScene(name);
	});
	connect(up, &QPushButton::clicked, this, [this] { m_core->moveScene(m_core->currentScene(), true); });
	connect(down, &QPushButton::clicked, this, [this] { m_core->moveScene(m_core->currentScene(), false); });

	col->addWidget(m_scenes, 1);
	col->addLayout(tools);
	auto *w = new QWidget;
	w->setLayout(col);
	w->setMinimumWidth(230);
	return w;
}

QWidget *MainWindow::buildSources()
{
	auto *col = new QVBoxLayout;
	col->setContentsMargins(0, 0, 0, 0);
	col->setSpacing(8);
	m_sources = new QListWidget;
	m_sources->setToolTip(L("Tick to show or hide. Double-click for its options. Right-click for more."));
	m_sources->setContextMenuPolicy(Qt::CustomContextMenu);
	m_sources->setSelectionMode(QAbstractItemView::ExtendedSelection); // Ctrl/Shift-click several to group them
	m_empty = new QLabel(L("Nothing here yet.\nAdd a Game or your Screen to start."));
	m_empty->setObjectName("mute");
	m_empty->setAlignment(Qt::AlignCenter);
	auto *add = button(L("+  Add source"), "primary");
	auto *menu = new QMenu(add);
	connect(menu, &QMenu::aboutToShow, this, [this, menu] { fillAddMenu(menu); });
	add->setMenu(menu);
	auto *up = button("↑", "icon", L("Move up (in front)"));
	auto *down = button("↓", "icon", L("Move down (behind)"));
	auto *props = button(L("Options"), "icon", L("Everything this source can do"));
	auto *filters = button(L("Filters"), "icon", L("Green screen, colour, crop, sharpen..."));
	filters->setProperty("pro", true);
	auto *remove = button("✕", "icon", L("Remove the selected source (Delete)"));
	auto *tools = new QHBoxLayout;
	tools->setSpacing(4);
	tools->addWidget(up);
	tools->addWidget(down);
	tools->addStretch();
	tools->addWidget(props);
	tools->addWidget(filters);
	tools->addWidget(remove);

	connect(m_sources, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
		m_core->setVisible(item->data(Qt::UserRole).toString(), item->checkState() == Qt::Checked);
	});
	connect(m_sources, &QListWidget::currentItemChanged, this, [this] { m_preview->select(selectedSource()); });
	connect(m_sources, &QListWidget::itemDoubleClicked, this, [this] { openProperties(selectedSource()); });
	connect(m_sources, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
		if (QListWidgetItem *item = m_sources->itemAt(pos))
			m_sources->setCurrentItem(item);
		sourceMenu(selectedSource(), m_sources->viewport()->mapToGlobal(pos));
	});
	connect(up, &QPushButton::clicked, this, [this] {
		if (!selectedSource().isEmpty())
			m_core->moveSource(selectedSource(), true);
	});
	connect(down, &QPushButton::clicked, this, [this] {
		if (!selectedSource().isEmpty())
			m_core->moveSource(selectedSource(), false);
	});
	connect(props, &QPushButton::clicked, this, [this] { openProperties(selectedSource()); });
	connect(filters, &QPushButton::clicked, this, [this] {
		if (obs_sceneitem_t *item = m_core->item(selectedSource()))
			openFilters(obs_sceneitem_get_source(item));
	});
	connect(remove, &QPushButton::clicked, this, [this] {
		if (!selectedSource().isEmpty())
			m_core->removeSource(selectedSource());
	});
	auto *del = new QShortcut(QKeySequence::Delete, m_sources, nullptr, nullptr, Qt::WidgetShortcut);
	connect(del, &QShortcut::activated, this, [this] {
		if (!selectedSource().isEmpty())
			m_core->removeSource(selectedSource());
	});

	col->addWidget(m_sources, 1);
	col->addWidget(m_empty, 1);
	col->addLayout(tools);
	col->addWidget(add);
	auto *w = new QWidget;
	w->setLayout(col);
	w->setMinimumWidth(230);
	return w;
}

void MainWindow::fillAddMenu(QMenu *menu)
{
	menu->clear();
	auto offer = [this, menu](const Kind &k) {
		if (obs_get_source_output_flags(k.id) != 0) // a module that is not loaded is not offered
			menu->addAction(L(k.label), this, [this, id = QString(k.id)] { addSource(id); });
	};
	offer(KINDS[0]); // game
	// every screen by name, so a second or third monitor is one click away
	const auto screens = m_core->screens();
	if (screens.size() == 1) {
		menu->addAction(screenLabel(1, screens[0].first), this,
				[this, s = screens[0]] { addScreen(s.second, screenLabel(1, s.first)); });
	} else if (!screens.isEmpty()) {
		QMenu *sub = menu->addMenu(QString(L("Screen  (%1 found)")).arg(screens.size()));
		for (int i = 0; i < screens.size(); i++)
			sub->addAction(screenLabel(i + 1, screens[i].first), this,
				       [this, s = screens[i], i] { addScreen(s.second, screenLabel(i + 1, s.first)); });
		sub->addSeparator();
		sub->addAction(L("All screens, side by side"), this, &MainWindow::addAllScreens);
	}
	for (int i = 1; i < int(std::size(KINDS)); i++) {
		if (i == 3 || i == 9)
			menu->addSeparator(); // sound sources, then extra devices
		offer(KINDS[i]);
	}
	menu->addSeparator();
	QMenu *widgets = menu->addMenu(L("Alerts and widgets  (no browser needed)"));
	widgets->addAction(L("Alert box  (follows, subs, tips, raids...)"), this, [this] { addOverlay("alerts"); });
	widgets->addAction(L("Chat box  (Twitch, YouTube, Kick together)"), this, [this] { addOverlay("chat"); });
	widgets->addAction(L("Goal bar"), this, [this] { addOverlay("goal"); });
	widgets->addAction(L("Label  (latest follower, top tipper...)"), this, [this] { addOverlay("label"); });
	widgets->addAction(L("Timer  (countdown, subathon, clock)"), this, [this] { addOverlay("timer"); });
	widgets->addAction(L("Recent events list"), this, [this] { addOverlay("events"); });
	widgets->addSeparator();
	widgets->addAction(L("StreamElements / Streamlabs overlay or widget link..."), this, [this] { addBrowser(true); });
	menu->addAction(L("Web page  (browser source)"), this, [this] { addBrowser(false); });
	if (simple())
		return;
	const QStringList nestable = m_core->nestableScenes();
	if (!nestable.isEmpty()) {
		menu->addSeparator();
		QMenu *sub = menu->addMenu(L("Another scene  (inside this one)"));
		for (const QString &name : nestable)
			sub->addAction(name, this, [this, name] {
				m_core->addExisting(name);
				selectSource(name);
			});
	}
	const QStringList existing = m_core->otherScenesSources();
	if (!existing.isEmpty()) {
		menu->addSeparator();
		QMenu *sub = menu->addMenu(L("From another scene"));
		for (const QString &name : existing)
			sub->addAction(name, this, [this, name] {
				m_core->addExisting(name);
				selectSource(name);
			});
	}
}

QWidget *MainWindow::buildPreview()
{
	auto *col = new QVBoxLayout;
	col->setContentsMargins(8, 8, 8, 8);
	m_previewStack = new QStackedWidget;
	m_preview = new PreviewWidget(m_core);
	m_preview->setToolTip({});
	auto *paused = new QWidget;
	auto *pausedCol = new QVBoxLayout(paused);
	auto *note = new QLabel(L("Preview paused to give your PC more power.\nYour stream and recording are not affected."));
	note->setObjectName("preview_note");
	note->setAlignment(Qt::AlignCenter);
	auto *show = button(L("Show preview"));
	connect(show, &QPushButton::clicked, this, [this] {
		m_previewWanted = true;
		Settings::get().set("preview/pauseWhileLive", false);
		updatePreview();
	});
	pausedCol->addStretch();
	pausedCol->addWidget(note);
	pausedCol->addWidget(show, 0, Qt::AlignCenter);
	pausedCol->addStretch();
	m_previewStack->addWidget(m_preview);
	m_previewStack->addWidget(paused);
	connect(m_preview, &PreviewWidget::selected, this, [this](const QString &name) {
		QSignalBlocker block(m_sources);
		m_sources->setCurrentItem(nullptr);
		for (int i = 0; i < m_sources->count(); i++)
			if (m_sources->item(i)->data(Qt::UserRole).toString() == name)
				m_sources->setCurrentRow(i);
	});
	connect(m_preview, &PreviewWidget::menuRequested, this, &MainWindow::sourceMenu);

	// Studio mode: the edited scene on the left, what is on air on the right.
	m_previewTitle = new QLabel(L("Preview  (edit here, then Go)"));
	m_previewTitle->setObjectName("mute");
	m_landscapeButton = button(L("Landscape  16:9"), "", L("Edit the normal picture"));
	m_verticalButton = button(L("Vertical  9:16"), "", L("Edit the vertical picture (Shorts, Reels, TikTok)"));
	for (QPushButton *b : {m_landscapeButton, m_verticalButton})
		b->setCheckable(true);
	connect(m_landscapeButton, &QPushButton::clicked, this, [this] { m_core->setEditVertical(false); });
	connect(m_verticalButton, &QPushButton::clicked, this, [this] { m_core->setEditVertical(true); });
	auto *verticalMore = button("⋯", "icon", L("Vertical canvas settings"));
	connect(verticalMore, &QPushButton::clicked, this, &MainWindow::verticalSettings);
	m_canvasSwitch = new QWidget;
	auto *switchRow = new QHBoxLayout(m_canvasSwitch);
	switchRow->setContentsMargins(0, 0, 0, 0);
	switchRow->setSpacing(4);
	switchRow->addWidget(m_landscapeButton);
	switchRow->addWidget(m_verticalButton);
	switchRow->addWidget(verticalMore);
	switchRow->addStretch();
	auto *left = new QVBoxLayout;
	left->setSpacing(4);
	left->addWidget(m_canvasSwitch);
	left->addWidget(m_previewTitle);
	left->addWidget(m_previewStack, 1);
	m_program = new PreviewWidget(m_core, PreviewWidget::Mode::Program);
	auto *onAir = new QLabel(L("On air"));
	onAir->setObjectName("mute");
	auto *go = button(L("Go"), "primary", L("Send the preview live with the scene's transition (hotkey in Settings > Hotkeys)"));
	connect(go, &QPushButton::clicked, m_core, &ObsCore::transitionNow);
	auto *programCol = new QVBoxLayout;
	programCol->setContentsMargins(0, 0, 0, 0);
	programCol->setSpacing(4);
	auto *programHead = new QHBoxLayout;
	programHead->addWidget(onAir, 1);
	programHead->addWidget(go);
	programCol->addLayout(programHead);
	programCol->addWidget(m_program, 1);
	m_programBox = new QWidget;
	m_programBox->setLayout(programCol);
	auto *row = new QHBoxLayout;
	row->setSpacing(10);
	row->addLayout(left, 1);
	row->addWidget(m_programBox, 1);
	col->addLayout(row);
	return panel(col);
}

QWidget *MainWindow::buildTransitions()
{
	m_transition = new QComboBox;
	for (const auto &[id, label] : ObsCore::transitionKinds())
		m_transition->addItem(label, id);
	m_transition->setCurrentIndex(std::max(0, m_transition->findData(m_core->transitionId())));
	m_transitionMs = new QSpinBox;
	m_transitionMs->setRange(0, 20000);
	m_transitionMs->setSingleStep(100);
	m_transitionMs->setSuffix(" ms");
	m_transitionMs->setValue(m_core->transitionMs());
	auto applyTransition = [this] {
		m_core->setTransition(m_transition->currentData().toString(), m_transitionMs->value());
	};
	connect(m_transition, &QComboBox::activated, this, [this, applyTransition] {
		if (m_transition->currentData().toString() == "obs_stinger_transition")
			chooseTransition(); // needs a video
		else
			applyTransition();
	});
	connect(m_transitionMs, &QSpinBox::editingFinished, this, applyTransition);

	auto *form = new QFormLayout;
	form->setContentsMargins(0, 0, 0, 0);
	form->setSpacing(8);
	form->addRow(L("Transition"), m_transition);
	form->addRow(L("Duration"), m_transitionMs);
	auto *w = new QWidget;
	auto *col = new QVBoxLayout(w);
	col->setContentsMargins(0, 0, 0, 0);
	col->addLayout(form);
	col->addStretch();
	w->setMinimumWidth(240);
	return w;
}

QWidget *MainWindow::buildControls()
{
	auto *col = new QVBoxLayout;
	col->setContentsMargins(0, 0, 0, 0);
	col->setSpacing(8);
	m_live = button(L("Start Streaming"), "live", L("Start or stop streaming"));
	m_rec = button(L("Start Recording"), "rec", L("Start or stop recording"));
	m_pause = button(L("Pause"), "", L("Pause the recording without ending the file"));
	m_vcam = button(L("Start Virtual Camera"), "", L("Use what's on air as a webcam in Discord, Zoom, Meet and other apps"));
	m_vcam->setCheckable(true);
	m_vcam->setProperty("pro", true);
	m_studio = button(L("Studio Mode"), "", L("Edit the preview, then press Go to send it live"));
	m_studio->setCheckable(true);
	m_studio->setProperty("pro", true);
	m_replay = button(L("Replay buffer"), "", L("Keeps the last seconds in memory so you can save a moment after it happens"));
	m_replay->setCheckable(true);
	m_saveReplay = button(L("Save replay"), "", L("Saves the last seconds as an MP4 (set a hotkey in Settings > Hotkeys)"));
	auto *settings = button(L("Settings"), "", L("Stream key, quality, recording, replay, hotkeys, window"));
	auto *exit = button(L("Exit"), "", L("Quit (asks first while you are live or recording)"));
	connect(m_live, &QPushButton::clicked, this, &MainWindow::toggleStream);
	connect(m_rec, &QPushButton::clicked, this, &MainWindow::toggleRecord);
	connect(m_pause, &QPushButton::clicked, this, &MainWindow::togglePause);
	connect(m_vcam, &QPushButton::clicked, this, &MainWindow::toggleVirtualCam);
	connect(m_studio, &QPushButton::clicked, this, [this] { m_core->setStudioMode(!m_core->studioMode()); });
	connect(m_replay, &QPushButton::clicked, this, &MainWindow::toggleReplay);
	connect(m_saveReplay, &QPushButton::clicked, this, &MainWindow::saveReplay);
	connect(settings, &QPushButton::clicked, this, [this] { openSettings(); });
	connect(exit, &QPushButton::clicked, m_modes, [this] { m_modes->quit(); });
	col->addWidget(m_live);
	auto *recRow = new QHBoxLayout;
	recRow->setSpacing(8);
	recRow->addWidget(m_rec, 1);
	recRow->addWidget(m_pause);
	col->addLayout(recRow);
	col->addWidget(m_vcam);
	col->addWidget(m_studio);
	auto *replayRow = new QHBoxLayout;
	replayRow->setSpacing(8);
	replayRow->addWidget(m_replay, 1);
	replayRow->addWidget(m_saveReplay);
	col->addLayout(replayRow);
	auto *last = new QHBoxLayout;
	last->setSpacing(8);
	last->addWidget(settings, 1);
	last->addWidget(exit, 1);
	col->addLayout(last);
	// the card at the bottom: a little encouragement and what Auto-Tune chose
	// The Go Live card: one big button that starts the stream, and shows how long you've been live
	m_hero = new QPushButton;
	m_hero->setObjectName("hero");
	m_hero->setCursor(Qt::PointingHandCursor);
	m_hero->setToolTip(L("Start or stop streaming"));
	auto *cardCol = new QVBoxLayout(m_hero);
	cardCol->setContentsMargins(16, 14, 16, 14);
	cardCol->setSpacing(4);
	m_slogan = new QLabel(L("Go Live.\nBe Legendary."));
	m_slogan->setObjectName("slogan");
	m_tune = new QLabel;
	m_tune->setObjectName("heroNote");
	m_tune->setWordWrap(true);
	for (QLabel *l : {m_slogan, m_tune})
		l->setAttribute(Qt::WA_TransparentForMouseEvents); // clicks go to the card
	cardCol->addWidget(m_slogan);
	cardCol->addWidget(m_tune);
	m_hero->setMinimumHeight(118);
	connect(m_hero, &QPushButton::clicked, this, &MainWindow::toggleStream);
	col->addSpacing(4);
	col->addWidget(m_hero);
	col->addStretch();
	auto *w = new QWidget;
	w->setLayout(col);
	w->setMinimumWidth(250);
	w->setMinimumHeight(w->sizeHint().height()); // the dock can't be squeezed until the card disappears
	return w;
}

namespace {
QString platformName(const QString &key)
{
	if (key == "twitch")
		return "Twitch";
	if (key == "youtube")
		return "YouTube";
	if (key == "kick")
		return "Kick";
	if (key == "whip")
		return "WHIP";
	return L("Your server");
}
} // namespace

QWidget *MainWindow::buildGoLive()
{
	auto *w = new QWidget;
	auto *col = new QVBoxLayout(w);
	col->setContentsMargins(0, 0, 0, 0);
	col->setSpacing(8);
	m_goLiveRows = new QVBoxLayout;
	m_goLiveRows->setSpacing(6);
	col->addLayout(m_goLiveRows);
	auto *add = button(L("+  Add destination"), "", L("Another platform, landscape or vertical"));
	auto *manage = button(L("Keys and servers..."), "", L("Settings > Stream"));
	connect(add, &QPushButton::clicked, this, &MainWindow::addDestinationDialog);
	connect(manage, &QPushButton::clicked, this, [this] { openSettings(SettingsDialog::Stream); });
	auto *buttons = new QHBoxLayout;
	buttons->setSpacing(8);
	buttons->addWidget(add, 1);
	buttons->addWidget(manage, 1);
	col->addLayout(buttons);
	auto *note = new QLabel(L("Switch platforms on or off any time, even while live. Vertical 9:16 ones get the "
				  "vertical canvas."));
	note->setObjectName("hint");
	note->setWordWrap(true);
	col->addWidget(note);
	col->addStretch();
	refreshGoLive(true);
	return w;
}

void MainWindow::refreshGoLive(bool rebuild)
{
	if (!m_goLiveRows)
		return;
	const QVector<ObsCore::Destination> list = ObsCore::destinations();
	if (rebuild || m_goLive.size() != list.size() + 1) {
		while (QLayoutItem *item = m_goLiveRows->takeAt(0)) {
			delete item->widget();
			delete item;
		}
		m_goLive.clear();
		auto row = [this](const QString &name, bool vertical, QWidget *trailing) {
			auto *box = new QWidget;
			box->setObjectName("tile");
			box->setAttribute(Qt::WA_StyledBackground);
			auto *r = new QHBoxLayout(box);
			r->setContentsMargins(10, 7, 8, 7);
			r->setSpacing(8);
			GoLiveRow g;
			g.dot = new QLabel;
			g.dot->setFixedSize(10, 10);
			auto *text = new QVBoxLayout;
			text->setSpacing(0);
			auto *title = new QLabel(name);
			g.status = new QLabel;
			g.status->setObjectName("mute");
			text->addWidget(title);
			text->addWidget(g.status);
			auto *pill = new QLabel(vertical ? "9:16" : "16:9");
			pill->setObjectName("pill");
			r->addWidget(g.dot);
			r->addLayout(text, 1);
			r->addWidget(pill);
			if (trailing)
				r->addWidget(trailing);
			box->setMinimumHeight(box->sizeHint().height()); // two lines of text, never clipped
			m_goLiveRows->addWidget(box);
			m_goLive << g;
		};
		Settings &st = Settings::get();
		row(platformName(st.value("stream/platform", "twitch").toString()) + L("  (main)"), false, nullptr);
		for (int i = 0; i < list.size(); i++) {
			auto *on = new QCheckBox;
			on->setChecked(list[i].on);
			on->setToolTip(L("Go live here too"));
			connect(on, &QCheckBox::toggled, this, [this, i](bool v) {
				m_core->setDestinationOn(i, v);
				refreshGoLive(false);
			});
			row(list[i].name.isEmpty() ? list[i].server : list[i].name, list[i].vertical, on);
		}
	}
	// the status lights
	const bool live = m_core->streaming();
	const QVector<ObsCore::DestinationState> states = m_core->destinationStates();
	auto paint = [](const GoLiveRow &g, const QString &state, const QString &error, bool wanted) {
		QColor c = Theme::Line;
		QString text = wanted ? L("Ready") : L("Off");
		if (state == "live") {
			c = Theme::Good;
			text = L("Live");
		} else if (state == "connecting") {
			c = Theme::Warn;
			text = L("Connecting...");
		} else if (state == "reconnecting") {
			c = Theme::Warn;
			text = L("Reconnecting...");
		} else if (state == "failed") {
			c = Theme::Live;
			text = L("Failed: ") + error;
		}
		g.dot->setStyleSheet(QString("background:%1; border-radius:5px;").arg(c.name()));
		g.status->setText(text);
		g.status->setToolTip(error);
	};
	if (!m_goLive.isEmpty())
		paint(m_goLive[0], m_core->mainState(), {}, true);
	for (int i = 0; i < list.size() && i + 1 < m_goLive.size(); i++) {
		const ObsCore::DestinationState s = i < states.size() ? states[i] : ObsCore::DestinationState{"off", {}};
		paint(m_goLive[i + 1], live ? s.state : "off", s.error, list[i].on && !list[i].server.trimmed().isEmpty());
	}
}

void MainWindow::addDestinationDialog()
{
	QDialog dialog(this);
	dialog.setWindowTitle(L("Add destination"));
	struct Preset {
		const char *name, *server;
		bool vertical;
	};
	const Preset presets[] = {{"Twitch", "rtmp://live.twitch.tv/app", false},
				  {"YouTube", "rtmps://a.rtmps.youtube.com:443/live2", false},
				  {"YouTube Shorts", "rtmps://a.rtmps.youtube.com:443/live2", true},
				  {"Kick", "", false},
				  {"Facebook", "rtmps://live-api-s.facebook.com:443/rtmp/", false},
				  {"TikTok", "", true},
				  {"Instagram", "", true},
				  {"Other", "", false}};
	auto *platform = new QComboBox;
	for (const Preset &p : presets)
		platform->addItem(L(p.name));
	auto *name = new QLineEdit;
	auto *server = new QLineEdit;
	server->setPlaceholderText("rtmp://...  rtmps://...  srt://...");
	auto *key = new QLineEdit;
	key->setEchoMode(QLineEdit::Password);
	auto *picture = new QComboBox;
	picture->addItem(L("Landscape 16:9"), false);
	picture->addItem(L("Vertical 9:16"), true);
	auto pick = [&](int i) {
		name->setText(L(presets[i].name));
		server->setText(presets[i].server);
		picture->setCurrentIndex(presets[i].vertical ? 1 : 0);
	};
	connect(platform, &QComboBox::currentIndexChanged, &dialog, pick);
	pick(0);
	auto *form = new QFormLayout;
	form->addRow(L("Platform"), platform);
	form->addRow(L("Name"), name);
	form->addRow(L("Server"), server);
	form->addRow(L("Stream key"), key);
	form->addRow(L("Picture"), picture);
	auto *note = new QLabel(L("Kick, TikTok and Instagram show their server address next to the stream key in their "
				  "live settings. Copy both here."));
	note->setObjectName("hint");
	note->setWordWrap(true);
	form->addRow("", note);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	auto *layout = new QVBoxLayout(&dialog);
	layout->setContentsMargins(18, 18, 18, 18);
	layout->addLayout(form);
	layout->addWidget(buttons);
	dialog.resize(520, 0);
	if (dialog.exec() != QDialog::Accepted)
		return;
	if (server->text().trimmed().isEmpty()) {
		say(L("A destination needs its server address."), true);
		return;
	}
	QVector<ObsCore::Destination> list = ObsCore::destinations();
	list.append({name->text().trimmed(), server->text().trimmed(), key->text().trimmed(), true, picture->currentData().toBool()});
	ObsCore::setDestinations(list);
	if (m_core->streaming())
		m_core->setDestinationOn(list.size() - 1, true); // starts it now
	refreshGoLive(true);
	say(QString(L("%1 added. It goes live with you.")).arg(name->text().trimmed()));
}

QWidget *MainWindow::buildSession()
{
	auto *grid = new QGridLayout;
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setSpacing(8);
	auto tile = [grid](const QString &name, int r, int c) {
		auto *box = new QWidget;
		box->setObjectName("tile");
		box->setAttribute(Qt::WA_StyledBackground);
		auto *col = new QVBoxLayout(box);
		col->setContentsMargins(12, 8, 12, 8);
		col->setSpacing(2);
		auto *label = new QLabel(name);
		label->setObjectName("tileLabel");
		auto *value = new QLabel("00:00:00");
		value->setObjectName("tileValue");
		col->addWidget(label);
		col->addWidget(value);
		grid->addWidget(box, r, c);
		return value;
	};
	m_liveTime = tile(L("LIVE"), 0, 0);
	m_recTime = tile(L("REC"), 0, 1);
	m_cpu = tile(L("CPU"), 1, 0);
	m_fps = tile(L("FPS"), 1, 1);
	m_cpu->setText("–");
	m_fps->setText("–");
	grid->setRowStretch(2, 1); // tiles keep their size; extra room goes below them
	auto *w = new QWidget;
	w->setLayout(grid);
	return w;
}

QWidget *MainWindow::buildUpdateBar()
{
	auto *row = new QHBoxLayout;
	row->setContentsMargins(14, 8, 10, 8);
	row->setSpacing(10);
	m_updateText = new QLabel;
	m_updateText->setWordWrap(true);
	m_updateText->setTextFormat(Qt::RichText);
	m_updateGo = button(L("Update now"), "primary");
	m_updateLater = button(L("Later"));
	connect(m_updateGo, &QPushButton::clicked, this, &MainWindow::updateClicked);
	connect(m_updateLater, &QPushButton::clicked, this, [this] {
		m_updater->cancel();
		m_updateBar->hide();
	});
	row->addWidget(m_updateText, 1);
	row->addWidget(m_updateGo);
	row->addWidget(m_updateLater);
	m_updateBar = panel(row);
	m_updateBar->setStyleSheet(QString("QWidget#panel { border-color: %1; }").arg(Theme::Accent.name()));
	m_updateBar->hide();
	return m_updateBar;
}

void MainWindow::buildTray()
{
	QMenu *menu = m_modes->trayMenu();
	m_trayLive = menu->addAction(L("Go Live"), this, &MainWindow::toggleStream);
	m_trayRec = menu->addAction(L("Start recording"), this, &MainWindow::toggleRecord);
	m_trayReplay = menu->addAction(L("Save replay"), this, &MainWindow::saveReplay);
	menu->addSeparator();
	menu->addAction(L("Settings"), this, [this] {
		m_modes->showAgain();
		openSettings();
	});
	menu->addAction(L("Quit Zunimi Studio"), m_modes, [this] { m_modes->quit(); });
}

// ---- scenes
void MainWindow::refreshScenes()
{
	QSignalBlocker block(m_scenes);
	m_scenes->clear();
	const QStringList names = m_core->scenes();
	const bool studio = m_core->studioMode();
	const QString onAir = m_core->programScene();
	for (int i = 0; i < names.size(); i++) {
		const bool live = studio && names[i] == onAir;
		auto *item = new QListWidgetItem(live ? QString::fromUtf8("\u25CF  ") + names[i] : names[i]);
		item->setData(Qt::UserRole, names[i]);
		if (live)
			item->setForeground(Theme::Live);
		QString tip = i < 9 ? QString(L("Scene %1")).arg(i + 1) : QString();
		if (!m_core->sceneTransition(names[i]).first.isEmpty())
			tip += (tip.isEmpty() ? "" : "  ·  ") + QString(L("Own transition"));
		item->setToolTip(tip);
		m_scenes->addItem(item);
		if (names[i] == m_core->currentScene())
			m_scenes->setCurrentItem(item);
	}
	if (m_core->studioMode()) // the left side shows the scene being edited
		m_preview->setSource(m_core->editSceneSource());
}

void MainWindow::renameScene()
{
	const QString from = m_core->currentScene();
	bool ok = false;
	const QString to = QInputDialog::getText(this, L("Rename scene"), L("Name"), QLineEdit::Normal, from, &ok);
	if (ok && !m_core->renameScene(from, to))
		say(L("That name is already used by a scene or source."), true);
}

// ---- sources
QString MainWindow::selectedSource() const
{
	QListWidgetItem *item = m_sources->currentItem();
	return item ? item->data(Qt::UserRole).toString() : QString();
}

void MainWindow::selectSource(const QString &name)
{
	for (int i = 0; i < m_sources->count(); i++)
		if (m_sources->item(i)->data(Qt::UserRole).toString() == name)
			m_sources->setCurrentRow(i);
	m_preview->select(name);
}

void MainWindow::refreshSources()
{
	QSignalBlocker block(m_sources);
	const QString current = selectedSource();
	m_sources->clear();
	for (const SceneItem &item : m_core->items()) {
		QString text = item.locked ? item.name + "  🔒" : item.name;
		if (item.isGroup)
			text = "▾ " + text;
		else if (!item.group.isEmpty())
			text = "      " + text; // inside the group above
		auto *row = new QListWidgetItem(text);
		row->setData(Qt::UserRole, item.name);
		row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
		row->setCheckState(item.visible ? Qt::Checked : Qt::Unchecked);
		if (item.isGroup)
			row->setToolTip(L("Group: its sources move, hide and lock together. Right-click to ungroup."));
		else if (const Kind *k = kindOf(item.kind))
			row->setToolTip(QString(L(k->label)).section("  ", 0, 0));
		else if (item.kind == "monitor_capture")
			row->setToolTip(L("Screen"));
		m_sources->addItem(row);
		if (item.name == current)
			m_sources->setCurrentItem(row);
	}
	const bool empty = m_sources->count() == 0;
	m_sources->setVisible(!empty);
	m_empty->setVisible(empty);
	m_preview->select(selectedSource());
}

void MainWindow::addScreen(const QString &id, const QString &label)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "monitor_id", id.toUtf8().constData());
	obs_source_t *source = m_core->addSource("monitor_capture", label.section("  ", 0, 0), s, ObsCore::Fit::Fill);
	obs_data_release(s);
	if (!source) {
		say(L("That screen could not be captured."), true);
		return;
	}
	selectSource(QString::fromUtf8(obs_source_get_name(source)));
	say(label.section("  ", 0, 0) + L(" added. Drag a corner in the preview to make it smaller."));
}

void MainWindow::addAllScreens()
{
	// Laid out the way Windows arranges them, scaled to fit the canvas.
	const auto screens = m_core->screens();
	QRectF all;
	QVector<QRectF> rects;
	for (const auto &s : screens) {
		rects << screenRect(s.first);
		all = all.united(rects.last());
	}
	if (all.isEmpty())
		return;
	const QSize canvas = m_core->canvas();
	const double scale = std::min(canvas.width() / all.width(), canvas.height() / all.height());
	const QPointF offset((canvas.width() - all.width() * scale) / 2, (canvas.height() - all.height() * scale) / 2);
	for (int i = 0; i < screens.size(); i++) {
		obs_data_t *s = obs_data_create();
		obs_data_set_string(s, "monitor_id", screens[i].second.toUtf8().constData());
		const QRectF r = rects[i].translated(-all.topLeft());
		m_core->addSourceAt("monitor_capture", screenLabel(i + 1, screens[i].first).section("  ", 0, 0), s,
				    QRectF(offset + r.topLeft() * scale, r.size() * scale));
		obs_data_release(s);
	}
	say(QString(L("All %1 screens added side by side.")).arg(screens.size()));
}

void MainWindow::addSource(const QString &id)
{
	const Kind *kind = kindOf(id);
	if (!kind)
		return;
	obs_data_t *s = obs_data_create();
	if (id == "game_capture") {
		obs_data_set_string(s, "capture_mode", "any_fullscreen");
	} else if (id == "text_gdiplus") {
		obs_data_set_string(s, "text", "Your text here");
		obs_data_t *font = obs_data_create();
		obs_data_set_string(font, "face", "Segoe UI");
		obs_data_set_int(font, "size", 72);
		obs_data_set_string(font, "style", "Bold");
		obs_data_set_int(font, "flags", 1);
		obs_data_set_obj(s, "font", font);
		obs_data_release(font);
	} else if (id == "color_source") {
		obs_data_set_int(s, "width", m_core->canvas().width());
		obs_data_set_int(s, "height", m_core->canvas().height());
		obs_data_set_int(s, "color", 0xFF2A1A12); // ABGR: deep blue
	} else if (id == "ffmpeg_source") {
		obs_data_set_bool(s, "looping", true);
	}
	obs_source_t *source = m_core->addSource(kind->id, kind->name, s, kind->fit);
	obs_data_release(s);
	if (!source) {
		say(L("That source could not be added on this PC."), true);
		return;
	}
	const QString name = QString::fromUtf8(obs_source_get_name(source));
	selectSource(name);
	if (kind->ask)
		openProperties(name, true);
	else if (id == "game_capture")
		say(L("Game added. Start your game in fullscreen and it appears here by itself. Anti-cheat games "
		    "(Valorant and others) may need a Screen source instead for now."));
	if (id == "dshow_input" || id == "ffmpeg_source")
		m_core->fit(name, kind->fit); // its real size is known now
}

void MainWindow::openProperties(const QString &name, bool justAdded)
{
	obs_sceneitem_t *item = m_core->item(name);
	if (!item)
		return;
	obs_source_t *source = obs_sceneitem_get_source(item);
	PropertiesDialog dialog(source, name, this);
	if (dialog.exec() == QDialog::Rejected && justAdded) {
		m_core->removeSource(name);
		return;
	}
	m_core->save();
}

void MainWindow::openFilters(obs_source_t *source)
{
	if (!source)
		return;
	FiltersDialog dialog(source, this);
	dialog.exec();
	m_core->save();
}

void MainWindow::renameSource(const QString &name)
{
	bool ok = false;
	const QString to = QInputDialog::getText(this, L("Rename"), L("Name"), QLineEdit::Normal, name, &ok);
	if (!ok)
		return;
	if (!m_core->renameSource(name, to))
		say(L("That name is already used by a scene or source."), true);
	else
		selectSource(to.trimmed());
}

void MainWindow::sourceMenu(const QString &requested, const QPoint &global)
{
	if (requested.startsWith("options:")) { // double-click in the preview
		openProperties(requested.mid(8));
		return;
	}
	const QString name = requested;
	QMenu menu(this);
	if (name.isEmpty()) {
		fillAddMenu(&menu);
		menu.exec(global);
		return;
	}
	obs_sceneitem_t *item = m_core->item(name);
	if (!item)
		return;
	const bool locked = obs_sceneitem_locked(item), visible = obs_sceneitem_visible(item);
	obs_source_t *source = obs_sceneitem_get_source(item);
	const uint32_t flags = obs_source_get_output_flags(source);
	const bool hasVideo = flags & OBS_SOURCE_VIDEO;
	// groups: several selected sources become one; a group can be taken apart again
	QStringList picked;
	for (QListWidgetItem *row : m_sources->selectedItems())
		picked << row->data(Qt::UserRole).toString();
	if (picked.size() >= 2) {
		menu.addAction(QString(L("Group the %1 selected sources...")).arg(picked.size()), this, [this, picked] {
			bool ok = false;
			const QString group = QInputDialog::getText(this, L("Group"), L("Name"), QLineEdit::Normal, L("Group"), &ok);
			if (ok && m_core->groupItems(picked, group).isEmpty())
				say(L("Sources already in a group can't go in another one."), true);
		});
		menu.addSeparator();
	}
	if (obs_sceneitem_is_group(item)) {
		menu.addAction(L("Ungroup"), this, [this, name] { m_core->ungroup(name); });
		menu.addSeparator();
	}
	menu.addAction(L("Options..."), this, [this, name] { openProperties(name); });
	menu.addAction(L("Filters..."), this, [this, source] { openFilters(source); });
	if (flags & OBS_SOURCE_AUDIO)
		menu.addAction(L("Sound settings..."), this, [this, source] { openAudio(source); });
	menu.addAction(L("Rename..."), this, [this, name] { renameSource(name); });
	if (flags & OBS_SOURCE_CONTROLLABLE_MEDIA) {
		QMenu *media = menu.addMenu(L("Play"));
		const bool playing = obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING;
		media->addAction(playing ? L("Pause") : L("Play"), this, [source, playing] { obs_source_media_play_pause(source, playing); });
		media->addAction(L("Restart"), this, [source] { obs_source_media_restart(source); });
		media->addAction(L("Stop"), this, [source] { obs_source_media_stop(source); });
		media->addAction(L("Next"), this, [source] { obs_source_media_next(source); });
		media->addAction(L("Previous"), this, [source] { obs_source_media_previous(source); });
	}
	if (hasVideo) {
		menu.addSeparator();
		QMenu *size = menu.addMenu(L("Size and place"));
		size->setEnabled(!locked);
		size->addAction(L("Fit to screen"), this, [this, name] { m_core->fit(name, ObsCore::Fit::Fill); });
		size->addAction(L("Stretch to screen"), this, [this, name] { m_core->fit(name, ObsCore::Fit::Stretch); });
		size->addAction(L("Centre, original size"), this, [this, name] { m_core->fit(name, ObsCore::Fit::Center); });
		size->addAction(L("Bottom-right corner"), this, [this, name] { m_core->fit(name, ObsCore::Fit::Corner); });
		size->addAction(L("Left half"), this, [this, name] { m_core->fit(name, ObsCore::Fit::Left); });
		size->addAction(L("Right half"), this, [this, name] { m_core->fit(name, ObsCore::Fit::Right); });
		size->addSeparator();
		size->addAction(L("Edit size and place..."), this, [this, name] { openTransform(name); });
		size->addAction(L("Reset (original size, top-left)"), this, [this, name] { m_core->resetTransform(name); });

		QMenu *turn = menu.addMenu(L("Rotate and flip"));
		turn->setEnabled(!locked);
		turn->addAction(L("Rotate 90 degrees right  (Ctrl+R)"), this, [this, name] { m_core->rotate(name, 90); });
		turn->addAction(L("Rotate 90 degrees left"), this, [this, name] { m_core->rotate(name, -90); });
		turn->addAction(L("Rotate 180 degrees"), this, [this, name] { m_core->rotate(name, 180); });
		turn->addSeparator();
		turn->addAction(L("Flip left-right (mirror)"), this, [this, name] { m_core->flip(name, true); });
		turn->addAction(L("Flip upside down"), this, [this, name] { m_core->flip(name, false); });

		QMenu *copy = menu.addMenu(L("Copy size and place"));
		copy->addAction(L("Copy  (Ctrl+C)"), this, [this, name] { m_core->copyTransform(name); });
		QAction *paste = copy->addAction(L("Paste  (Ctrl+V)"), this, [this, name] { m_core->pasteTransform(name); });
		paste->setEnabled(m_core->hasCopiedTransform() && !locked);

		QMenu *look = menu.addMenu(L("Look"));
		QMenu *blend = look->addMenu("Blend mode");
		const std::pair<obs_blending_type, const char *> blends[] = {
			{OBS_BLEND_NORMAL, "Normal"},
			{OBS_BLEND_ADDITIVE, "Add (glow)"},
			{OBS_BLEND_SUBTRACT, "Subtract"},
			{OBS_BLEND_SCREEN, "Screen (lighten, good for light leaks)"},
			{OBS_BLEND_MULTIPLY, "Multiply (darken, good for shadows)"},
			{OBS_BLEND_LIGHTEN, "Lighten"},
			{OBS_BLEND_DARKEN, "Darken"}};
		const obs_blending_type nowBlend = obs_sceneitem_get_blending_mode(item);
		for (const auto &[type, label] : blends) {
			QAction *a = blend->addAction(L(label), this, [this, name, type = type] { m_core->setBlend(name, type); });
			a->setCheckable(true);
			a->setChecked(type == nowBlend);
		}
		QMenu *scaling = look->addMenu("Scaling");
		const std::pair<obs_scale_type, const char *> scales[] = {
			{OBS_SCALE_DISABLE, "Automatic"},
			{OBS_SCALE_POINT, "Sharp pixels (pixel art, small text)"},
			{OBS_SCALE_BILINEAR, "Bilinear (fastest)"},
			{OBS_SCALE_BICUBIC, "Bicubic"},
			{OBS_SCALE_LANCZOS, "Lanczos (sharpest)"},
			{OBS_SCALE_AREA, "Area (best when shrinking)"}};
		const obs_scale_type nowScale = obs_sceneitem_get_scale_filter(item);
		for (const auto &[type, label] : scales) {
			QAction *a = scaling->addAction(L(label), this, [this, name, type = type] { m_core->setScaleFilter(name, type); });
			a->setCheckable(true);
			a->setChecked(type == nowScale);
		}
		QMenu *effect = look->addMenu("Show/hide effect");
		const QString nowEffect = m_core->showHide(name);
		const std::pair<const char *, const char *> effects[] = {
			{"", "None"}, {"fade_transition", "Fade"}, {"slide_transition", "Slide"}, {"swipe_transition", "Swipe"}};
		for (const auto &[id, label] : effects) {
			QAction *a = effect->addAction(L(label), this, [this, name, id = QString(id)] { m_core->setShowHide(name, id, 300); });
			a->setCheckable(true);
			a->setChecked(nowEffect == id);
		}
		QMenu *extra = menu.addMenu(L("Projector and screenshot"));
		extra->addAction(L("Screenshot of this source"), this, [this, source] { screenshot(source); });
		fillProjectorMenu(extra->addMenu(L("Projector of this source")), source);
	}
	menu.addSeparator();
	menu.addAction(locked ? L("Unlock") : L("Lock in place"), this, [this, name, locked] { m_core->setLocked(name, !locked); });
	menu.addAction(visible ? L("Hide") : L("Show"), this, [this, name, visible] {
		m_core->setVisible(name, !visible);
		refreshSources();
	});
	menu.addAction(L("Move up"), this, [this, name] { m_core->moveSource(name, true); });
	menu.addAction(L("Move down"), this, [this, name] { m_core->moveSource(name, false); });
	menu.addSeparator();
	menu.addAction(L("Remove"), this, [this, name] { m_core->removeSource(name); });
	menu.exec(global);
}

// ---- outputs
void MainWindow::toggleStream()
{
	if (m_core->streaming()) {
		// the stream sends what is still buffered before it ends, which takes a few seconds: say so at once
		m_live->setEnabled(false);
		m_live->setText(L("Stopping..."));
		m_hero->setEnabled(false);
		m_core->stopStream();
		say(L("Stream ended."));
		return;
	}
	Settings &s = Settings::get();
	const QString key = s.secret("stream/key");
	const bool whip = s.value("stream/platform").toString() == "whip"; // WHIP may not need a token
	if ((key.isEmpty() && !whip) || s.value("stream/server").toString().isEmpty()) {
		say(L("Add your stream key to go live."));
		m_modes->showAgain();
		openSettings(SettingsDialog::Stream);
		return;
	}
	if (!preflightOk())
		return;
	QString error;
	m_live->setEnabled(false);
	m_live->setText(L("Connecting..."));
	m_hero->setEnabled(false);
	m_slogan->setText(L("Connecting..."));
	if (!m_core->startStream(s.value("stream/server").toString(), key, &error)) {
		say(L("Could not go live: ") + error, true);
		updateButtons();
	}
}

void MainWindow::toggleRecord()
{
	if (m_core->recording()) {
		m_rec->setEnabled(false);
		m_rec->setText(L("Saving...")); // the file is finished off before the button can say Start again
		m_core->stopRecording();
		return;
	}
	QString error;
	m_rec->setEnabled(false);
	m_rec->setText(L("Starting..."));
	if (!m_core->startRecording(Settings::get().value("record/folder", defaultFolder()).toString(), &error)) {
		say(L("Could not record: ") + error, true);
		updateButtons();
	}
}

void MainWindow::toggleReplay()
{
	if (m_core->replaying()) {
		m_core->stopReplay();
		return;
	}
	QString error;
	Settings &s = Settings::get();
	if (!m_core->startReplay(s.value("record/folder", defaultFolder()).toString(), s.value("replay/seconds", 30).toInt(),
				 &error)) {
		say(L("Could not start the replay buffer: ") + error, true);
		updateButtons();
	}
}

void MainWindow::saveReplay()
{
	if (!m_core->replaying()) {
		say(L("Turn on the Replay buffer first; then Save replay keeps the moment that just happened."));
		return;
	}
	m_core->saveReplay();
}

void MainWindow::hotkey(const QString &action)
{
	if (action == "live")
		toggleStream();
	else if (action == "record")
		toggleRecord();
	else if (action == "replay_save")
		saveReplay();
	else if (action == "replay_toggle")
		toggleReplay();
	else if (action == "mic_mute" || action == "desktop_mute") {
		const bool mic = action == "mic_mute";
		obs_source_t *source = mic ? m_core->microphone() : m_core->desktopAudio();
		if (!source)
			return;
		const bool muted = !obs_source_muted(source);
		obs_source_set_muted(source, muted);
		Settings::get().set(mic ? "audio/micMuted" : "audio/desktopMuted", muted);
		m_mixer->refresh(true);
		say(QString("%1 %2.").arg(mic ? L("Mic") : "Desktop audio", muted ? "muted" : "on"));
	} else if (action == "ptt:down" || action == "ptt:up" || action == "ptm:down" || action == "ptm:up") {
		obs_source_t *mic = m_core->microphone();
		if (!mic)
			return;
		const bool down = action.endsWith(":down"), talk = action.startsWith("ptt");
		if (down)
			m_pttMuted = obs_source_muted(mic);
		obs_source_set_muted(mic, down ? !talk : m_pttMuted); // let go: back to how it was
		m_mixer->refresh(true);
	} else if (action == "record_pause") {
		togglePause();
	} else if (action == "screenshot") {
		screenshot();
	} else if (action == "studio_go") {
		m_core->transitionNow();
	} else if (action == "vcam") {
		toggleVirtualCam();
	} else if (action == "mark") {
		m_assist->mark();
	} else if (action == "undo") {
		m_core->undo();
	} else if (action.startsWith("scene_")) {
		const int index = action.mid(6).toInt() - 1;
		const QStringList scenes = m_core->scenes();
		if (index >= 0 && index < scenes.size())
			m_core->setScene(scenes[index]);
	}
}

void MainWindow::applyRemote()
{
	QString error;
	if (!m_remote->apply(&error))
		say(L("The remote could not start: ") + error, true);
}

void MainWindow::applyHotkeys()
{
	const QStringList failed = m_hotkeys->apply();
	if (!failed.isEmpty())
		say(L("Another program already uses these hotkeys, so they won't work here: ") + failed.join(", ") +
			    L(". Pick different keys in Settings > Hotkeys."),
		    true);
}

void MainWindow::updateButtons()
{
	const bool live = m_core->streaming(), rec = m_core->recording(), replay = m_core->replaying();
	m_live->setEnabled(true);
	m_live->setText(live ? L("Stop Streaming") : L("Start Streaming"));
	m_hero->setEnabled(true);
	m_hero->setProperty("on", live);
	m_slogan->setText(live ? L("You're live.\nBe Legendary.") : L("Go Live.\nBe Legendary."));
	restyle(m_hero);
	m_rec->setEnabled(true);
	m_live->setProperty("on", live);
	m_rec->setText(rec ? L("Stop Recording") : L("Start Recording"));
	m_rec->setProperty("on", rec);
	m_replay->setChecked(replay);
	m_replay->setText(replay ? L("Replay buffer: on") : L("Replay buffer"));
	m_replay->setProperty("on", replay);
	m_saveReplay->setEnabled(replay);
	m_pause->setVisible(rec);
	m_pause->setText(m_core->recordingPaused() ? L("Resume") : L("Pause"));
	m_pause->setProperty("on", m_core->recordingPaused());
	restyle(m_pause);
	const bool vcam = m_core->virtualCam();
	m_vcam->setChecked(vcam);
	m_vcam->setText(vcam ? L("Stop Virtual Camera") : L("Start Virtual Camera"));
	m_vcam->setProperty("on", vcam);
	restyle(m_vcam);
	restyle(m_live);
	restyle(m_rec);
	restyle(m_replay);
	m_trayLive->setText(live ? L("End stream") : L("Go Live"));
	m_trayRec->setText(rec ? L("Stop recording") : L("Start recording"));
	m_trayReplay->setEnabled(replay);
	m_studio->setChecked(m_core->studioMode());
	m_studio->setProperty("on", m_core->studioMode());
	restyle(m_studio);
}

void MainWindow::openSettings(int tab)
{
	SettingsDialog dialog(m_core, m_modes, this);
	dialog.checkUpdates = [this] { checkForUpdates(); };
	dialog.openTab(SettingsDialog::Tab(tab));
	if (dialog.exec() == QDialog::Accepted) {
		say(L("Settings saved."));
		if (Settings::get().value("ui/language", "en").toString() != i18n::language() &&
		    QMessageBox::question(this, L("Language"), L("Restart Zunimi Studio now to switch the language?")) ==
			    QMessageBox::Yes) {
			restartApp();
			return;
		}
		applyHotkeys();
		applyRemote();
		updatePreview();
		refreshGoLive(true);
	}
}

// ---- live status
void MainWindow::tick()
{
	const Health h = m_core->health();
	m_assist->sample(h);
	const Finding fix = m_assist->doctor(h);
	m_fixId = fix.fix;
	m_fix->setVisible(!fix.fix.isEmpty());
	if (!fix.fix.isEmpty())
		m_fix->setText(fix.fixLabel);
	const QColor dot = h.level == 2 ? Theme::Live : h.level == 1 ? Theme::Warn : Theme::Good;
	m_healthDot->setStyleSheet(QString("background:%1; border-radius:5px;").arg(dot.name()));
	m_health->setText(h.text);
	if (QWidget *corner = menuBar()->cornerWidget(Qt::TopRightCorner); corner && corner->sizeHint() != corner->size()) {
		QResizeEvent resize(menuBar()->size(), menuBar()->size());
		QCoreApplication::sendEvent(menuBar(), &resize);
	}
	m_cpu->setText(QString("%1%").arg(h.cpu, 0, 'f', 1));
	m_cpu->setToolTip(QString(L("RAM %1 MB")).arg(h.ramMb, 0, 'f', 0));
	m_fps->setText(QString::number(h.fps, 'f', 2));
	const VideoPlan &p = m_core->plan();
	m_profile->setText(QString("%1  ·  %2p%3  ·  %4")
				   .arg(profileName(profileFromKey(Settings::get().value("video/profile").toString())))
				   .arg(p.height)
				   .arg(p.fps)
				   .arg(encoderLabel(p.encoder)));
	const QString tune = QString(L("Auto-Tune picked %1 for this PC."))
				     .arg(profileName(profileFromKey(Settings::get().value("video/profile").toString())));
	auto clock = [](const QElapsedTimer &t) {
		const qint64 secs = t.elapsed() / 1000;
		return QString("%1:%2:%3")
			.arg(secs / 3600, 2, 10, QChar('0'))
			.arg((secs / 60) % 60, 2, 10, QChar('0'))
			.arg(secs % 60, 2, 10, QChar('0'));
	};
	const bool live = m_core->streaming() && m_liveSince.isValid();
	const bool rec = m_core->recording() && m_recSince.isValid();
	m_liveTime->setText(live ? clock(m_liveSince) : "00:00:00");
	m_tune->setText(live ? QString(L("Live for %1. Click to end the stream.")).arg(clock(m_liveSince)) : tune);
	m_recTime->setText(rec ? (m_core->recordingPaused() ? L("Paused") : clock(m_recSince)) : "00:00:00");
	refreshGoLive(false);
	m_liveTime->setProperty("on", live);
	m_recTime->setProperty("on", rec);
	restyle(m_liveTime);
	restyle(m_recTime);
}

void MainWindow::updatePreview()
{
	const bool visible = isVisible() && !isMinimized();
	const bool pauseLive = Settings::get().value("preview/pauseWhileLive", false).toBool() &&
			       (m_core->streaming() || m_core->recording());
	const bool run = visible && m_previewWanted && !pauseLive;
	m_preview->setRunning(run);
	m_program->setRunning(run && m_core->studioMode());
	m_previewStack->setCurrentIndex(run || !visible ? 0 : 1);
}

void MainWindow::say(const QString &text, bool bad)
{
	m_banner->setText(text);
	m_banner->setStyleSheet(QString("background:%1; border:1px solid %2; border-radius:10px; padding:8px 12px;")
					.arg(Theme::Raised.name(), (bad ? Theme::Live : Theme::Accent).name()));
	m_banner->show();
	m_bannerTimer->start(bad ? 12000 : 6000);
}

QString MainWindow::quitWouldStop() const
{
	if (m_core->streaming())
		return L("You're live. Quitting ends the stream.");
	if (m_core->recording())
		return L("A recording is running. Quitting stops and saves it.");
	return {};
}

// ---- updates
void MainWindow::showUpdate(const QString &text, const QString &go, const QString &later)
{
	m_updateText->setText(text);
	m_updateGo->setText(go);
	m_updateGo->setEnabled(true);
	m_updateLater->setText(later);
	m_updateBar->show();
}

void MainWindow::updateClicked()
{
	if (!Updater::installed()) { // the zip build cannot replace itself
		QDesktopServices::openUrl(QUrl(m_update.page));
		m_updateBar->hide();
		return;
	}
	if (!quitWouldStop().isEmpty()) {
		say(L("Finish your stream or recording first: updating restarts Zunimi Studio."), true);
		return;
	}
	if (m_installer.isEmpty()) {
		m_updateGo->setEnabled(false);
		m_updateLater->setText(L("Cancel"));
		m_updateText->setText(QString(L("Downloading Zunimi Studio %1...")).arg(m_update.version.toHtmlEscaped()));
		m_updater->download(m_update);
		return;
	}
	if (!m_updater->launch(m_installer)) {
		showUpdate(L("The installer could not be started. Try again, or download it from the release page."),
			   L("Try again"), L("Close"));
		m_installer.clear();
		return;
	}
	m_modes->quit(false);
}

// ---- window
void MainWindow::keyPressEvent(QKeyEvent *event)
{
	if (event->key() == Qt::Key_Delete && m_preview->hasFocus() && !m_preview->selectedName().isEmpty()) {
		m_core->removeSource(m_preview->selectedName());
		return;
	}
	if (!m_modes->handleKey(event))
		QMainWindow::keyPressEvent(event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
	if (m_modes->closeEvent(event))
		return;
	m_ticker->stop();
	QMainWindow::closeEvent(event);
	QApplication::quit();
}

// ---- menus (filled each time they open)
namespace {
void openFolder(const QString &folder)
{
	QDir().mkpath(folder);
	QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}
} // namespace

void MainWindow::fillFileMenu(QMenu *menu)
{
	menu->addAction(L("Settings..."), this, [this] { openSettings(); });
	QMenu *import = menu->addMenu(L("Import"));
	import->addAction(L("From OBS Studio on this PC..."), this, &MainWindow::importFromObs);
	import->addAction(L("Overlay pack or scene collection (.json)..."), this, &MainWindow::importCollection);
	menu->addSeparator();
	menu->addAction(L("Open recordings folder"), this,
			[] { openFolder(Settings::get().value("record/folder", defaultFolder()).toString()); });
	menu->addAction(L("Open log files"), this, [] { openFolder(Settings::dataDir() + "/logs"); });
	menu->addSeparator();
	menu->addAction(L("Exit"), m_modes, [this] { m_modes->quit(); });
}

void MainWindow::fillEditMenu(QMenu *menu)
{
	QAction *undo = menu->addAction(m_core->canUndo() ? L("Undo ") + m_core->undoLabel() : L("Undo"), this,
					[this] { m_core->undo(); });
	undo->setShortcut(QKeySequence::Undo);
	undo->setEnabled(m_core->canUndo());
	QAction *redo = menu->addAction(m_core->canRedo() ? L("Redo ") + m_core->redoLabel() : L("Redo"), this,
					[this] { m_core->redo(); });
	redo->setShortcut(QKeySequence("Ctrl+Y"));
	redo->setEnabled(m_core->canRedo());
	menu->addSeparator();
	const bool picked = !selectedSource().isEmpty();
	QAction *copy = menu->addAction(L("Copy size and place"), this, [this] { m_core->copyTransform(selectedSource()); });
	copy->setShortcut(QKeySequence::Copy);
	copy->setEnabled(picked);
	QAction *paste = menu->addAction(L("Paste size and place"), this, [this] { m_core->pasteTransform(selectedSource()); });
	paste->setShortcut(QKeySequence::Paste);
	paste->setEnabled(picked);
	QAction *rotate = menu->addAction(L("Rotate 90°"), this, [this] { m_core->rotate(selectedSource(), 90); });
	rotate->setShortcut(QKeySequence("Ctrl+R"));
	rotate->setEnabled(picked);
	menu->addSeparator();
	QAction *props = menu->addAction(L("Source options..."), this, [this] { openProperties(selectedSource()); });
	props->setEnabled(picked);
}

void MainWindow::fillViewMenu(QMenu *menu)
{
	const bool easy = simple();
	QAction *mode = menu->addAction(L("Simple mode  (fewer buttons)"), this, [this](bool on) { setSimple(on); });
	mode->setCheckable(true);
	mode->setChecked(easy);
	QAction *preview = menu->addAction(L("Preview"), this, [this](bool on) {
		m_previewWanted = on;
		updatePreview();
	});
	preview->setCheckable(true);
	preview->setChecked(m_previewWanted);
	if (!easy) {
		menu->addSeparator();
		QAction *studio = menu->addAction(L("Studio mode  (edit, then Go)"), this,
						  [this] { m_core->setStudioMode(!m_core->studioMode()); });
		studio->setCheckable(true);
		studio->setChecked(m_core->studioMode());
		QAction *vertical = menu->addAction(L("Vertical canvas  (Shorts, Reels, TikTok)"), this, [this] {
			QString error;
			if (!m_core->setVertical(!m_core->verticalOn(), &error))
				say(error, true);
		});
		vertical->setCheckable(true);
		vertical->setChecked(m_core->verticalOn());
		fillProjectorMenu(menu->addMenu(L("Projector  (what's on air)")), nullptr);
		menu->addAction(L("Stats window"), this, [this] {
			auto *dialog = new StatsDialog(m_core, this);
			dialog->setAttribute(Qt::WA_DeleteOnClose);
			dialog->show();
		});
	}
	menu->addSeparator();
	fillDocksMenu(menu->addMenu(L("Docks")));
}

void MainWindow::fillToolsMenu(QMenu *menu)
{
	menu->addAction(L("Screenshot of what's on air"), this, [this] { screenshot(); });
	menu->addAction(L("Mark moment  (chapter + saved replay)"), this, [this] { m_assist->mark(); });
	menu->addAction(L("Check my setup now"), this, [this] {
		const QVector<Finding> found = m_assist->preflight(true);
		if (found.isEmpty()) {
			say(L("All good: scene, mic, disk and encoder look ready."));
			return;
		}
		QStringList lines;
		for (const Finding &f : found)
			lines << (f.level >= 2 ? "• " : "◦ ") + f.text;
		QMessageBox::information(this, L("Setup check"), lines.join("\n"));
	});
	if (!simple()) {
		QAction *vcam = menu->addAction(L("Virtual camera"), this, &MainWindow::toggleVirtualCam);
		vcam->setCheckable(true);
		vcam->setChecked(m_core->virtualCam());
		menu->addAction(L("Phone and Stream Deck remote..."), this, [this] { openSettings(SettingsDialog::Remote); });
		menu->addAction(L("Chat and alert connections..."), this, [this] { openSettings(SettingsDialog::Connections); });
	}
	menu->addSeparator();
	menu->addAction(L("Run the setup again..."), this, &MainWindow::runSetup);
}

void MainWindow::fillHelpMenu(QMenu *menu)
{
	menu->addAction(L("Check for updates"), this, &MainWindow::checkForUpdates);
	menu->addAction(L("Website and guides"), this, [] { QDesktopServices::openUrl(QUrl("https://zunimilabs.online")); });
	menu->addAction(L("Open log files"), this, [] { openFolder(Settings::dataDir() + "/logs"); });
	menu->addSeparator();
	menu->addAction(L("About"), this, [this] {
		QMessageBox::about(this, L("About"),
				   QString("<b>Zunimi Stream+ %1</b><br>%2<br><br>%3")
					   .arg(ZUNIMI_VERSION, L("Made by ZunimiLabs."),
						L("Built on OBS Studio's libobs (GPL-2.0). Not affiliated with the OBS Project.")));
	});
}

// ---- Simple / Pro and the first-time setup
bool MainWindow::simple()
{
	return Settings::get().value("ui/simple", false).toBool();
}

void MainWindow::setSimple(bool on)
{
	Settings::get().set("ui/simple", on);
	if (on && m_core->studioMode())
		m_core->setStudioMode(false);
	applyLevel();
	say(on ? L("Simple mode: just what you need to go live and record. Untick View > Simple mode to bring back "
		 "every tool.")
	       : L("Pro mode: every tool is shown."));
}

void MainWindow::applyLevel()
{
	const bool easy = simple();
	for (QWidget *w : findChildren<QWidget *>())
		if (w->property("pro").toBool())
			w->setVisible(!easy);
	// Pro-only docks hide in Simple mode and come back where they were in Pro mode
	for (QDockWidget *dock : m_docks)
		if (dock->property("proDock").toBool()) {
			if (easy) {
				if (!dock->property("hiddenBySimple").toBool() && dock->isVisible()) {
					dock->setProperty("hiddenBySimple", true);
					dock->hide();
				}
			} else if (dock->property("hiddenBySimple").toBool()) {
				dock->setProperty("hiddenBySimple", false);
				dock->show();
			}
		}
	for (QAction *a : menuBar()->actions())
		if (a->property("proMenu").toBool())
			a->setVisible(!easy);
	if (m_mixer)
		m_mixer->refresh(true);
}

void MainWindow::runSetup()
{
	m_modes->showAgain();
	SetupWizard wizard(m_core, this);
	if (wizard.exec() == QDialog::Accepted)
		applySetup(wizard.choices());
	else
		Settings::get().set("setup/done", true); // skipped: not asked again (Menu has it)
}

void MainWindow::applySetup(const SetupChoices &c)
{
	Settings &s = Settings::get();
	s.set("setup/done", true);
	s.set("ui/simple", c.simple);
	if (c.simple && m_core->studioMode())
		m_core->setStudioMode(false);
	bool channels = false;
	if (!c.platform.isEmpty()) {
		s.set("stream/platform", c.platform);
		if (!c.server.isEmpty())
			s.set("stream/server", c.server);
		if (!c.key.isEmpty())
			s.setSecret("stream/key", c.key);
		if (!c.channel.isEmpty()) {
			const char *key = c.platform == "twitch"    ? "events/twitchChannel"
					  : c.platform == "youtube" ? "events/youtube"
								    : "events/kickChannel";
			channels = s.value(key).toString() != c.channel;
			s.set(key, c.channel);
		}
	}
	// back to front: the screen, then the game over it (a game window that isn't fullscreen leaves the screen
	// showing), then the webcam in a corner, then the widgets
	if (!c.screenId.isEmpty()) {
		const auto screens = m_core->screens();
		int number = 1;
		for (int i = 0; i < screens.size(); ++i)
			if (screens[i].second == c.screenId)
				number = i + 1;
		addScreen(c.screenId, screenLabel(number, c.screenLabel));
	}
	if (c.game)
		addSource("game_capture");
	if (!c.webcamId.isEmpty()) {
		obs_data_t *cam = obs_data_create();
		obs_data_set_string(cam, "video_device_id", c.webcamId.toUtf8().constData());
		if (obs_source_t *source = m_core->addSource("dshow_input", L("Webcam"), cam, ObsCore::Fit::Corner))
			m_core->fit(QString::fromUtf8(obs_source_get_name(source)), ObsCore::Fit::Corner);
		obs_data_release(cam);
	}
	if (c.alerts)
		addOverlay("alerts", true);
	if (c.chat)
		addOverlay("chat", true);
	if (channels)
		EventHub::get().restart();
	applyLevel();
	refreshSources();
	say(c.platform.isEmpty() || !c.key.isEmpty()
		    ? L("All set. Press Go Live when you're ready, or Record to try it out first.")
		    : L("All set. Add your stream key when you press Go Live, or press Record to try it out first."));
	if (c.importObs)
		importFromObs();
}

void MainWindow::fillCollectionsMenu(QMenu *menu)
{
	const QStringList all = m_core->collections();
	for (const QString &name : all) {
		QAction *a = menu->addAction(name, this, [this, name] {
			QString error;
			if (!m_core->switchCollection(name, &error))
				say(error, true);
		});
		a->setCheckable(true);
		a->setChecked(name == m_core->collection());
	}
	menu->addSeparator();
	auto create = [this](bool copy) {
		bool ok = false;
		const QString name = QInputDialog::getText(this, copy ? L("Copy scene collection") : L("New scene collection"),
							   L("Name"), QLineEdit::Normal,
							   copy ? m_core->collection() + L(" copy") : QString(), &ok);
		QString error;
		if (ok && !m_core->newCollection(name, copy, &error))
			say(error, true);
	};
	menu->addAction(L("New (empty)..."), this, [create] { create(false); });
	menu->addAction(L("Copy this one..."), this, [create] { create(true); });
	menu->addAction(L("Rename this one..."), this, [this] {
		bool ok = false;
		const QString from = m_core->collection();
		const QString to = QInputDialog::getText(this, L("Rename scene collection"), L("Name"), QLineEdit::Normal, from, &ok);
		QString error;
		if (ok && !m_core->renameCollection(from, to, &error))
			say(error, true);
	});
	QAction *remove = menu->addAction(L("Remove this one..."), this, [this] {
		const QString name = m_core->collection();
		if (QMessageBox::question(this, L("Remove scene collection"),
					  QString(L("Remove \"%1\" and all its scenes? This can't be undone.")).arg(name),
					  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
			return;
		QString error;
		if (!m_core->removeCollection(name, &error))
			say(error, true);
	});
	remove->setEnabled(all.size() > 1);
}

void MainWindow::fillProfilesMenu(QMenu *menu)
{
	for (const QString &name : m_core->profiles()) {
		QAction *a = menu->addAction(name, this, [this, name] {
			QString error;
			if (!m_core->switchProfile(name, &error))
				say(error, true);
			else
				say(L("Profile: ") + name + L(". Its stream key, quality and recording settings are in use."));
		});
		a->setCheckable(true);
		a->setChecked(name == m_core->profile());
	}
	menu->addSeparator();
	menu->addAction(L("New profile..."), this, [this] {
		bool ok = false;
		const QString name = QInputDialog::getText(this, L("New profile"),
							   L("Name (for example \"YouTube 1080p\" or \"Practice, record only\")"),
							   QLineEdit::Normal, {}, &ok);
		QString error;
		if (ok && !m_core->newProfile(name, &error))
			say(error, true);
	});
	QAction *remove = menu->addAction(L("Remove this profile"), this, [this] {
		QString error;
		if (!m_core->removeProfile(m_core->profile(), &error))
			say(error, true);
	});
	remove->setEnabled(m_core->profile() != "Default");
}

void MainWindow::fillProjectorMenu(QMenu *menu, obs_source_t *source)
{
	const QList<QScreen *> screens = QGuiApplication::screens();
	for (int i = 0; i < screens.size(); i++) {
		const QRect g = screens[i]->geometry();
		menu->addAction(QString(L("Fullscreen on screen %1  (%2×%3)")).arg(i + 1).arg(g.width()).arg(g.height()), this,
				[this, source, i] { openProjector(source, i); });
	}
	menu->addAction(L("In a window"), this, [this, source] { openProjector(source, -1); });
}

void MainWindow::openProjector(obs_source_t *source, int screen)
{
	auto *projector = new ProjectorWindow(m_core, source, screen);
	projector->setWindowIcon(windowIcon());
	if (screen >= 0)
		say(L("Projector open. Press Esc or double-click it to close."));
}

void MainWindow::screenshot(obs_source_t *source)
{
	const QString folder = Settings::get().value("record/folder", defaultFolder()).toString() + "/Screenshots";
	m_core->screenshot(folder, source);
}

// ---- scenes
void MainWindow::sceneMenu(const QPoint &global)
{
	QListWidgetItem *row = m_scenes->currentItem();
	if (!row)
		return;
	const QString name = row->data(Qt::UserRole).toString();
	QMenu menu(this);
	menu.addAction(L("Rename..."), this, [this] { renameScene(); });
	menu.addAction(L("Copy"), this, [this, name] { m_core->duplicateScene(name); });
	QMenu *own = menu.addMenu(L("Transition into this scene"));
	const auto current = m_core->sceneTransition(name);
	QAction *def = own->addAction(L("Use the default"), this, [this, name] { m_core->setSceneTransition(name, {}, 0); });
	def->setCheckable(true);
	def->setChecked(current.first.isEmpty());
	for (const auto &[id, label] : ObsCore::transitionKinds()) {
		QAction *a = own->addAction(label, this, [this, name, id = id] {
			bool ok = true;
			int ms = m_core->transitionMs();
			if (id != "cut_transition" && id != "obs_stinger_transition")
				ms = QInputDialog::getInt(this, L("Transition length"), L("Milliseconds"), std::max(ms, 300), 50, 20000,
							  50, &ok);
			if (ok)
				m_core->setSceneTransition(name, id, ms);
			refreshScenes();
		});
		a->setCheckable(true);
		a->setChecked(current.first == id);
	}
	obs_source_t *src = obs_get_source_by_name(name.toUtf8().constData());
	fillProjectorMenu(menu.addMenu(L("Projector of this scene")), src);
	menu.addAction(L("Screenshot of this scene"), this, [this, src] { screenshot(src); });
	menu.addSeparator();
	menu.addAction(L("Move up"), this, [this, name] { m_core->moveScene(name, true); });
	menu.addAction(L("Move down"), this, [this, name] { m_core->moveScene(name, false); });
	menu.addSeparator();
	QAction *remove = menu.addAction(L("Remove"), this, [this, name] {
		if (QMessageBox::question(this, L("Remove scene"),
					  QString(L("Remove the scene \"%1\"? Sources that only this scene uses go too. "
						  "Ctrl+Z brings it back."))
						  .arg(name)) == QMessageBox::Yes)
			m_core->removeScene(name);
	});
	remove->setEnabled(m_core->scenes().size() > 1);
	menu.exec(global);
	obs_source_release(src);
}

void MainWindow::chooseTransition()
{
	// The stinger plays your own video over the cut; the scene switches at the moment you pick.
	Settings &st = Settings::get();
	QDialog dialog(this);
	dialog.setWindowTitle(L("Stinger transition"));
	auto *path = new QLineEdit(st.value("scenes/stingerPath").toString());
	auto *browse = new QPushButton(L("Choose..."));
	connect(browse, &QPushButton::clicked, &dialog, [&dialog, path] {
		const QString file = QFileDialog::getOpenFileName(&dialog, L("Stinger video"), path->text(),
								  L("Videos (*.webm *.mov *.mp4 *.mkv *.gif)"));
		if (!file.isEmpty())
			path->setText(file);
	});
	auto *row = new QHBoxLayout;
	row->addWidget(path, 1);
	row->addWidget(browse);
	auto *point = new QSpinBox;
	point->setRange(0, 20000);
	point->setSingleStep(50);
	point->setSuffix(" ms");
	point->setValue(st.value("scenes/stingerPoint", 500).toInt());
	auto *form = new QFormLayout;
	form->addRow(L("Video"), row);
	form->addRow(L("Switch scenes at"), point);
	auto *note = new QLabel(L("Use a video with a see-through background (.webm or .mov) so the scenes show around "
				"it. Pick the moment the video covers the whole screen."));
	note->setObjectName("hint");
	note->setWordWrap(true);
	form->addRow("", note);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	auto *layout = new QVBoxLayout(&dialog);
	layout->setContentsMargins(18, 18, 18, 18);
	layout->addLayout(form);
	layout->addWidget(buttons);
	dialog.resize(560, 0);
	if (dialog.exec() != QDialog::Accepted || path->text().trimmed().isEmpty()) {
		m_transition->setCurrentIndex(std::max(0, m_transition->findData(m_core->transitionId())));
		return;
	}
	st.set("scenes/stingerPath", path->text().trimmed());
	st.set("scenes/stingerPoint", point->value());
	m_core->setTransition("obs_stinger_transition", m_transitionMs->value());
	say(L("Stinger set. It plays every time you switch scenes."));
}

void MainWindow::updateStudio()
{
	const bool studio = m_core->studioMode();
	m_programBox->setVisible(studio);
	m_previewTitle->setVisible(studio);
	m_preview->setSource(studio ? m_core->editSceneSource() : nullptr);
	refreshScenes();
	updatePreview();
}

void MainWindow::importCollection()
{
	const QString file = QFileDialog::getOpenFileName(
		this, L("Import a scene collection"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
		L("Scene collection (*.json)"));
	if (file.isEmpty())
		return;
	QString folder;
	if (QMessageBox::question(this, L("Import"),
				  L("Is the pack's pictures and videos folder somewhere else than next to this file? "
				  "(Pick No if you unzipped the whole pack in one place.)"),
				  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
		folder = QFileDialog::getExistingDirectory(this, L("The pack's folder"), QFileInfo(file).absolutePath());
	QStringList notes;
	QString error;
	const QString name = m_core->importCollection(file, folder, &notes, &error);
	if (name.isEmpty()) {
		say(error, true);
		return;
	}
	QMessageBox::information(this, L("Imported \"") + name + "\"", notes.join("\n\n"));
}

void MainWindow::importFromObs()
{
	// OBS keeps each scene collection as a .json in %APPDATA%\obs-studio\basic\scenes.
	const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/obs-studio/basic/scenes";
	const QFileInfoList files = QDir(dir).entryInfoList({"*.json"}, QDir::Files, QDir::Time);
	if (files.isEmpty()) {
		QMessageBox::information(this, L("Import from OBS Studio"),
					 L("No OBS Studio scene collections were found on this PC. If you have a .json file "
					 "from OBS, use Import > Overlay pack or scene collection."));
		return;
	}
	QStringList names;
	for (const QFileInfo &f : files)
		names << f.completeBaseName();
	bool ok = false;
	const QString pick = QInputDialog::getItem(this, L("Import from OBS Studio"),
						   L("Which scene collection? (OBS itself is not changed.)"), names, 0, false, &ok);
	if (!ok)
		return;
	QStringList notes;
	QString error;
	const QString name = m_core->importCollection(dir + "/" + pick + ".json", {}, &notes, &error);
	if (name.isEmpty()) {
		say(error, true);
		return;
	}
	QMessageBox::information(this, L("Imported \"") + name + "\"", notes.join("\n\n"));
}

// ---- dialogs
void MainWindow::openTransform(const QString &name)
{
	if (!m_core->item(name))
		return;
	TransformDialog dialog(m_core, name, this);
	dialog.exec();
}

void MainWindow::openAudio(obs_source_t *source)
{
	if (!source)
		return;
	AudioDialog dialog(m_core, source, this);
	dialog.exec();
}

// ---- outputs
void MainWindow::togglePause()
{
	if (!m_core->recording()) {
		say(L("Pause works while recording."));
		return;
	}
	if (!m_core->pauseRecording(!m_core->recordingPaused()))
		say(L("This recording can't be paused. MP4 and MKV recordings can."), true);
}

void MainWindow::toggleVirtualCam()
{
	if (m_core->virtualCam()) {
		m_core->stopVirtualCam();
		return;
	}
	if (!ObsCore::virtualCamInstalled() &&
	    QMessageBox::question(this, L("Virtual camera"),
				  L("The first time, Windows asks for permission to add the virtual camera (one Yes, then "
				  "it stays). Continue?")) != QMessageBox::Yes) {
		updateButtons();
		return;
	}
	QString error;
	if (!m_core->startVirtualCam(&error))
		say(L("Virtual camera: ") + error, true);
	updateButtons();
}

void MainWindow::addOverlay(const QString &kind, bool quiet)
{
	// sizes and places that suit each widget on the canvas; everything can be moved like any source
	const QSize canvas = m_core->canvas();
	const double w = canvas.width(), h = canvas.height();
	struct Plan {
		const char *name;
		int width, height;
		QRectF box;
	};
	const QHash<QString, Plan> plans = {
		{"alerts", {"Alerts", 800, 450, QRectF(w * 0.25, h * 0.05, w * 0.5, h * 0.5)}},
		{"chat", {"Chat", 420, 640, QRectF(w * 0.01, h * 0.30, w * 0.24, h * 0.66)}},
		{"goal", {"Goal", 600, 90, QRectF(w * 0.30, h * 0.90, w * 0.40, h * 0.08)}},
		{"label", {"Latest follower", 600, 60, QRectF(w * 0.01, h * 0.02, w * 0.35, h * 0.05)}},
		{"timer", {"Timer", 600, 220, QRectF(w * 0.30, h * 0.35, w * 0.40, h * 0.30)}},
		{"events", {"Recent events", 420, 300, QRectF(w * 0.75, h * 0.02, w * 0.24, h * 0.30)}}};
	const Plan p = plans.value(kind);
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "kind", kind.toUtf8().constData());
	obs_data_set_int(s, "width", p.width);
	obs_data_set_int(s, "height", p.height);
	if (kind == "alerts")
		obs_data_set_string(s, "design", QJsonDocument(overlays::defaultDesign()).toJson(QJsonDocument::Compact).constData());
	if (kind == "label")
		obs_data_set_int(s, "font_size", 34);
	if (kind == "timer")
		obs_data_set_string(s, "title", "Starting soon");
	obs_source_t *source = m_core->addSourceAt("zunimi_overlay", p.name, s, p.box);
	obs_data_release(s);
	if (!source) {
		say(L("That widget could not be added."), true);
		return;
	}
	const QString name = QString::fromUtf8(obs_source_get_name(source));
	selectSource(name);
	if (quiet)
		return;
	if (kind == "alerts") {
		say(L("Alert box added. Design it here, then press Test to see it on stream."));
		overlays::Engine::get().openDesigner(source);
	} else if (kind == "chat" || kind == "goal" || kind == "label" || kind == "events") {
		const QStringList services = EventHub::get().services();
		if (std::none_of(services.begin(), services.end(), [](const QString &k) { return EventHub::get().connected(k); }))
			say(L("Added. Connect your channels in Settings > Connections so it fills in."));
		openProperties(name);
	} else {
		openProperties(name);
	}
}

bool MainWindow::preflightOk()
{
	// Pre-flight: a last look before going live. Fixes are one click; "Go live anyway" always works.
	if (!Settings::get().value("preflight/on", true).toBool())
		return true;
	const QVector<Finding> found = m_assist->preflight(true);
	if (found.isEmpty())
		return true;
	QDialog dialog(this);
	dialog.setWindowTitle(L("Before you go live"));
	auto *layout = new QVBoxLayout(&dialog);
	layout->setContentsMargins(18, 18, 18, 18);
	layout->setSpacing(10);
	for (const Finding &f : found) {
		auto *row = new QHBoxLayout;
		auto *text = new QLabel((f.level >= 2 ? L("<b style='color:#FF4D6D'>●</b>  ") : L("<b style='color:#FFB020'>●</b>  ")) +
					f.text.toHtmlEscaped());
		text->setWordWrap(true);
		row->addWidget(text, 1);
		if (!f.fix.isEmpty()) {
			auto *fixButton = new QPushButton(f.fixLabel);
			connect(fixButton, &QPushButton::clicked, &dialog, [this, fixButton, id = f.fix] {
				m_assist->applyFix(id);
				fixButton->setEnabled(false);
				fixButton->setText(L("Fixed"));
			});
			row->addWidget(fixButton);
		}
		layout->addLayout(row);
	}
	auto *never = new QCheckBox(L("Don't check before going live"));
	layout->addWidget(never);
	auto *buttons = new QDialogButtonBox;
	QPushButton *go = buttons->addButton(L("Go live anyway"), QDialogButtonBox::AcceptRole);
	go->setObjectName("primary");
	buttons->addButton(QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	layout->addWidget(buttons);
	dialog.resize(560, 0);
	const bool ok = dialog.exec() == QDialog::Accepted;
	if (never->isChecked())
		Settings::get().set("preflight/on", false);
	return ok;
}

void MainWindow::restartApp()
{
	QProcess::startDetached(QCoreApplication::applicationFilePath(), {"--restarted"});
	m_modes->quit(false);
}

bool MainWindow::ensureBrowser()
{
	// The browser source comes with Zunimi Studio but starts only once it's used (Chromium takes memory).
	if (m_core->moduleLoaded("obs-browser"))
		return true;
	if (!Addons::browserInstalled()) {
		say(L("The browser source is missing from this copy of Zunimi Studio. Installing the latest version brings it back."), true);
		return false;
	}
	Addons::setBrowserEnabled(true);
	if (QMessageBox::question(this, L("Browser source"),
				  L("Web pages and StreamElements/Streamlabs overlays use the browser source. It starts the first "
				  "time you use it, so Zunimi Studio needs a quick restart. "
				  "Restart now?")) != QMessageBox::Yes)
		return false;
	if (!quitWouldStop().isEmpty()) {
		say(L("Finish your stream or recording first: starting the browser source needs a restart."), true);
		return false;
	}
	restartApp();
	return false;
}

void MainWindow::addBrowser(bool overlayLink)
{
	if (!ensureBrowser())
		return;
	bool ok = false;
	const QString url = QInputDialog::getText(
		this, overlayLink ? L("Overlay or widget link") : L("Web page"),
		overlayLink ? L("Paste the overlay or widget link from StreamElements or Streamlabs\n"
			      "(StreamElements: Overlays > your overlay > Copy URL. Streamlabs: the widget's URL):")
			    : L("Address of the page:"),
		QLineEdit::Normal, {}, &ok).trimmed();
	if (!ok || url.isEmpty())
		return;
	if (!url.startsWith("https://") && !url.startsWith("http://") && !url.startsWith("file:///")) {
		say(L("That doesn't look like a web address (it should start with https://)."), true);
		return;
	}
	const QSize canvas = m_core->canvas();
	obs_data_t *s = obs_data_create();
	if (url.startsWith("file:///")) { // a page on this PC (an HTML overlay from a pack)
		obs_data_set_bool(s, "is_local_file", true);
		obs_data_set_string(s, "local_file", QUrl(url).toLocalFile().toUtf8().constData());
	} else {
		obs_data_set_string(s, "url", url.toUtf8().constData());
	}
	obs_data_set_int(s, "width", overlayLink ? 1920 : 1280);
	obs_data_set_int(s, "height", overlayLink ? 1080 : 720);
	obs_data_set_bool(s, "shutdown", true);        // a hidden browser source stops using CPU
	obs_data_set_bool(s, "reroute_audio", true);   // its sound (alert sounds) goes through the mixer
	QString name = overlayLink ? (url.contains("streamelements") ? L("StreamElements overlay") : url.contains("streamlabs") ? L("Streamlabs widget") : L("Overlay"))
				   : QUrl(url).host();
	obs_source_t *source = overlayLink ? m_core->addSourceAt("browser_source", name, s, QRectF(0, 0, canvas.width(), canvas.height()))
					   : m_core->addSource("browser_source", name.isEmpty() ? L("Web page") : name, s, ObsCore::Fit::Fill);
	obs_data_release(s);
	if (!source) {
		say(L("The browser source could not be added."), true);
		return;
	}
	selectSource(QString::fromUtf8(obs_source_get_name(source)));
	say(overlayLink ? L("Overlay added over the whole screen. Its alerts and widgets appear when they're triggered.")
			: L("Page added. Right-click it for Options (size, custom CSS, refresh)."));
}

void MainWindow::updateCanvasSwitch()
{
	m_canvasSwitch->setVisible(m_core->verticalOn());
	m_landscapeButton->setChecked(!m_core->editingVertical());
	m_verticalButton->setChecked(m_core->editingVertical());
	for (QPushButton *b : {m_landscapeButton, m_verticalButton}) {
		b->setProperty("on", b->isChecked());
		restyle(b);
	}
}

void MainWindow::verticalSettings()
{
	Settings &st = Settings::get();
	QDialog dialog(this);
	dialog.setWindowTitle(L("Vertical canvas"));
	auto *size = new QComboBox;
	size->addItem(L("1080 x 1920 (sharp)"), "1080");
	size->addItem(L("720 x 1280 (lighter)"), "720");
	size->setCurrentIndex(m_core->verticalSize().width() == 720 ? 1 : 0);
	auto *record = new QCheckBox(L("Record it too, next to every recording"));
	record->setChecked(st.value("vertical/record", true).toBool());
	auto *where = new QLabel(L("Where it goes live: add a Vertical 9:16 destination in the Go Live to panel "
				 "(YouTube Shorts, TikTok, Instagram...). It can go to several at once."));
	where->setObjectName("hint");
	where->setWordWrap(true);
	auto *bitrate = new QSpinBox;
	bitrate->setRange(1000, 20000);
	bitrate->setSingleStep(500);
	bitrate->setSuffix(" kbps");
	bitrate->setValue(st.value("vertical/bitrate", std::max(2500, m_core->plan().bitrate * 3 / 4)).toInt());
	auto *form = new QFormLayout;
	form->addRow(L("Size"), size);
	form->addRow("", record);
	form->addRow("", where);
	form->addRow(L("Bitrate"), bitrate);
	auto *note = new QLabel(L("The vertical picture is encoded separately, so it costs a second encode (easy for a "
				"graphics card encoder, heavy for x264). YouTube takes vertical live streams as Shorts."));
	note->setObjectName("hint");
	note->setWordWrap(true);
	form->addRow("", note);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	auto *layout = new QVBoxLayout(&dialog);
	layout->setContentsMargins(18, 18, 18, 18);
	layout->addLayout(form);
	layout->addWidget(buttons);
	dialog.resize(560, 0);
	if (dialog.exec() != QDialog::Accepted)
		return;
	const bool resized = size->currentData().toString() != st.value("vertical/size").toString() && st.has("vertical/size");
	st.set("vertical/size", size->currentData());
	st.set("vertical/record", record->isChecked());
	st.set("vertical/bitrate", bitrate->value());
	m_core->outputSettingsChanged();
	if (resized)
		say(L("The new vertical size applies after a restart of Zunimi Studio."));
}
