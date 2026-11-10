#include "ui/MainWindow.h"

#include "core/ObsCore.h"
#include "core/Settings.h"
#include "ui/PreviewWidget.h"
#include "ui/SettingsDialog.h"
#include "ui/Theme.h"
#include "ui/WindowModes.h"
#include "version.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QStandardPaths>
#include <QStyle>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

namespace {
struct Kind {
	const char *id, *label, *name;
	ObsCore::Fit fit;
};
const Kind KINDS[] = {
	{"game_capture", "Game  (captures whichever game is fullscreen)", "Game", ObsCore::Fit::Fill},
	{"monitor_capture", "Screen", "Screen", ObsCore::Fit::Fill},
	{"window_capture", "Window", "Window", ObsCore::Fit::Fill},
	{"dshow_input", "Webcam", "Webcam", ObsCore::Fit::Corner},
	{"image_source", "Image", "Image", ObsCore::Fit::Center},
	{"text_gdiplus", "Text", "Text", ObsCore::Fit::Center},
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

// A slider that feels even: the slider is in decibels-ish steps, libobs wants a multiplier.
float toMul(int slider)
{
	return slider <= 0 ? 0.0f : float(std::pow(slider / 100.0, 2.0));
}

int toSlider(float mul)
{
	return int(std::round(std::sqrt(std::max(0.0f, mul)) * 100.0));
}
} // namespace

MainWindow::MainWindow(ObsCore *core) : m_core(core)
{
	setWindowTitle(ZUNIMI_NAME);
	resize(1280, 780);
	setMinimumSize(900, 560);
	m_modes = new WindowModes(this, ZUNIMI_NAME);
	m_modes->busy = [this] { return quitWouldStop(); };

	auto *central = new QWidget;
	auto *root = new QVBoxLayout(central);
	root->setContentsMargins(14, 10, 14, 12);
	root->setSpacing(10);
	root->addWidget(buildTopBar());
	auto *body = new QHBoxLayout;
	body->setSpacing(10);
	body->addWidget(buildSources());
	body->addWidget(buildPreview(), 1);
	root->addLayout(body, 1);
	m_banner = new QLabel;
	m_banner->setWordWrap(true);
	m_banner->hide();
	root->addWidget(m_banner);
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
	connect(m_core, &ObsCore::problem, this, [this](const QString &text) { say(text, true); });
	connect(m_core, &ObsCore::notice, this, [this](const QString &text) { say(text); });
	connect(m_core, &ObsCore::sceneChanged, this, &MainWindow::refreshSources);
	connect(m_modes, &WindowModes::visibilityChanged, this, [this](bool) { updatePreview(); });
	refreshSources();
	updateButtons();

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
}

// ---- layout
QWidget *MainWindow::buildTopBar()
{
	auto *bar = new QWidget;
	bar->setObjectName("topbar");
	auto *row = new QHBoxLayout(bar);
	row->setContentsMargins(4, 0, 4, 0);
	row->setSpacing(10);
	auto *brand = new QLabel("ZUNIMI STUDIO");
	brand->setObjectName("brand");
	m_healthDot = new QLabel;
	m_healthDot->setFixedSize(10, 10);
	m_health = new QLabel("Ready");
	m_stats = new QLabel;
	m_stats->setObjectName("chip");
	m_profile = new QLabel;
	m_profile->setObjectName("chip");
	auto *settings = button("Settings", "", "Stream key, quality, recording folder, window");
	connect(settings, &QPushButton::clicked, this, [this] { openSettings(); });
	row->addWidget(brand);
	row->addSpacing(16);
	row->addWidget(m_healthDot);
	row->addWidget(m_health, 1);
	row->addWidget(m_stats);
	row->addWidget(m_profile);
	row->addWidget(settings);
	return bar;
}

QWidget *MainWindow::buildSources()
{
	auto *col = new QVBoxLayout;
	col->setContentsMargins(12, 12, 12, 12);
	col->setSpacing(8);
	auto *title = new QLabel("Sources");
	title->setObjectName("h2");
	m_sources = new QListWidget;
	m_sources->setToolTip("Tick to show or hide. Double-click to change what it captures.");
	m_empty = new QLabel("Nothing here yet.\nAdd a Game or your Screen to start.");
	m_empty->setObjectName("mute");
	m_empty->setAlignment(Qt::AlignCenter);
	auto *add = button("+  Add source", "primary");
	auto *menu = new QMenu(add);
	for (const Kind &k : KINDS)
		menu->addAction(k.label, this, [this, id = QString(k.id)] { addSource(id); });
	add->setMenu(menu);
	auto *up = button("↑", "icon", "Move up (in front)");
	auto *down = button("↓", "icon", "Move down (behind)");
	auto *edit = button("Change", "", "Change what this source captures");
	auto *remove = button("Remove", "", "Remove the selected source");
	auto *tools = new QHBoxLayout;
	tools->setSpacing(6);
	tools->addWidget(up);
	tools->addWidget(down);
	tools->addStretch();
	tools->addWidget(edit);
	tools->addWidget(remove);

	connect(m_sources, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
		m_core->setVisible(item->data(Qt::UserRole).toString(), item->checkState() == Qt::Checked);
	});
	connect(m_sources, &QListWidget::itemDoubleClicked, this, [this] { changeSelected(); });
	auto selected = [this] {
		QListWidgetItem *item = m_sources->currentItem();
		return item ? item->data(Qt::UserRole).toString() : QString();
	};
	connect(up, &QPushButton::clicked, this, [this, selected] {
		const QString name = selected();
		if (!name.isEmpty())
			m_core->moveSource(name, true);
	});
	connect(down, &QPushButton::clicked, this, [this, selected] {
		const QString name = selected();
		if (!name.isEmpty())
			m_core->moveSource(name, false);
	});
	connect(edit, &QPushButton::clicked, this, [this] { changeSelected(); });
	connect(remove, &QPushButton::clicked, this, [this, selected] {
		const QString name = selected();
		if (!name.isEmpty())
			m_core->removeSource(name);
	});

	col->addWidget(title);
	col->addWidget(m_sources, 1);
	col->addWidget(m_empty, 1);
	col->addLayout(tools);
	col->addWidget(add);
	QWidget *w = panel(col);
	w->setFixedWidth(290);
	return w;
}

QWidget *MainWindow::buildPreview()
{
	auto *col = new QVBoxLayout;
	col->setContentsMargins(8, 8, 8, 8);
	m_previewStack = new QStackedWidget;
	m_preview = new PreviewWidget;
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
	col->addWidget(m_previewStack);
	return panel(col);
}

QWidget *MainWindow::audioControl(const QString &label, obs_source_t *source, const QString &key)
{
	auto *box = new QWidget;
	auto *row = new QHBoxLayout(box);
	row->setContentsMargins(0, 0, 0, 0);
	row->setSpacing(8);
	auto *name = new QLabel(label);
	auto *slider = new QSlider(Qt::Horizontal);
	slider->setRange(0, 100);
	slider->setFixedWidth(150);
	auto *mute = button("Mute", "icon");
	mute->setCheckable(true);
	if (!source) {
		box->setEnabled(false);
		box->setToolTip("No " + label.toLower() + " device found.");
	} else {
		slider->setValue(toSlider(obs_source_get_volume(source)));
		mute->setChecked(obs_source_muted(source));
		mute->setText(mute->isChecked() ? "Muted" : "Mute");
		connect(slider, &QSlider::valueChanged, this, [source, key](int v) {
			obs_source_set_volume(source, toMul(v));
			Settings::get().set("audio/" + key + "Volume", double(toMul(v)));
		});
		connect(mute, &QPushButton::toggled, this, [source, key, mute](bool on) {
			obs_source_set_muted(source, on);
			mute->setText(on ? "Muted" : "Mute");
			Settings::get().set("audio/" + key + "Muted", on);
		});
	}
	row->addWidget(name);
	row->addWidget(slider);
	row->addWidget(mute);
	return box;
}

QWidget *MainWindow::buildBottomBar()
{
	auto *row = new QHBoxLayout;
	row->setContentsMargins(14, 10, 14, 10);
	row->setSpacing(18);
	row->addWidget(audioControl("Desktop", m_core->desktopAudio(), "desktop"));
	row->addWidget(audioControl("Mic", m_core->microphone(), "mic"));
	auto *cleanup = new QCheckBox("Clean up mic noise");
	cleanup->setToolTip("RNNoise: removes keyboard, fan and room noise. Light on the CPU.");
	cleanup->setChecked(Settings::get().value("audio/micCleanup", true).toBool());
	cleanup->setEnabled(m_core->microphone() != nullptr);
	connect(cleanup, &QCheckBox::toggled, m_core, &ObsCore::setMicCleanup);
	row->addWidget(cleanup);
	row->addStretch();
	m_clock = new QLabel;
	m_clock->setObjectName("mute");
	m_rec = button("Record", "rec", "Start or stop recording (saved as MP4)");
	m_live = button("Go Live", "live", "Start or stop streaming");
	connect(m_rec, &QPushButton::clicked, this, &MainWindow::toggleRecord);
	connect(m_live, &QPushButton::clicked, this, &MainWindow::toggleStream);
	row->addWidget(m_clock);
	row->addWidget(m_rec);
	row->addWidget(m_live);
	return panel(row);
}

void MainWindow::buildTray()
{
	QMenu *menu = m_modes->trayMenu();
	m_trayLive = menu->addAction("Go Live", this, &MainWindow::toggleStream);
	m_trayRec = menu->addAction("Start recording", this, &MainWindow::toggleRecord);
	menu->addSeparator();
	menu->addAction("Settings", this, [this] {
		m_modes->showAgain();
		openSettings();
	});
	menu->addAction("Quit Zunimi Studio", m_modes, &WindowModes::quit);
}

// ---- sources
void MainWindow::refreshSources()
{
	QSignalBlocker block(m_sources);
	const QString current = m_sources->currentItem() ? m_sources->currentItem()->data(Qt::UserRole).toString() : "";
	m_sources->clear();
	for (const SceneItem &item : m_core->items()) {
		const Kind *kind = kindOf(item.kind);
		auto *row = new QListWidgetItem(item.name + (kind ? "" : QString()));
		row->setData(Qt::UserRole, item.name);
		row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
		row->setCheckState(item.visible ? Qt::Checked : Qt::Unchecked);
		m_sources->addItem(row);
		if (item.name == current)
			m_sources->setCurrentItem(row);
	}
	const bool empty = m_sources->count() == 0;
	m_sources->setVisible(!empty);
	m_empty->setVisible(empty);
}

void MainWindow::addSource(const QString &id)
{
	const Kind *kind = kindOf(id);
	if (!kind)
		return;
	obs_data_t *s = obs_data_create();
	if (id == "game_capture") {
		obs_data_set_string(s, "capture_mode", "any_fullscreen");
	} else if (id == "image_source") {
		const QString file = QFileDialog::getOpenFileName(this, "Pick an image", {},
								  "Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp)");
		if (file.isEmpty()) {
			obs_data_release(s);
			return;
		}
		obs_data_set_string(s, "file", file.toUtf8().constData());
	} else if (id == "text_gdiplus") {
		bool ok = false;
		const QString text = QInputDialog::getText(this, "Text", "What should it say?", QLineEdit::Normal, "", &ok);
		if (!ok || text.trimmed().isEmpty()) {
			obs_data_release(s);
			return;
		}
		obs_data_set_string(s, "text", text.toUtf8().constData());
		obs_data_t *font = obs_data_create();
		obs_data_set_string(font, "face", "Segoe UI");
		obs_data_set_int(font, "size", 72);
		obs_data_set_string(font, "style", "Bold");
		obs_data_set_obj(s, "font", font);
		obs_data_release(font);
	}
	obs_source_t *source = m_core->addSource(kind->id, kind->name, s, kind->fit);
	obs_data_release(s);
	if (!source) {
		say("That source could not be added on this PC.", true);
		return;
	}
	const QString name = QString::fromUtf8(obs_source_get_name(source));
	bool picked = true;
	if (id == "monitor_capture")
		picked = pick(source, "monitor_id", "Which screen?");
	else if (id == "window_capture")
		picked = pick(source, "window", "Which window?");
	else if (id == "dshow_input")
		picked = pick(source, "video_device_id", "Which camera?");
	if (!picked) {
		m_core->removeSource(name);
		return;
	}
	if (id == "game_capture")
		say("Game added. Start your game in fullscreen and it appears here by itself.");
	m_core->save();
}

bool MainWindow::pick(obs_source_t *source, const char *property, const QString &title)
{
	obs_properties_t *props = obs_source_properties(source);
	obs_property_t *p = props ? obs_properties_get(props, property) : nullptr;
	QStringList names, values;
	bool isString = true;
	if (p) {
		isString = obs_property_list_format(p) == OBS_COMBO_FORMAT_STRING;
		for (size_t i = 0; i < obs_property_list_item_count(p); i++) {
			const QString value = isString ? QString::fromUtf8(obs_property_list_item_string(p, i))
						       : QString::number(obs_property_list_item_int(p, i));
			if (value.isEmpty() || obs_property_list_item_disabled(p, i))
				continue;
			names << QString::fromUtf8(obs_property_list_item_name(p, i));
			values << value;
		}
	}
	if (props)
		obs_properties_destroy(props);
	if (names.isEmpty()) {
		say("Nothing to pick from: " + title.toLower().remove('?') + " was not found.", true);
		return false;
	}
	bool ok = false;
	const QString chosen = QInputDialog::getItem(this, title, title, names, 0, false, &ok);
	if (!ok)
		return false;
	const QString value = values.value(names.indexOf(chosen));
	obs_data_t *s = obs_data_create();
	if (isString)
		obs_data_set_string(s, property, value.toUtf8().constData());
	else
		obs_data_set_int(s, property, value.toLongLong());
	obs_source_update(source, s);
	obs_data_release(s);
	return true;
}

void MainWindow::changeSelected()
{
	QListWidgetItem *item = m_sources->currentItem();
	if (!item)
		return;
	const QString name = item->data(Qt::UserRole).toString();
	obs_source_t *source = obs_get_source_by_name(name.toUtf8().constData());
	if (!source)
		return;
	const QString id = QString::fromUtf8(obs_source_get_id(source));
	if (id == "monitor_capture")
		pick(source, "monitor_id", "Which screen?");
	else if (id == "window_capture")
		pick(source, "window", "Which window?");
	else if (id == "dshow_input")
		pick(source, "video_device_id", "Which camera?");
	else if (id == "game_capture")
		say("Game capture follows whichever game is fullscreen. Nothing to change.");
	else
		say("This source has nothing to change yet.");
	obs_source_release(source);
	m_core->save();
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
	const QString folder = Settings::get()
				       .value("record/folder", QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) +
								       "/Zunimi Studio")
				       .toString();
	if (!m_core->startRecording(folder, &error))
		say("Could not record: " + error, true);
}

void MainWindow::updateButtons()
{
	const bool live = m_core->streaming(), rec = m_core->recording();
	m_live->setEnabled(true);
	m_live->setText(live ? "End stream" : "Go Live");
	m_live->setProperty("on", live);
	m_rec->setText(rec ? "Stop recording" : "Record");
	m_rec->setProperty("on", rec);
	restyle(m_live);
	restyle(m_rec);
	m_trayLive->setText(live ? "End stream" : "Go Live");
	m_trayRec->setText(rec ? "Stop recording" : "Start recording");
	if (!live && !rec)
		m_clock->clear();
}

void MainWindow::openSettings(int tab)
{
	SettingsDialog dialog(m_core, m_modes, this);
	dialog.openTab(SettingsDialog::Tab(tab));
	if (dialog.exec() == QDialog::Accepted) {
		say("Settings saved.");
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
	const bool pauseLive = Settings::get().value("preview/pauseWhileLive", false).toBool() && m_core->busy();
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

// ---- window
void MainWindow::keyPressEvent(QKeyEvent *event)
{
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
