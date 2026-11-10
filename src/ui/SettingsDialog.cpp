#include "ui/SettingsDialog.h"

#include "core/ObsCore.h"
#include "core/Settings.h"
#include "ui/Hotkeys.h"
#include "ui/WindowModes.h"
#include "version.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QKeySequenceEdit>
#include <QScrollArea>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {
struct Platform {
	const char *key, *label, *server, *hint;
};
const Platform PLATFORMS[] = {
	{"twitch", "Twitch", "rtmp://live.twitch.tv/app", "Key: Twitch Creator Dashboard > Settings > Stream."},
	{"youtube", "YouTube", "rtmps://a.rtmps.youtube.com:443/live2", "Key: YouTube Studio > Go live > Stream key."},
	{"kick", "Kick", "", "Kick gives you a Stream URL and a Stream Key: Dashboard > Settings > Stream URL & Key."},
	{"custom", "Other (custom RTMP)", "", "Paste the server address and key your platform gives you."},
};

QLabel *hint(const QString &text)
{
	auto *label = new QLabel(text);
	label->setObjectName("hint");
	label->setWordWrap(true);
	return label;
}

QString defaultFolder()
{
	return QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + "/Zunimi Studio";
}
} // namespace

SettingsDialog::SettingsDialog(ObsCore *core, WindowModes *modes, QWidget *parent)
	: QDialog(parent), m_core(core), m_modes(modes)
{
	setWindowTitle("Settings");
	resize(620, 520);
	m_tabs = new QTabWidget;
	m_tabs->addTab(streamTab(), "Stream");
	m_tabs->addTab(videoTab(), "Video");
	m_tabs->addTab(recordingTab(), "Recording");
	m_tabs->addTab(hotkeysTab(), "Hotkeys");
	m_tabs->addTab(windowTab(), "Window");
	m_tabs->addTab(aboutTab(), "About this PC");
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
	buttons->button(QDialogButtonBox::Save)->setObjectName("primary");
	connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::save);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(18, 18, 18, 18);
	layout->addWidget(m_tabs, 1);
	layout->addWidget(buttons);
}

void SettingsDialog::openTab(Tab tab)
{
	m_tabs->setCurrentIndex(int(tab));
}

QWidget *SettingsDialog::streamTab()
{
	Settings &s = Settings::get();
	auto *page = new QWidget;
	auto *form = new QFormLayout(page);
	form->setContentsMargins(18, 18, 18, 18);
	form->setVerticalSpacing(12);
	m_platform = new QComboBox;
	for (const Platform &p : PLATFORMS)
		m_platform->addItem(p.label, p.key);
	m_server = new QLineEdit(s.value("stream/server").toString());
	m_server->setPlaceholderText("rtmp://...");
	m_key = new QLineEdit(s.secret("stream/key"));
	m_key->setEchoMode(QLineEdit::Password);
	m_key->setPlaceholderText("Paste your stream key");
	auto *show = new QPushButton("Show");
	show->setCheckable(true);
	connect(show, &QPushButton::toggled, this, [this, show](bool on) {
		m_key->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
		show->setText(on ? "Hide" : "Show");
	});
	auto *keyRow = new QHBoxLayout;
	keyRow->addWidget(m_key, 1);
	keyRow->addWidget(show);
	m_serverHint = hint("");
	form->addRow("Platform", m_platform);
	form->addRow("Server", m_server);
	form->addRow("Stream key", keyRow);
	form->addRow("", m_serverHint);
	form->addRow("", hint("Your key is encrypted by Windows and never leaves this PC except to your platform."));
	m_autoHeal = new QCheckBox("Keep me live when the internet struggles");
	m_autoHeal->setChecked(s.value("stream/autoHeal", true).toBool());
	form->addRow("", m_autoHeal);
	form->addRow("", hint("When frames start dropping, Zunimi lowers the bitrate step by step (never below 40%) "
			      "instead of letting the stream stutter, then raises it again once your connection is steady. "
			      "A recording made at the same time follows the same bitrate."));
	m_platform->setCurrentIndex(std::max(0, m_platform->findData(s.value("stream/platform", "twitch"))));
	connect(m_platform, &QComboBox::currentIndexChanged, this, &SettingsDialog::platformChanged);
	platformChanged();
	if (s.has("stream/server"))
		m_server->setText(s.value("stream/server").toString());
	return page;
}

void SettingsDialog::platformChanged()
{
	const Platform &p = PLATFORMS[std::max(0, m_platform->currentIndex())];
	m_server->setText(p.server);
	m_serverHint->setText(p.hint);
}

QWidget *SettingsDialog::videoTab()
{
	Settings &s = Settings::get();
	const Hardware &hw = m_core->hardware();
	const VideoPlan &plan = m_core->plan();
	auto *page = new QWidget;
	auto *form = new QFormLayout(page);
	form->setContentsMargins(18, 18, 18, 18);
	form->setVerticalSpacing(12);
	m_profile = new QComboBox;
	m_profile->addItem("Potato  (weak PCs: 720p 30 fps)", "potato");
	m_profile->addItem("Balanced", "balanced");
	m_profile->addItem("Ultra  (strong PCs: 1080p 60 fps)", "ultra");
	m_profile->addItem("Custom", "custom");
	m_profileHint = hint("Auto-Tune picked " + profileName(pickProfile(hw)) + " for this PC. Change anything you like.");
	m_resolution = new QComboBox;
	for (const QSize &size : {QSize(1280, 720), QSize(1600, 900), QSize(1920, 1080), QSize(2560, 1440)})
		m_resolution->addItem(QString("%1 x %2").arg(size.width()).arg(size.height()), size);
	m_fps = new QComboBox;
	m_fps->addItem("30 fps", 30);
	m_fps->addItem("60 fps", 60);
	m_bitrate = new QSpinBox;
	m_bitrate->setRange(800, 20000);
	m_bitrate->setSingleStep(500);
	m_bitrate->setSuffix(" kbps");
	m_encoder = new QComboBox;
	if (hw.nvenc)
		m_encoder->addItem("NVIDIA NVENC (graphics card, recommended)", "obs_nvenc_h264_tex");
	if (hw.amf)
		m_encoder->addItem("AMD AMF (graphics card, recommended)", "h264_texture_amf");
	if (hw.qsv)
		m_encoder->addItem("Intel QuickSync (recommended)", "obs_qsv11_v2");
	m_encoder->addItem(hw.anyHardwareEncoder() ? "x264 (CPU, heavier)" : "x264 (CPU)", "obs_x264");
	m_pausePreview = new QCheckBox("Pause the preview while live or recording");
	m_pausePreview->setChecked(s.value("preview/pauseWhileLive", s.value("video/profile") == "potato").toBool());

	m_filling = true;
	m_profile->setCurrentIndex(std::max(0, m_profile->findData(s.value("video/profile", "balanced"))));
	m_resolution->setCurrentIndex(std::max(0, m_resolution->findData(QSize(plan.width, plan.height))));
	m_fps->setCurrentIndex(std::max(0, m_fps->findData(plan.fps)));
	m_bitrate->setValue(plan.bitrate);
	m_encoder->setCurrentIndex(std::max(0, m_encoder->findData(plan.encoder)));
	m_filling = false;

	connect(m_profile, &QComboBox::currentIndexChanged, this, &SettingsDialog::profileChanged);
	for (QComboBox *box : {m_resolution, m_fps, m_encoder})
		connect(box, &QComboBox::currentIndexChanged, this, &SettingsDialog::markCustom);
	connect(m_bitrate, &QSpinBox::valueChanged, this, &SettingsDialog::markCustom);

	form->addRow("Profile", m_profile);
	form->addRow("", m_profileHint);
	form->addRow("Resolution", m_resolution);
	form->addRow("Frame rate", m_fps);
	form->addRow("Bitrate", m_bitrate);
	form->addRow("", hint("Twitch: up to 6000. YouTube: 4500-9000 for 1080p. Slow upload? Use about 70% of it."));
	form->addRow("Encoder", m_encoder);
	form->addRow("", m_pausePreview);
	form->addRow("", hint("Pausing the preview frees your graphics card for the game. The stream is not affected."));
	return page;
}

void SettingsDialog::profileChanged()
{
	const QString key = m_profile->currentData().toString();
	if (key == "custom")
		return;
	const VideoPlan plan = planFor(profileFromKey(key), m_core->hardware());
	m_filling = true;
	m_resolution->setCurrentIndex(std::max(0, m_resolution->findData(QSize(plan.width, plan.height))));
	m_fps->setCurrentIndex(std::max(0, m_fps->findData(plan.fps)));
	m_bitrate->setValue(plan.bitrate);
	m_encoder->setCurrentIndex(std::max(0, m_encoder->findData(plan.encoder)));
	m_pausePreview->setChecked(key == "potato");
	m_filling = false;
}

void SettingsDialog::markCustom()
{
	if (!m_filling)
		m_profile->setCurrentIndex(m_profile->findData("custom"));
}

QWidget *SettingsDialog::recordingTab()
{
	auto *page = new QWidget;
	auto *form = new QFormLayout(page);
	form->setContentsMargins(18, 18, 18, 18);
	m_folder = new QLineEdit(Settings::get().value("record/folder", defaultFolder()).toString());
	auto *browse = new QPushButton("Choose...");
	connect(browse, &QPushButton::clicked, this, [this] {
		const QString dir = QFileDialog::getExistingDirectory(this, "Save recordings to", m_folder->text());
		if (!dir.isEmpty())
			m_folder->setText(dir);
	});
	auto *row = new QHBoxLayout;
	row->addWidget(m_folder, 1);
	row->addWidget(browse);
	form->addRow("Save recordings to", row);
	form->addRow("", hint("Recordings are MP4 files that survive a crash or power cut. They use the same quality as "
			      "the stream, so recording while live costs almost nothing extra."));
	m_replaySeconds = new QSpinBox;
	m_replaySeconds->setRange(5, 600);
	m_replaySeconds->setSingleStep(15);
	m_replaySeconds->setSuffix(" seconds");
	m_replaySeconds->setValue(Settings::get().value("replay/seconds", 30).toInt());
	m_replayAuto = new QCheckBox("Turn the replay buffer on when Zunimi Studio starts");
	m_replayAuto->setChecked(Settings::get().value("replay/autoStart", false).toBool());
	form->addRow("Replay length", m_replaySeconds);
	form->addRow("", m_replayAuto);
	form->addRow("", hint("The replay buffer keeps the last moments in memory. Save replay (button, tray or "
			      "hotkey) writes them to an MP4 in the same folder. While it is on, the encoder runs, "
			      "so it costs a little GPU or CPU even when you are not live."));
	return page;
}

QWidget *SettingsDialog::hotkeysTab()
{
	auto *page = new QWidget;
	auto *outer = new QVBoxLayout(page);
	outer->setContentsMargins(18, 18, 18, 18);
	outer->addWidget(hint("These work everywhere, even while a game is fullscreen. Click a box and press the "
			      "keys, for example Ctrl+F9. Use a combination games don't need."));
	auto *area = new QScrollArea;
	area->setWidgetResizable(true);
	area->setFrameShape(QFrame::NoFrame);
	area->setObjectName("clear");
	auto *inner = new QWidget;
	inner->setObjectName("clearInner");
	auto *form = new QFormLayout(inner);
	form->setVerticalSpacing(8);
	for (const Hotkeys::Action &a : Hotkeys::actions()) {
		auto *edit = new QKeySequenceEdit(QKeySequence(Settings::get().value("hotkeys/" + a.id).toString()));
		edit->setMaximumSequenceLength(1);
		auto *clear = new QPushButton("Clear");
		connect(clear, &QPushButton::clicked, edit, &QKeySequenceEdit::clear);
		auto *row = new QHBoxLayout;
		row->addWidget(edit, 1);
		row->addWidget(clear);
		form->addRow(a.label, row);
		m_keys.append({a.id, edit});
	}
	area->setWidget(inner);
	outer->addWidget(area, 1);
	return page;
}

QWidget *SettingsDialog::windowTab()
{
	Settings &s = Settings::get();
	auto *page = new QWidget;
	auto *form = new QFormLayout(page);
	form->setContentsMargins(18, 18, 18, 18);
	form->setVerticalSpacing(12);
	m_mode = new QComboBox;
	for (const QString &mode : WindowModes::modes())
		m_mode->addItem(WindowModes::label(mode), mode);
	m_mode->setCurrentIndex(std::max(0, m_mode->findData(m_modes->mode())));
	m_closeTray = new QCheckBox("Closing the window keeps Zunimi Studio in the tray");
	m_closeTray->setChecked(s.value("tray/close", true).toBool());
	m_escTray = new QCheckBox("Esc hides it to the tray");
	m_escTray->setChecked(s.value("tray/esc", true).toBool());
	form->addRow("Window mode", m_mode);
	form->addRow("", hint("Borderless fills the screen above the taskbar; Fullscreen covers it all. F11 switches "
			      "fullscreen on and off any time. Without a title bar, touch the top edge for the window "
			      "buttons."));
	form->addRow("", m_closeTray);
	form->addRow("", m_escTray);
	form->addRow("", hint("Esc first leaves a text box or fullscreen. Quit from the tray icon. Quitting while "
			      "live or recording always asks first."));
	return page;
}

QWidget *SettingsDialog::aboutTab()
{
	const Hardware &hw = m_core->hardware();
	auto *page = new QWidget;
	auto *layout = new QVBoxLayout(page);
	layout->setContentsMargins(18, 18, 18, 18);
	QStringList encoders;
	if (hw.nvenc)
		encoders << "NVIDIA NVENC";
	if (hw.amf)
		encoders << "AMD AMF";
	if (hw.qsv)
		encoders << "Intel QuickSync";
	encoders << "x264";
	auto *title = new QLabel(QString("%1 %2").arg(ZUNIMI_NAME, ZUNIMI_VERSION));
	title->setObjectName("h2");
	auto *check = new QPushButton("Check for updates");
	check->setCursor(Qt::PointingHandCursor);
	connect(check, &QPushButton::clicked, this, [this] {
		if (checkUpdates)
			checkUpdates();
	});
	auto *head = new QHBoxLayout;
	head->addWidget(title, 1);
	head->addWidget(check);
	layout->addLayout(head);
	m_autoUpdate = new QCheckBox("Check for updates when Zunimi Studio starts");
	m_autoUpdate->setChecked(Settings::get().value("update/auto", true).toBool());
	layout->addWidget(m_autoUpdate);
	layout->addSpacing(8);
	layout->addWidget(hint(QString("Processor threads: %1\nMemory: %2 GB\nGraphics: %3\nEncoders: %4\n"
				       "Auto-Tune choice: %5")
				       .arg(hw.cores)
				       .arg(hw.ramGb, 0, 'f', 1)
				       .arg(hw.gpu.isEmpty() ? "unknown" : hw.gpu)
				       .arg(encoders.join(", "))
				       .arg(profileName(pickProfile(hw)))));
	layout->addWidget(hint("Loaded modules (only what is switched on is loaded): " + m_core->modules().join(", ")));
	layout->addWidget(hint("Built on libobs from the OBS Project. Zunimi Studio is free software under the GPL "
			       "version 2 or later."));
	layout->addStretch();
	return page;
}

void SettingsDialog::save()
{
	Settings &s = Settings::get();
	s.set("stream/platform", m_platform->currentData());
	s.set("stream/server", m_server->text().trimmed());
	s.setSecret("stream/key", m_key->text().trimmed());
	s.set("record/folder", m_folder->text().trimmed().isEmpty() ? defaultFolder() : m_folder->text().trimmed());
	s.set("preview/pauseWhileLive", m_pausePreview->isChecked());
	s.set("tray/close", m_closeTray->isChecked());
	s.set("tray/esc", m_escTray->isChecked());
	s.set("update/auto", m_autoUpdate->isChecked());
	s.set("stream/autoHeal", m_autoHeal->isChecked());
	s.set("replay/seconds", m_replaySeconds->value());
	s.set("replay/autoStart", m_replayAuto->isChecked());
	QStringList bad;
	for (const auto &[id, edit] : m_keys) {
		const QKeySequence keys = edit->keySequence();
		if (!keys.isEmpty() && !Hotkeys::supported(keys))
			bad << keys.toString(QKeySequence::NativeText);
		s.set("hotkeys/" + id, Hotkeys::supported(keys) ? keys.toString(QKeySequence::PortableText) : QString());
	}
	if (!bad.isEmpty())
		QMessageBox::information(this, "Hotkeys", "These keys can't be used as hotkeys, so they were left empty: " +
								  bad.join(", "));
	m_modes->syncTray();
	if (m_mode->currentData().toString() != m_modes->mode())
		m_modes->setMode(m_mode->currentData().toString());

	VideoPlan plan;
	const QSize size = m_resolution->currentData().toSize();
	plan.width = size.width();
	plan.height = size.height();
	plan.fps = m_fps->currentData().toInt();
	plan.bitrate = m_bitrate->value();
	plan.encoder = m_encoder->currentData().toString();
	const VideoPlan &now = m_core->plan();
	const bool changed = plan.width != now.width || plan.height != now.height || plan.fps != now.fps ||
			     plan.bitrate != now.bitrate || plan.encoder != now.encoder;
	if (changed) {
		QString error;
		if (!m_core->applyPlan(plan, &error)) {
			QMessageBox::warning(this, "Settings", "Video settings were not changed: " + error);
			return;
		}
	}
	s.set("video/profile", m_profile->currentData());
	accept();
}
