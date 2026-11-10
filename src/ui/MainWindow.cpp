#include "ui/MainWindow.h"

#include "core/Addons.h"
#include "core/Assist.h"
#include "core/ObsCore.h"
#include "core/Settings.h"
#include "events/Events.h"
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
#include "ui/Theme.h"
#include "ui/WindowModes.h"
#include "version.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QJsonDocument>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
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
	return QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + "/Zunimi Studio";
}

// "KA222Q H: 1920x1080 @ 0,0 (Primary Monitor)" -> "Screen 1: KA222Q H (main)"; a screen with no name is
// almost always a laptop's own panel.
QString screenLabel(int number, const QString &raw)
{
	QString name = raw.section(':', 0, 0).trimmed();
	if (name.isEmpty())
		name = "Built-in display";
	const auto size = QRegularExpression("(\\d+)x(\\d+)").match(raw);
	QString label = QString("Screen %1: %2").arg(number).arg(name);
	if (size.hasMatch())
		label += QString("  %1×%2").arg(size.captured(1), size.captured(2));
	if (raw.contains("Primary"))
		label += "  (main)";
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
	m_assist = new Assist(m_core, this);
	m_modes->busy = [this] { return quitWouldStop(); };

	auto *central = new QWidget;
	auto *root = new QVBoxLayout(central);
	root->setContentsMargins(14, 10, 14, 12);
	root->setSpacing(10);
	root->addWidget(buildTopBar());
	auto *body = new QHBoxLayout;
	body->setSpacing(10);
	auto *left = new QVBoxLayout;
	left->setSpacing(10);
	left->addWidget(buildScenes());
	left->addWidget(buildSources(), 1);
	body->addLayout(left);
	body->addWidget(buildPreview(), 1);
	m_mixer = new Mixer(m_core);
	auto *chat = new ChatPanel;
	auto *events = new EventsPanel;
	connect(chat, &ChatPanel::setupRequested, this, [this] { openSettings(SettingsDialog::Connections); });
	connect(events, &EventsPanel::setupRequested, this, [this] { openSettings(SettingsDialog::Connections); });
	auto *side = new QTabWidget;
	side->setObjectName("side");
	side->addTab(m_mixer, "Audio");
	side->addTab(chat, "Chat");
	side->addTab(events, "Events");
	side->setCurrentIndex(Settings::get().value("window/sideTab", 0).toInt());
	connect(side, &QTabWidget::currentChanged, this, [](int i) { Settings::get().set("window/sideTab", i); });
	auto *mixerLayout = new QVBoxLayout;
	mixerLayout->setContentsMargins(0, 0, 0, 0);
	mixerLayout->addWidget(side);
	QWidget *mixerPanel = panel(mixerLayout);
	mixerPanel->setFixedWidth(300);
	body->addWidget(mixerPanel);
	root->addLayout(body, 1);
	m_banner = new QLabel;
	m_banner->setWordWrap(true);
	m_banner->hide();
	root->addWidget(m_banner);
	root->addWidget(buildUpdateBar());
	root->addWidget(buildBottomBar());
	setCentralWidget(central);
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
			say("You're live!");
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
	connect(m_core, &ObsCore::recordingChanged, this, [this](bool on, const QString &file) {
		if (!on && QFileInfo::exists(file))
			say("Recording saved: " + QFileInfo(file).fileName());
		updateButtons();
		updatePreview();
	});
	connect(m_core, &ObsCore::replayChanged, this, [this](bool on) {
		if (on)
			say(QString("Replay buffer on: Save replay keeps the last %1 seconds.")
				    .arg(Settings::get().value("replay/seconds", 30).toInt()));
		updateButtons();
	});
	connect(m_core, &ObsCore::replaySaved, this, [this](const QString &file) {
		say("Replay saved: " + QFileInfo(file).fileName());
	});
	connect(m_core, &ObsCore::problem, this, [this](const QString &text) { say(text, true); });
	connect(m_core, &ObsCore::notice, this, [this](const QString &text) { say(text); });
	connect(m_core, &ObsCore::sceneChanged, this, &MainWindow::refreshSources);
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
		dialog->setWindowTitle("How your stream went");
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
		say(paused ? "Recording paused. Nothing is written until you resume." : "Recording resumed.");
		updateButtons();
	});
	connect(m_core, &ObsCore::virtualCamChanged, this, [this](bool on) {
		if (on)
			say("Virtual camera on: pick \"OBS Virtual Camera\" as the camera in Discord, Zoom, Meet or any app.");
		updateButtons();
	});
	connect(m_core, &ObsCore::screenshotSaved, this,
		[this](const QString &file) { say("Screenshot saved: " + QFileInfo(file).fileName()); });
	connect(m_core, &ObsCore::studioModeChanged, this, [this] { updateStudio(); });
	connect(m_core, &ObsCore::verticalChanged, this, [this](bool on) {
		updateCanvasSwitch();
		if (on)
			say("Vertical canvas on. Switch to \"Vertical 9:16\" above the preview to arrange it; it follows the "
			    "scene that's on air, cropped to the middle.");
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
		say("Scene collection: " + m_core->collection());
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
			say("Size and place copied. Select another source and press Ctrl+V.");
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
	if (!s.value("welcome/shown", false).toBool()) {
		const VideoPlan &p = m_core->plan();
		say(QString("Auto-Tune set this PC to %1: %2p %3 fps with %4. Add a Game or Screen source to begin.")
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
		showUpdate(QString("<b>Zunimi Studio %1 is out.</b> %2").arg(info.version.toHtmlEscaped(),
									     info.notes.toHtmlEscaped()),
			   Updater::installed() ? "Update now" : "Download page", "Later");
	});
	connect(m_updater, &Updater::nothing, this,
		[this] { say(QString("You have the newest version (%1).").arg(ZUNIMI_VERSION)); });
	connect(m_updater, &Updater::progress, this, [this](double f) {
		m_updateText->setText(QString("Downloading Zunimi Studio %1... %2%")
					      .arg(m_update.version.toHtmlEscaped())
					      .arg(int(f * 100)));
	});
	connect(m_updater, &Updater::ready, this, [this](const QString &path) {
		m_installer = path;
		showUpdate(QString("<b>Zunimi Studio %1 is ready to install.</b> It takes a few seconds and opens "
				   "again by itself.")
				   .arg(m_update.version.toHtmlEscaped()),
			   "Restart and update", "Later");
	});
	connect(m_updater, &Updater::failed, this, [this](const QString &reason) {
		if (m_update.version.isEmpty())
			say(reason, true);
		else
			showUpdate(reason.toHtmlEscaped(), "Try again", "Close");
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
QWidget *MainWindow::buildTopBar()
{
	auto *bar = new QWidget;
	bar->setObjectName("topbar");
	auto *row = new QHBoxLayout(bar);
	row->setContentsMargins(4, 0, 4, 0);
	row->setSpacing(10);
	auto *logo = new QLabel;
	logo->setPixmap(QPixmap(":/logo-64.png").scaled(28, 28, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	auto *brand = new QLabel("ZUNIMI STUDIO");
	brand->setObjectName("brand");
	m_healthDot = new QLabel;
	m_healthDot->setFixedSize(10, 10);
	m_health = new QLabel("Ready");
	m_stats = new QLabel;
	m_stats->setObjectName("chip");
	m_profile = new QLabel;
	m_profile->setObjectName("chip");
	auto *more = button("Menu", "", "Undo, scene collections, profiles, import, studio mode, projectors, virtual camera, stats");
	auto *mainMenu = new QMenu(more);
	connect(mainMenu, &QMenu::aboutToShow, this, [this, mainMenu] { fillMainMenu(mainMenu); });
	more->setMenu(mainMenu);
	auto *settings = button("Settings", "", "Stream key, quality, recording, replay, hotkeys, window");
	connect(settings, &QPushButton::clicked, this, [this] { openSettings(); });
	row->addWidget(logo);
	row->addWidget(brand);
	row->addSpacing(16);
	row->addWidget(m_healthDot);
	row->addWidget(m_health);
	m_fix = button("Fix it", "primary", "Stream Doctor: one click to fix what the health bar shows");
	m_fix->hide();
	connect(m_fix, &QPushButton::clicked, this, [this] {
		m_assist->applyFix(m_fixId);
		m_fix->hide();
	});
	row->addWidget(m_fix);
	row->addStretch(1);
	row->addWidget(m_stats);
	row->addWidget(m_profile);
	row->addWidget(more);
	row->addWidget(settings);
	return bar;
}

QWidget *MainWindow::buildScenes()
{
	auto *col = new QVBoxLayout;
	col->setContentsMargins(12, 12, 12, 12);
	col->setSpacing(8);
	auto *title = new QLabel("Scenes");
	title->setObjectName("h2");
	m_scenes = new QListWidget;
	m_scenes->setToolTip("Click to switch. Double-click to rename. Hotkeys for scenes 1-9 are in Settings > Hotkeys.");
	m_scenes->setFixedHeight(128);
	auto *add = button("+", "icon", "New scene");
	auto *copy = button("Copy", "icon", "Duplicate this scene (sources are shared)");
	auto *rename = button("Rename", "icon");
	auto *remove = button("✕", "icon", "Remove this scene");
	auto *up = button("↑", "icon", "Move up");
	auto *down = button("↓", "icon", "Move down");
	auto *tools = new QHBoxLayout;
	tools->setSpacing(4);
	for (QPushButton *b : {add, copy, rename, remove})
		tools->addWidget(b);
	tools->addStretch();
	tools->addWidget(up);
	tools->addWidget(down);

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
	auto *transition = new QHBoxLayout;
	auto *tlabel = new QLabel("Transition");
	tlabel->setObjectName("mute");
	transition->addWidget(tlabel);
	transition->addWidget(m_transition, 1);
	transition->addWidget(m_transitionMs);

	connect(m_scenes, &QListWidget::currentRowChanged, this, [this](int row) {
		if (row >= 0)
			m_core->setScene(m_scenes->item(row)->data(Qt::UserRole).toString());
	});
	m_scenes->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(m_scenes, &QListWidget::customContextMenuRequested, this,
		[this](const QPoint &pos) { sceneMenu(m_scenes->viewport()->mapToGlobal(pos)); });
	connect(m_scenes, &QListWidget::itemDoubleClicked, this, [this] { renameScene(); });
	connect(add, &QPushButton::clicked, this, [this] {
		bool ok = false;
		const QString name = QInputDialog::getText(this, "New scene", "Name", QLineEdit::Normal,
							   QString("Scene %1").arg(m_core->scenes().size() + 1), &ok);
		if (ok)
			m_core->addScene(name);
	});
	connect(copy, &QPushButton::clicked, this, [this] { m_core->duplicateScene(m_core->currentScene()); });
	connect(rename, &QPushButton::clicked, this, [this] { renameScene(); });
	connect(remove, &QPushButton::clicked, this, [this] {
		const QString name = m_core->currentScene();
		if (m_core->scenes().size() <= 1) {
			say("A scene is always kept. Add another before removing this one.");
			return;
		}
		if (QMessageBox::question(this, "Remove scene",
					  QString("Remove the scene \"%1\"? Sources that only this scene uses go too.")
						  .arg(name)) == QMessageBox::Yes)
			m_core->removeScene(name);
	});
	connect(up, &QPushButton::clicked, this, [this] { m_core->moveScene(m_core->currentScene(), true); });
	connect(down, &QPushButton::clicked, this, [this] { m_core->moveScene(m_core->currentScene(), false); });

	col->addWidget(title);
	col->addWidget(m_scenes);
	col->addLayout(tools);
	col->addLayout(transition);
	QWidget *w = panel(col);
	w->setFixedWidth(310);
	return w;
}

QWidget *MainWindow::buildSources()
{
	auto *col = new QVBoxLayout;
	col->setContentsMargins(12, 12, 12, 12);
	col->setSpacing(8);
	auto *title = new QLabel("Sources");
	title->setObjectName("h2");
	m_sources = new QListWidget;
	m_sources->setToolTip("Tick to show or hide. Double-click for its options. Right-click for more.");
	m_sources->setContextMenuPolicy(Qt::CustomContextMenu);
	m_empty = new QLabel("Nothing here yet.\nAdd a Game or your Screen to start.");
	m_empty->setObjectName("mute");
	m_empty->setAlignment(Qt::AlignCenter);
	auto *add = button("+  Add source", "primary");
	auto *menu = new QMenu(add);
	connect(menu, &QMenu::aboutToShow, this, [this, menu] { fillAddMenu(menu); });
	add->setMenu(menu);
	auto *up = button("↑", "icon", "Move up (in front)");
	auto *down = button("↓", "icon", "Move down (behind)");
	auto *props = button("Options", "icon", "Everything this source can do");
	auto *filters = button("Filters", "icon", "Green screen, colour, crop, sharpen...");
	auto *remove = button("✕", "icon", "Remove the selected source (Delete)");
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

	col->addWidget(title);
	col->addWidget(m_sources, 1);
	col->addWidget(m_empty, 1);
	col->addLayout(tools);
	col->addWidget(add);
	QWidget *w = panel(col);
	w->setFixedWidth(310);
	return w;
}

void MainWindow::fillAddMenu(QMenu *menu)
{
	menu->clear();
	auto offer = [this, menu](const Kind &k) {
		if (obs_get_source_output_flags(k.id) != 0) // a module that is not loaded is not offered
			menu->addAction(k.label, this, [this, id = QString(k.id)] { addSource(id); });
	};
	offer(KINDS[0]); // game
	// every screen by name, so a second or third monitor is one click away
	const auto screens = m_core->screens();
	if (screens.size() == 1) {
		menu->addAction(screenLabel(1, screens[0].first), this,
				[this, s = screens[0]] { addScreen(s.second, screenLabel(1, s.first)); });
	} else if (!screens.isEmpty()) {
		QMenu *sub = menu->addMenu(QString("Screen  (%1 found)").arg(screens.size()));
		for (int i = 0; i < screens.size(); i++)
			sub->addAction(screenLabel(i + 1, screens[i].first), this,
				       [this, s = screens[i], i] { addScreen(s.second, screenLabel(i + 1, s.first)); });
		sub->addSeparator();
		sub->addAction("All screens, side by side", this, &MainWindow::addAllScreens);
	}
	for (int i = 1; i < int(std::size(KINDS)); i++) {
		if (i == 3 || i == 9)
			menu->addSeparator(); // sound sources, then extra devices
		offer(KINDS[i]);
	}
	menu->addSeparator();
	QMenu *widgets = menu->addMenu("Alerts and widgets  (no browser needed)");
	widgets->addAction("Alert box  (follows, subs, tips, raids...)", this, [this] { addOverlay("alerts"); });
	widgets->addAction("Chat box  (Twitch, YouTube, Kick together)", this, [this] { addOverlay("chat"); });
	widgets->addAction("Goal bar", this, [this] { addOverlay("goal"); });
	widgets->addAction("Label  (latest follower, top tipper...)", this, [this] { addOverlay("label"); });
	widgets->addAction("Timer  (countdown, subathon, clock)", this, [this] { addOverlay("timer"); });
	widgets->addAction("Recent events list", this, [this] { addOverlay("events"); });
	widgets->addSeparator();
	widgets->addAction("StreamElements / Streamlabs overlay or widget link...", this, [this] { addBrowser(true); });
	menu->addAction("Web page  (browser source)", this, [this] { addBrowser(false); });
	const QStringList nestable = m_core->nestableScenes();
	if (!nestable.isEmpty()) {
		menu->addSeparator();
		QMenu *sub = menu->addMenu("Another scene  (inside this one)");
		for (const QString &name : nestable)
			sub->addAction(name, this, [this, name] {
				m_core->addExisting(name);
				selectSource(name);
			});
	}
	const QStringList existing = m_core->otherScenesSources();
	if (!existing.isEmpty()) {
		menu->addSeparator();
		QMenu *sub = menu->addMenu("From another scene");
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
	auto *note = new QLabel("Preview paused to give your PC more power.\nYour stream and recording are not affected.");
	note->setObjectName("preview_note");
	note->setAlignment(Qt::AlignCenter);
	auto *show = button("Show preview");
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
	m_previewTitle = new QLabel("Preview  (edit here, then Go)");
	m_previewTitle->setObjectName("mute");
	m_landscapeButton = button("Landscape  16:9", "", "Edit the normal picture");
	m_verticalButton = button("Vertical  9:16", "", "Edit the vertical picture (Shorts, Reels, TikTok)");
	for (QPushButton *b : {m_landscapeButton, m_verticalButton})
		b->setCheckable(true);
	connect(m_landscapeButton, &QPushButton::clicked, this, [this] { m_core->setEditVertical(false); });
	connect(m_verticalButton, &QPushButton::clicked, this, [this] { m_core->setEditVertical(true); });
	auto *verticalMore = button("⋯", "icon", "Vertical canvas settings");
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
	auto *onAir = new QLabel("On air");
	onAir->setObjectName("mute");
	auto *go = button("Go", "primary", "Send the preview live with the scene's transition (hotkey in Settings > Hotkeys)");
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

QWidget *MainWindow::buildBottomBar()
{
	auto *row = new QHBoxLayout;
	row->setContentsMargins(14, 10, 14, 10);
	row->setSpacing(12);
	auto *hint = new QLabel("Drag sources in the preview to move them, drag a corner to resize. Right-click for more.");
	hint->setObjectName("mute");
	row->addWidget(hint, 1);
	m_clock = new QLabel;
	m_clock->setObjectName("mute");
	m_replay = button("Replay buffer", "", "Keeps the last seconds in memory so you can save a moment after it happens");
	m_replay->setCheckable(true);
	m_saveReplay = button("Save replay", "", "Saves the last seconds as an MP4 (set a hotkey in Settings > Hotkeys)");
	m_rec = button("Record", "rec", "Start or stop recording");
	m_pause = button("Pause", "", "Pause the recording without ending the file");
	m_vcam = button("Virtual camera", "", "Use what's on air as a webcam in Discord, Zoom, Meet and other apps");
	m_vcam->setCheckable(true);
	connect(m_pause, &QPushButton::clicked, this, &MainWindow::togglePause);
	connect(m_vcam, &QPushButton::clicked, this, &MainWindow::toggleVirtualCam);
	m_live = button("Go Live", "live", "Start or stop streaming");
	connect(m_replay, &QPushButton::clicked, this, &MainWindow::toggleReplay);
	connect(m_saveReplay, &QPushButton::clicked, this, &MainWindow::saveReplay);
	connect(m_rec, &QPushButton::clicked, this, &MainWindow::toggleRecord);
	connect(m_live, &QPushButton::clicked, this, &MainWindow::toggleStream);
	row->addWidget(m_clock);
	row->addWidget(m_vcam);
	row->addWidget(m_replay);
	row->addWidget(m_saveReplay);
	row->addWidget(m_pause);
	row->addWidget(m_rec);
	row->addWidget(m_live);
	return panel(row);
}

QWidget *MainWindow::buildUpdateBar()
{
	auto *row = new QHBoxLayout;
	row->setContentsMargins(14, 8, 10, 8);
	row->setSpacing(10);
	m_updateText = new QLabel;
	m_updateText->setWordWrap(true);
	m_updateText->setTextFormat(Qt::RichText);
	m_updateGo = button("Update now", "primary");
	m_updateLater = button("Later");
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
	m_trayLive = menu->addAction("Go Live", this, &MainWindow::toggleStream);
	m_trayRec = menu->addAction("Start recording", this, &MainWindow::toggleRecord);
	m_trayReplay = menu->addAction("Save replay", this, &MainWindow::saveReplay);
	menu->addSeparator();
	menu->addAction("Settings", this, [this] {
		m_modes->showAgain();
		openSettings();
	});
	menu->addAction("Quit Zunimi Studio", m_modes, [this] { m_modes->quit(); });
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
		QString tip = i < 9 ? QString("Scene %1").arg(i + 1) : QString();
		if (!m_core->sceneTransition(names[i]).first.isEmpty())
			tip += (tip.isEmpty() ? "" : "  ·  ") + QString("Own transition");
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
	const QString to = QInputDialog::getText(this, "Rename scene", "Name", QLineEdit::Normal, from, &ok);
	if (ok && !m_core->renameScene(from, to))
		say("That name is already used by a scene or source.", true);
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
		auto *row = new QListWidgetItem(item.locked ? item.name + "  🔒" : item.name);
		row->setData(Qt::UserRole, item.name);
		row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
		row->setCheckState(item.visible ? Qt::Checked : Qt::Unchecked);
		if (const Kind *k = kindOf(item.kind))
			row->setToolTip(QString(k->label).section("  ", 0, 0));
		else if (item.kind == "monitor_capture")
			row->setToolTip("Screen");
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
		say("That screen could not be captured.", true);
		return;
	}
	selectSource(QString::fromUtf8(obs_source_get_name(source)));
	say(label.section("  ", 0, 0) + " added. Drag a corner in the preview to make it smaller.");
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
	say(QString("All %1 screens added side by side.").arg(screens.size()));
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
		say("That source could not be added on this PC.", true);
		return;
	}
	const QString name = QString::fromUtf8(obs_source_get_name(source));
	selectSource(name);
	if (kind->ask)
		openProperties(name, true);
	else if (id == "game_capture")
		say("Game added. Start your game in fullscreen and it appears here by itself. Anti-cheat games "
		    "(Valorant and others) may need a Screen source instead for now.");
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
	const QString to = QInputDialog::getText(this, "Rename", "Name", QLineEdit::Normal, name, &ok);
	if (!ok)
		return;
	if (!m_core->renameSource(name, to))
		say("That name is already used by a scene or source.", true);
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
	menu.addAction("Options...", this, [this, name] { openProperties(name); });
	menu.addAction("Filters...", this, [this, source] { openFilters(source); });
	if (flags & OBS_SOURCE_AUDIO)
		menu.addAction("Sound settings...", this, [this, source] { openAudio(source); });
	menu.addAction("Rename...", this, [this, name] { renameSource(name); });
	if (flags & OBS_SOURCE_CONTROLLABLE_MEDIA) {
		QMenu *media = menu.addMenu("Play");
		const bool playing = obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING;
		media->addAction(playing ? "Pause" : "Play", this, [source, playing] { obs_source_media_play_pause(source, playing); });
		media->addAction("Restart", this, [source] { obs_source_media_restart(source); });
		media->addAction("Stop", this, [source] { obs_source_media_stop(source); });
		media->addAction("Next", this, [source] { obs_source_media_next(source); });
		media->addAction("Previous", this, [source] { obs_source_media_previous(source); });
	}
	if (hasVideo) {
		menu.addSeparator();
		QMenu *size = menu.addMenu("Size and place");
		size->setEnabled(!locked);
		size->addAction("Fit to screen", this, [this, name] { m_core->fit(name, ObsCore::Fit::Fill); });
		size->addAction("Stretch to screen", this, [this, name] { m_core->fit(name, ObsCore::Fit::Stretch); });
		size->addAction("Centre, original size", this, [this, name] { m_core->fit(name, ObsCore::Fit::Center); });
		size->addAction("Bottom-right corner", this, [this, name] { m_core->fit(name, ObsCore::Fit::Corner); });
		size->addAction("Left half", this, [this, name] { m_core->fit(name, ObsCore::Fit::Left); });
		size->addAction("Right half", this, [this, name] { m_core->fit(name, ObsCore::Fit::Right); });
		size->addSeparator();
		size->addAction("Edit size and place...", this, [this, name] { openTransform(name); });
		size->addAction("Reset (original size, top-left)", this, [this, name] { m_core->resetTransform(name); });

		QMenu *turn = menu.addMenu("Rotate and flip");
		turn->setEnabled(!locked);
		turn->addAction("Rotate 90 degrees right  (Ctrl+R)", this, [this, name] { m_core->rotate(name, 90); });
		turn->addAction("Rotate 90 degrees left", this, [this, name] { m_core->rotate(name, -90); });
		turn->addAction("Rotate 180 degrees", this, [this, name] { m_core->rotate(name, 180); });
		turn->addSeparator();
		turn->addAction("Flip left-right (mirror)", this, [this, name] { m_core->flip(name, true); });
		turn->addAction("Flip upside down", this, [this, name] { m_core->flip(name, false); });

		QMenu *copy = menu.addMenu("Copy size and place");
		copy->addAction("Copy  (Ctrl+C)", this, [this, name] { m_core->copyTransform(name); });
		QAction *paste = copy->addAction("Paste  (Ctrl+V)", this, [this, name] { m_core->pasteTransform(name); });
		paste->setEnabled(m_core->hasCopiedTransform() && !locked);

		QMenu *look = menu.addMenu("Look");
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
			QAction *a = blend->addAction(label, this, [this, name, type = type] { m_core->setBlend(name, type); });
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
			QAction *a = scaling->addAction(label, this, [this, name, type = type] { m_core->setScaleFilter(name, type); });
			a->setCheckable(true);
			a->setChecked(type == nowScale);
		}
		QMenu *effect = look->addMenu("Show/hide effect");
		const QString nowEffect = m_core->showHide(name);
		const std::pair<const char *, const char *> effects[] = {
			{"", "None"}, {"fade_transition", "Fade"}, {"slide_transition", "Slide"}, {"swipe_transition", "Swipe"}};
		for (const auto &[id, label] : effects) {
			QAction *a = effect->addAction(label, this, [this, name, id = QString(id)] { m_core->setShowHide(name, id, 300); });
			a->setCheckable(true);
			a->setChecked(nowEffect == id);
		}
		QMenu *extra = menu.addMenu("Projector and screenshot");
		extra->addAction("Screenshot of this source", this, [this, source] { screenshot(source); });
		fillProjectorMenu(extra->addMenu("Projector of this source"), source);
	}
	menu.addSeparator();
	menu.addAction(locked ? "Unlock" : "Lock in place", this, [this, name, locked] { m_core->setLocked(name, !locked); });
	menu.addAction(visible ? "Hide" : "Show", this, [this, name, visible] {
		m_core->setVisible(name, !visible);
		refreshSources();
	});
	menu.addAction("Move up", this, [this, name] { m_core->moveSource(name, true); });
	menu.addAction("Move down", this, [this, name] { m_core->moveSource(name, false); });
	menu.addSeparator();
	menu.addAction("Remove", this, [this, name] { m_core->removeSource(name); });
	menu.exec(global);
}

// ---- outputs
void MainWindow::toggleStream()
{
	if (m_core->streaming()) {
		m_core->stopStream();
		say("Stream ended.");
		return;
	}
	Settings &s = Settings::get();
	const QString key = s.secret("stream/key");
	const bool whip = s.value("stream/platform").toString() == "whip"; // WHIP may not need a token
	if ((key.isEmpty() && !whip) || s.value("stream/server").toString().isEmpty()) {
		say("Add your stream key to go live.");
		m_modes->showAgain();
		openSettings(SettingsDialog::Stream);
		return;
	}
	if (!preflightOk())
		return;
	QString error;
	m_live->setEnabled(false);
	m_live->setText("Connecting...");
	if (!m_core->startStream(s.value("stream/server").toString(), key, &error)) {
		say("Could not go live: " + error, true);
		updateButtons();
	}
}

void MainWindow::toggleRecord()
{
	if (m_core->recording()) {
		m_core->stopRecording();
		return;
	}
	QString error;
	if (!m_core->startRecording(Settings::get().value("record/folder", defaultFolder()).toString(), &error))
		say("Could not record: " + error, true);
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
		say("Could not start the replay buffer: " + error, true);
		updateButtons();
	}
}

void MainWindow::saveReplay()
{
	if (!m_core->replaying()) {
		say("Turn on the Replay buffer first; then Save replay keeps the moment that just happened.");
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
		say(QString("%1 %2.").arg(mic ? "Mic" : "Desktop audio", muted ? "muted" : "on"));
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

void MainWindow::applyHotkeys()
{
	const QStringList failed = m_hotkeys->apply();
	if (!failed.isEmpty())
		say("Another program already uses these hotkeys, so they won't work here: " + failed.join(", ") +
			    ". Pick different keys in Settings > Hotkeys.",
		    true);
}

void MainWindow::updateButtons()
{
	const bool live = m_core->streaming(), rec = m_core->recording(), replay = m_core->replaying();
	m_live->setEnabled(true);
	m_live->setText(live ? "End stream" : "Go Live");
	m_live->setProperty("on", live);
	m_rec->setText(rec ? "Stop recording" : "Record");
	m_rec->setProperty("on", rec);
	m_replay->setChecked(replay);
	m_replay->setText(replay ? "Replay buffer: on" : "Replay buffer");
	m_replay->setProperty("on", replay);
	m_saveReplay->setEnabled(replay);
	m_pause->setVisible(rec);
	m_pause->setText(m_core->recordingPaused() ? "Resume" : "Pause");
	m_pause->setProperty("on", m_core->recordingPaused());
	restyle(m_pause);
	const bool vcam = m_core->virtualCam();
	m_vcam->setChecked(vcam);
	m_vcam->setText(vcam ? "Virtual camera: on" : "Virtual camera");
	m_vcam->setProperty("on", vcam);
	restyle(m_vcam);
	restyle(m_live);
	restyle(m_rec);
	restyle(m_replay);
	m_trayLive->setText(live ? "End stream" : "Go Live");
	m_trayRec->setText(rec ? "Stop recording" : "Start recording");
	m_trayReplay->setEnabled(replay);
	if (!live && !rec)
		m_clock->clear();
}

void MainWindow::openSettings(int tab)
{
	SettingsDialog dialog(m_core, m_modes, this);
	dialog.checkUpdates = [this] { checkForUpdates(); };
	dialog.openTab(SettingsDialog::Tab(tab));
	if (dialog.exec() == QDialog::Accepted) {
		say("Settings saved.");
		applyHotkeys();
		updatePreview();
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
	m_stats->setText(QString("CPU %1%  ·  RAM %2 MB  ·  %3 fps")
				 .arg(h.cpu, 0, 'f', 0)
				 .arg(h.ramMb, 0, 'f', 0)
				 .arg(h.fps, 0, 'f', 0));
	const VideoPlan &p = m_core->plan();
	m_profile->setText(QString("%1  ·  %2p%3  ·  %4")
				   .arg(profileName(profileFromKey(Settings::get().value("video/profile").toString())))
				   .arg(p.height)
				   .arg(p.fps)
				   .arg(encoderLabel(p.encoder)));
	if (m_core->streaming() && m_liveSince.isValid()) {
		const qint64 secs = m_liveSince.elapsed() / 1000;
		m_clock->setText(QString("LIVE  %1:%2:%3")
					 .arg(secs / 3600, 2, 10, QChar('0'))
					 .arg((secs / 60) % 60, 2, 10, QChar('0'))
					 .arg(secs % 60, 2, 10, QChar('0')));
	} else if (m_core->recording()) {
		m_clock->setText(m_core->recordingPaused() ? "Recording paused" : "Recording");
	}
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
		return "You're live. Quitting ends the stream.";
	if (m_core->recording())
		return "A recording is running. Quitting stops and saves it.";
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
		say("Finish your stream or recording first: updating restarts Zunimi Studio.", true);
		return;
	}
	if (m_installer.isEmpty()) {
		m_updateGo->setEnabled(false);
		m_updateLater->setText("Cancel");
		m_updateText->setText(QString("Downloading Zunimi Studio %1...").arg(m_update.version.toHtmlEscaped()));
		m_updater->download(m_update);
		return;
	}
	if (!m_updater->launch(m_installer)) {
		showUpdate("The installer could not be started. Try again, or download it from the release page.",
			   "Try again", "Close");
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

// ---- main menu
void MainWindow::fillMainMenu(QMenu *menu)
{
	menu->clear();
	QAction *undo = menu->addAction(m_core->canUndo() ? "Undo " + m_core->undoLabel() : "Undo", this,
					[this] { m_core->undo(); });
	undo->setShortcut(QKeySequence::Undo);
	undo->setEnabled(m_core->canUndo());
	QAction *redo = menu->addAction(m_core->canRedo() ? "Redo " + m_core->redoLabel() : "Redo", this,
					[this] { m_core->redo(); });
	redo->setShortcut(QKeySequence("Ctrl+Y"));
	redo->setEnabled(m_core->canRedo());
	menu->addSeparator();
	fillCollectionsMenu(menu->addMenu("Scene collections  (" + m_core->collection() + ")"));
	fillProfilesMenu(menu->addMenu("Profiles  (" + m_core->profile() + ")"));
	QMenu *import = menu->addMenu("Import");
	import->addAction("From OBS Studio on this PC...", this, &MainWindow::importFromObs);
	import->addAction("Overlay pack or scene collection (.json)...", this, &MainWindow::importCollection);
	menu->addSeparator();
	QAction *studio = menu->addAction("Studio mode  (edit, then Go)", this,
					  [this] { m_core->setStudioMode(!m_core->studioMode()); });
	studio->setCheckable(true);
	studio->setChecked(m_core->studioMode());
	QAction *vertical = menu->addAction("Vertical canvas  (Shorts, Reels, TikTok)", this, [this] {
		QString error;
		if (!m_core->setVertical(!m_core->verticalOn(), &error))
			say(error, true);
	});
	vertical->setCheckable(true);
	vertical->setChecked(m_core->verticalOn());
	fillProjectorMenu(menu->addMenu("Projector  (what's on air)"), nullptr);
	QAction *vcam = menu->addAction("Virtual camera", this, &MainWindow::toggleVirtualCam);
	vcam->setCheckable(true);
	vcam->setChecked(m_core->virtualCam());
	menu->addAction("Screenshot of what's on air", this, [this] { screenshot(); });
	menu->addAction("Mark moment  (chapter + saved replay)", this, [this] { m_assist->mark(); });
	menu->addAction("Check my setup now", this, [this] {
		const QVector<Finding> found = m_assist->preflight(true);
		if (found.isEmpty()) {
			say("All good: scene, mic, disk and encoder look ready.");
			return;
		}
		QStringList lines;
		for (const Finding &f : found)
			lines << (f.level >= 2 ? "• " : "◦ ") + f.text;
		QMessageBox::information(this, "Setup check", lines.join("\n"));
	});
	menu->addSeparator();
	menu->addAction("Stats", this, [this] {
		auto *dialog = new StatsDialog(m_core, this);
		dialog->setAttribute(Qt::WA_DeleteOnClose);
		dialog->show();
	});
	menu->addAction("Open recordings folder", this, [] {
		const QString folder = Settings::get().value("record/folder", defaultFolder()).toString();
		QDir().mkpath(folder);
		QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
	});
	menu->addAction("Open log files", this,
			[] { QDesktopServices::openUrl(QUrl::fromLocalFile(Settings::dataDir() + "/logs")); });
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
		const QString name = QInputDialog::getText(this, copy ? "Copy scene collection" : "New scene collection",
							   "Name", QLineEdit::Normal,
							   copy ? m_core->collection() + " copy" : QString(), &ok);
		QString error;
		if (ok && !m_core->newCollection(name, copy, &error))
			say(error, true);
	};
	menu->addAction("New (empty)...", this, [create] { create(false); });
	menu->addAction("Copy this one...", this, [create] { create(true); });
	menu->addAction("Rename this one...", this, [this] {
		bool ok = false;
		const QString from = m_core->collection();
		const QString to = QInputDialog::getText(this, "Rename scene collection", "Name", QLineEdit::Normal, from, &ok);
		QString error;
		if (ok && !m_core->renameCollection(from, to, &error))
			say(error, true);
	});
	QAction *remove = menu->addAction("Remove this one...", this, [this] {
		const QString name = m_core->collection();
		if (QMessageBox::question(this, "Remove scene collection",
					  QString("Remove \"%1\" and all its scenes? This can't be undone.").arg(name),
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
				say("Profile: " + name + ". Its stream key, quality and recording settings are in use.");
		});
		a->setCheckable(true);
		a->setChecked(name == m_core->profile());
	}
	menu->addSeparator();
	menu->addAction("New profile...", this, [this] {
		bool ok = false;
		const QString name = QInputDialog::getText(this, "New profile",
							   "Name (for example \"YouTube 1080p\" or \"Practice, record only\")",
							   QLineEdit::Normal, {}, &ok);
		QString error;
		if (ok && !m_core->newProfile(name, &error))
			say(error, true);
	});
	QAction *remove = menu->addAction("Remove this profile", this, [this] {
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
		menu->addAction(QString("Fullscreen on screen %1  (%2×%3)").arg(i + 1).arg(g.width()).arg(g.height()), this,
				[this, source, i] { openProjector(source, i); });
	}
	menu->addAction("In a window", this, [this, source] { openProjector(source, -1); });
}

void MainWindow::openProjector(obs_source_t *source, int screen)
{
	auto *projector = new ProjectorWindow(m_core, source, screen);
	projector->setWindowIcon(windowIcon());
	if (screen >= 0)
		say("Projector open. Press Esc or double-click it to close.");
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
	menu.addAction("Rename...", this, [this] { renameScene(); });
	menu.addAction("Copy", this, [this, name] { m_core->duplicateScene(name); });
	QMenu *own = menu.addMenu("Transition into this scene");
	const auto current = m_core->sceneTransition(name);
	QAction *def = own->addAction("Use the default", this, [this, name] { m_core->setSceneTransition(name, {}, 0); });
	def->setCheckable(true);
	def->setChecked(current.first.isEmpty());
	for (const auto &[id, label] : ObsCore::transitionKinds()) {
		QAction *a = own->addAction(label, this, [this, name, id = id] {
			bool ok = true;
			int ms = m_core->transitionMs();
			if (id != "cut_transition" && id != "obs_stinger_transition")
				ms = QInputDialog::getInt(this, "Transition length", "Milliseconds", std::max(ms, 300), 50, 20000,
							  50, &ok);
			if (ok)
				m_core->setSceneTransition(name, id, ms);
			refreshScenes();
		});
		a->setCheckable(true);
		a->setChecked(current.first == id);
	}
	obs_source_t *src = obs_get_source_by_name(name.toUtf8().constData());
	fillProjectorMenu(menu.addMenu("Projector of this scene"), src);
	menu.addAction("Screenshot of this scene", this, [this, src] { screenshot(src); });
	menu.addSeparator();
	menu.addAction("Move up", this, [this, name] { m_core->moveScene(name, true); });
	menu.addAction("Move down", this, [this, name] { m_core->moveScene(name, false); });
	menu.addSeparator();
	QAction *remove = menu.addAction("Remove", this, [this, name] {
		if (QMessageBox::question(this, "Remove scene",
					  QString("Remove the scene \"%1\"? Sources that only this scene uses go too. "
						  "Ctrl+Z brings it back.")
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
	dialog.setWindowTitle("Stinger transition");
	auto *path = new QLineEdit(st.value("scenes/stingerPath").toString());
	auto *browse = new QPushButton("Choose...");
	connect(browse, &QPushButton::clicked, &dialog, [&dialog, path] {
		const QString file = QFileDialog::getOpenFileName(&dialog, "Stinger video", path->text(),
								  "Videos (*.webm *.mov *.mp4 *.mkv *.gif)");
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
	form->addRow("Video", row);
	form->addRow("Switch scenes at", point);
	auto *note = new QLabel("Use a video with a see-through background (.webm or .mov) so the scenes show around "
				"it. Pick the moment the video covers the whole screen.");
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
	say("Stinger set. It plays every time you switch scenes.");
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
		this, "Import a scene collection", QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
		"Scene collection (*.json)");
	if (file.isEmpty())
		return;
	QString folder;
	if (QMessageBox::question(this, "Import",
				  "Is the pack's pictures and videos folder somewhere else than next to this file? "
				  "(Pick No if you unzipped the whole pack in one place.)",
				  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
		folder = QFileDialog::getExistingDirectory(this, "The pack's folder", QFileInfo(file).absolutePath());
	QStringList notes;
	QString error;
	const QString name = m_core->importCollection(file, folder, &notes, &error);
	if (name.isEmpty()) {
		say(error, true);
		return;
	}
	QMessageBox::information(this, "Imported \"" + name + "\"", notes.join("\n\n"));
}

void MainWindow::importFromObs()
{
	// OBS keeps each scene collection as a .json in %APPDATA%\obs-studio\basic\scenes.
	const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/obs-studio/basic/scenes";
	const QFileInfoList files = QDir(dir).entryInfoList({"*.json"}, QDir::Files, QDir::Time);
	if (files.isEmpty()) {
		QMessageBox::information(this, "Import from OBS Studio",
					 "No OBS Studio scene collections were found on this PC. If you have a .json file "
					 "from OBS, use Import > Overlay pack or scene collection.");
		return;
	}
	QStringList names;
	for (const QFileInfo &f : files)
		names << f.completeBaseName();
	bool ok = false;
	const QString pick = QInputDialog::getItem(this, "Import from OBS Studio",
						   "Which scene collection? (OBS itself is not changed.)", names, 0, false, &ok);
	if (!ok)
		return;
	QStringList notes;
	QString error;
	const QString name = m_core->importCollection(dir + "/" + pick + ".json", {}, &notes, &error);
	if (name.isEmpty()) {
		say(error, true);
		return;
	}
	QMessageBox::information(this, "Imported \"" + name + "\"", notes.join("\n\n"));
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
		say("Pause works while recording.");
		return;
	}
	if (!m_core->pauseRecording(!m_core->recordingPaused()))
		say("This recording can't be paused. MP4 and MKV recordings can.", true);
}

void MainWindow::toggleVirtualCam()
{
	if (m_core->virtualCam()) {
		m_core->stopVirtualCam();
		return;
	}
	if (!ObsCore::virtualCamInstalled() &&
	    QMessageBox::question(this, "Virtual camera",
				  "The first time, Windows asks for permission to add the virtual camera (one Yes, then "
				  "it stays). Continue?") != QMessageBox::Yes) {
		updateButtons();
		return;
	}
	QString error;
	if (!m_core->startVirtualCam(&error))
		say("Virtual camera: " + error, true);
	updateButtons();
}

void MainWindow::addOverlay(const QString &kind)
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
		say("That widget could not be added.", true);
		return;
	}
	const QString name = QString::fromUtf8(obs_source_get_name(source));
	selectSource(name);
	if (kind == "alerts") {
		say("Alert box added. Design it here, then press Test to see it on stream.");
		overlays::Engine::get().openDesigner(source);
	} else if (kind == "chat" || kind == "goal" || kind == "label" || kind == "events") {
		const QStringList services = EventHub::get().services();
		if (std::none_of(services.begin(), services.end(), [](const QString &k) { return EventHub::get().connected(k); }))
			say("Added. Connect your channels in Settings > Connections so it fills in.");
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
	dialog.setWindowTitle("Before you go live");
	auto *layout = new QVBoxLayout(&dialog);
	layout->setContentsMargins(18, 18, 18, 18);
	layout->setSpacing(10);
	for (const Finding &f : found) {
		auto *row = new QHBoxLayout;
		auto *text = new QLabel((f.level >= 2 ? "<b style='color:#FF4D6D'>●</b>  " : "<b style='color:#FFB020'>●</b>  ") +
					f.text.toHtmlEscaped());
		text->setWordWrap(true);
		row->addWidget(text, 1);
		if (!f.fix.isEmpty()) {
			auto *fixButton = new QPushButton(f.fixLabel);
			connect(fixButton, &QPushButton::clicked, &dialog, [this, fixButton, id = f.fix] {
				m_assist->applyFix(id);
				fixButton->setEnabled(false);
				fixButton->setText("Fixed");
			});
			row->addWidget(fixButton);
		}
		layout->addLayout(row);
	}
	auto *never = new QCheckBox("Don't check before going live");
	layout->addWidget(never);
	auto *buttons = new QDialogButtonBox;
	QPushButton *go = buttons->addButton("Go live anyway", QDialogButtonBox::AcceptRole);
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
		say("The browser source is missing from this copy of Zunimi Studio. Installing the latest version brings it back.", true);
		return false;
	}
	Addons::setBrowserEnabled(true);
	if (QMessageBox::question(this, "Browser source",
				  "Web pages and StreamElements/Streamlabs overlays use the browser source. It starts the first "
				  "time you use it, so Zunimi Studio needs a quick restart. "
				  "Restart now?") != QMessageBox::Yes)
		return false;
	if (!quitWouldStop().isEmpty()) {
		say("Finish your stream or recording first: starting the browser source needs a restart.", true);
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
		this, overlayLink ? "Overlay or widget link" : "Web page",
		overlayLink ? "Paste the overlay or widget link from StreamElements or Streamlabs\n"
			      "(StreamElements: Overlays > your overlay > Copy URL. Streamlabs: the widget's URL):"
			    : "Address of the page:",
		QLineEdit::Normal, {}, &ok).trimmed();
	if (!ok || url.isEmpty())
		return;
	if (!url.startsWith("https://") && !url.startsWith("http://") && !url.startsWith("file:///")) {
		say("That doesn't look like a web address (it should start with https://).", true);
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
	QString name = overlayLink ? (url.contains("streamelements") ? "StreamElements overlay" : url.contains("streamlabs") ? "Streamlabs widget" : "Overlay")
				   : QUrl(url).host();
	obs_source_t *source = overlayLink ? m_core->addSourceAt("browser_source", name, s, QRectF(0, 0, canvas.width(), canvas.height()))
					   : m_core->addSource("browser_source", name.isEmpty() ? "Web page" : name, s, ObsCore::Fit::Fill);
	obs_data_release(s);
	if (!source) {
		say("The browser source could not be added.", true);
		return;
	}
	selectSource(QString::fromUtf8(obs_source_get_name(source)));
	say(overlayLink ? "Overlay added over the whole screen. Its alerts and widgets appear when they're triggered."
			: "Page added. Right-click it for Options (size, custom CSS, refresh).");
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
	dialog.setWindowTitle("Vertical canvas");
	auto *size = new QComboBox;
	size->addItem("1080 x 1920 (sharp)", "1080");
	size->addItem("720 x 1280 (lighter)", "720");
	size->setCurrentIndex(m_core->verticalSize().width() == 720 ? 1 : 0);
	auto *record = new QCheckBox("Record it too, next to every recording");
	record->setChecked(st.value("vertical/record", true).toBool());
	auto *stream = new QCheckBox("Stream it too, while you're live");
	stream->setChecked(st.value("vertical/stream", false).toBool());
	auto *server = new QLineEdit(st.value("vertical/server").toString());
	server->setPlaceholderText("rtmp://... (YouTube: rtmps://a.rtmps.youtube.com:443/live2)");
	auto *key = new QLineEdit(st.secret("vertical/key"));
	key->setEchoMode(QLineEdit::Password);
	key->setPlaceholderText("Stream key for the vertical stream");
	auto *bitrate = new QSpinBox;
	bitrate->setRange(1000, 20000);
	bitrate->setSingleStep(500);
	bitrate->setSuffix(" kbps");
	bitrate->setValue(st.value("vertical/bitrate", std::max(2500, m_core->plan().bitrate * 3 / 4)).toInt());
	auto *form = new QFormLayout;
	form->addRow("Size", size);
	form->addRow("", record);
	form->addRow("", stream);
	form->addRow("Server", server);
	form->addRow("Stream key", key);
	form->addRow("Bitrate", bitrate);
	auto *note = new QLabel("The vertical picture is encoded separately, so it costs a second encode (easy for a "
				"graphics card encoder, heavy for x264). YouTube takes vertical live streams as Shorts.");
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
	st.set("vertical/stream", stream->isChecked());
	st.set("vertical/server", server->text().trimmed());
	st.setSecret("vertical/key", key->text().trimmed());
	st.set("vertical/bitrate", bitrate->value());
	m_core->outputSettingsChanged();
	if (resized)
		say("The new vertical size applies after a restart of Zunimi Studio.");
}
