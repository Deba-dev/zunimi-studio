#include "ui/MainWindow.h"

#include "core/ObsCore.h"
#include "core/Settings.h"
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
#include <QCloseEvent>
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
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QSpinBox>
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
	auto *mixerLayout = new QVBoxLayout;
	mixerLayout->setContentsMargins(0, 0, 0, 0);
	mixerLayout->addWidget(m_mixer);
	QWidget *mixerPanel = panel(mixerLayout);
	mixerPanel->setFixedWidth(270);
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
		if (on) {
			m_liveSince.start();
			say("You're live!");
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
	refreshScenes();
	refreshSources();
	updateButtons();

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
	auto *settings = button("Settings", "", "Stream key, quality, recording, replay, hotkeys, window");
	connect(settings, &QPushButton::clicked, this, [this] { openSettings(); });
	row->addWidget(logo);
	row->addWidget(brand);
	row->addSpacing(16);
	row->addWidget(m_healthDot);
	row->addWidget(m_health, 1);
	row->addWidget(m_stats);
	row->addWidget(m_profile);
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
	m_transition->addItem("Fade", "fade_transition");
	m_transition->addItem("Cut", "cut_transition");
	m_transition->addItem("Swipe", "swipe_transition");
	m_transition->addItem("Slide", "slide_transition");
	m_transition->addItem("Fade to colour", "fade_to_color_transition");
	m_transition->setCurrentIndex(std::max(0, m_transition->findData(m_core->transitionId())));
	m_transitionMs = new QSpinBox;
	m_transitionMs->setRange(0, 5000);
	m_transitionMs->setSingleStep(100);
	m_transitionMs->setSuffix(" ms");
	m_transitionMs->setValue(m_core->transitionMs());
	auto applyTransition = [this] {
		m_core->setTransition(m_transition->currentData().toString(), m_transitionMs->value());
	};
	connect(m_transition, &QComboBox::activated, this, applyTransition);
	connect(m_transitionMs, &QSpinBox::editingFinished, this, applyTransition);
	auto *transition = new QHBoxLayout;
	auto *tlabel = new QLabel("Transition");
	tlabel->setObjectName("mute");
	transition->addWidget(tlabel);
	transition->addWidget(m_transition, 1);
	transition->addWidget(m_transitionMs);

	connect(m_scenes, &QListWidget::currentRowChanged, this, [this](int row) {
		if (row >= 0)
			m_core->setScene(m_scenes->item(row)->text());
	});
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
	col->addWidget(m_previewStack);
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
	m_rec = button("Record", "rec", "Start or stop recording (saved as MP4)");
	m_live = button("Go Live", "live", "Start or stop streaming");
	connect(m_replay, &QPushButton::clicked, this, &MainWindow::toggleReplay);
	connect(m_saveReplay, &QPushButton::clicked, this, &MainWindow::saveReplay);
	connect(m_rec, &QPushButton::clicked, this, &MainWindow::toggleRecord);
	connect(m_live, &QPushButton::clicked, this, &MainWindow::toggleStream);
	row->addWidget(m_clock);
	row->addWidget(m_replay);
	row->addWidget(m_saveReplay);
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
	menu->addAction("Quit Zunimi Studio", m_modes, &WindowModes::quit);
}

// ---- scenes
void MainWindow::refreshScenes()
{
	QSignalBlocker block(m_scenes);
	m_scenes->clear();
	const QStringList names = m_core->scenes();
	for (int i = 0; i < names.size(); i++) {
		auto *item = new QListWidgetItem(names[i]);
		if (i < 9)
			item->setToolTip(QString("Scene %1").arg(i + 1));
		m_scenes->addItem(item);
		if (names[i] == m_core->currentScene())
			m_scenes->setCurrentItem(item);
	}
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

void MainWindow::sourceMenu(const QString &name, const QPoint &global)
{
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
	const bool hasVideo = obs_source_get_output_flags(obs_sceneitem_get_source(item)) & OBS_SOURCE_VIDEO;
	menu.addAction("Options...", this, [this, name] { openProperties(name); });
	menu.addAction("Filters...", this, [this, item] { openFilters(obs_sceneitem_get_source(item)); });
	menu.addAction("Rename...", this, [this, name] { renameSource(name); });
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
	if (key.isEmpty() || s.value("stream/server").toString().isEmpty()) {
		say("Add your stream key to go live.");
		m_modes->showAgain();
		openSettings(SettingsDialog::Stream);
		return;
	}
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
		m_clock->setText("Recording");
	}
}

void MainWindow::updatePreview()
{
	const bool visible = isVisible() && !isMinimized();
	const bool pauseLive = Settings::get().value("preview/pauseWhileLive", false).toBool() &&
			       (m_core->streaming() || m_core->recording());
	const bool run = visible && m_previewWanted && !pauseLive;
	m_preview->setRunning(run);
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
	m_modes->quit();
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
