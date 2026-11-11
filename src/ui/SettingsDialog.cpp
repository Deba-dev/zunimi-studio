#include "ui/SettingsDialog.h"
#include "core/I18n.h"
#include "ui/Theme.h"

#include "core/ObsCore.h"
#include "core/Assist.h"
#include "core/Settings.h"
#include "events/Events.h"
#include "net/Remote.h"
#include "qrcodegen.hpp"
#include "ui/Hotkeys.h"
#include "ui/WindowModes.h"
#include "version.h"

#include <QCheckBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QStringList>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QKeySequenceEdit>
#include <QScrollArea>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QApplication>
#include <QClipboard>
#include <QImage>
#include <QPainter>
#include <QRandomGenerator>
#include <QVBoxLayout>

namespace {
struct Platform {
	const char *key, *label, *server, *hint;
};
const Platform PLATFORMS[] = {
	{"twitch", "Twitch", "rtmp://live.twitch.tv/app", "Key: Twitch Creator Dashboard > Settings > Stream."},
	{"youtube", "YouTube", "rtmps://a.rtmps.youtube.com:443/live2", "Key: YouTube Studio > Go live > Stream key."},
	{"kick", "Kick", "", "Kick gives you a Stream URL and a Stream Key: Dashboard > Settings > Stream URL & Key."},
	{"custom", "Other (RTMP, RTMPS, SRT or RIST)", "",
	 "Paste the server address and key your platform gives you. srt:// and rist:// addresses work too: they "
	 "cope better with a shaky connection."},
	{"whip", "WHIP (WebRTC, lowest delay)", "",
	 "Paste the WHIP address. The \"key\" box is the bearer token, if your service gives you one."},
};

QLabel *hint(const QString &text)
{
	auto *label = new QLabel(text);
	label->setObjectName("hint");
	label->setWordWrap(true);
	return label;
}

// A tab that scrolls when it is taller than the window (longer languages, small screens).
QWidget *scrolled(QWidget *page)
{
	auto *area = new QScrollArea;
	area->setWidgetResizable(true);
	area->setFrameShape(QFrame::NoFrame);
	area->setObjectName("clear");
	page->setObjectName("clearInner");
	area->setWidget(page);
	return area;
}

QString defaultFolder()
{
	return QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + L("/Zunimi Studio");
}
} // namespace

SettingsDialog::SettingsDialog(ObsCore *core, WindowModes *modes, QWidget *parent)
	: QDialog(parent), m_core(core), m_modes(modes)
{
	setWindowTitle(L("Settings"));
	resize(880, 640);
	m_tabs = new QTabWidget;
	m_tabs->addTab(scrolled(streamTab()), L("Stream"));
	m_tabs->addTab(scrolled(videoTab()), L("Video"));
	m_tabs->addTab(scrolled(recordingTab()), L("Recording"));
	m_tabs->addTab(scrolled(audioTab()), L("Audio"));
	m_tabs->addTab(connectionsTab(), L("Connections"));
	m_tabs->addTab(automationTab(), L("Automation"));
	m_tabs->addTab(remoteTab(), L("Remote"));
	m_tabs->addTab(scrolled(hotkeysTab()), L("Hotkeys"));
	m_tabs->addTab(scrolled(windowTab()), L("Window"));
	m_tabs->addTab(scrolled(aboutTab()), L("About this PC"));
	if (Settings::get().value("ui/simple", false).toBool()) // Simple mode: Auto-Tune and the defaults handle these
		for (Tab t : {Video, Audio, Automation, About})
			m_tabs->setTabVisible(int(t), false);
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
		m_platform->addItem(L(p.label), p.key);
	m_server = new QLineEdit(s.value("stream/server").toString());
	m_server->setPlaceholderText("rtmp://...");
	m_key = new QLineEdit(s.secret("stream/key"));
	m_key->setEchoMode(QLineEdit::Password);
	m_key->setPlaceholderText(L("Paste your stream key"));
	auto *show = new QPushButton(L("Show"));
	show->setCheckable(true);
	connect(show, &QPushButton::toggled, this, [this, show](bool on) {
		m_key->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
		show->setText(on ? L("Hide") : L("Show"));
	});
	auto *keyRow = new QHBoxLayout;
	keyRow->addWidget(m_key, 1);
	keyRow->addWidget(show);
	m_serverHint = hint("");
	form->addRow(L("Platform"), m_platform);
	form->addRow(L("Server"), m_server);
	form->addRow(L("Stream key"), keyRow);
	form->addRow("", m_serverHint);
	form->addRow("", hint(L("Your key is encrypted by Windows and never leaves this PC except to your platform.")));
	m_autoHeal = new QCheckBox(L("Keep me live when the internet struggles"));
	m_autoHeal->setChecked(s.value("stream/autoHeal", true).toBool());
	form->addRow("", m_autoHeal);
	form->addRow("", hint(L("When frames start dropping, Zunimi lowers the bitrate step by step (never below 40%) "
			      "instead of letting the stream stutter, then raises it again once your connection is steady. "
			      "A recording made at the same time follows the same bitrate.")));
	m_delay = new QSpinBox;
	m_delay->setRange(0, 600);
	m_delay->setSuffix(" seconds");
	m_delay->setSpecialValueText(L("No delay"));
	m_delay->setValue(s.value("stream/delaySec", 0).toInt());
	form->addRow(L("Stream delay"), m_delay);
	form->addRow("", hint(L("A delay hides what's on your screen from stream snipers. Viewers see everything that "
			      "many seconds later.")));
	m_autoRecord = new QCheckBox(L("Record automatically while live (and stop with the stream)"));
	m_autoRecord->setChecked(s.value("stream/autoRecord", false).toBool());
	form->addRow("", m_autoRecord);
	m_destinations = new QTableWidget(0, 4);
	m_destinations->setHorizontalHeaderLabels({L("On"), L("Name"), L("Server"), L("Stream key")});
	m_destinations->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	m_destinations->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	m_destinations->setMinimumHeight(110);
	auto addDestination = [this](const ObsCore::Destination &d) {
		const int r = m_destinations->rowCount();
		m_destinations->insertRow(r);
		auto *on = new QTableWidgetItem;
		on->setFlags(on->flags() | Qt::ItemIsUserCheckable);
		on->setCheckState(d.on ? Qt::Checked : Qt::Unchecked);
		m_destinations->setItem(r, 0, on);
		m_destinations->setItem(r, 1, new QTableWidgetItem(d.name));
		m_destinations->setItem(r, 2, new QTableWidgetItem(d.server));
		auto *key = new QLineEdit(d.key);
		key->setEchoMode(QLineEdit::Password);
		m_destinations->setCellWidget(r, 3, key);
	};
	for (const ObsCore::Destination &d : ObsCore::destinations())
		addDestination(d);
	auto *addDest = new QPushButton(L("Add a platform"));
	auto *addMenu = new QMenu(addDest);
	for (const Platform &p : PLATFORMS)
		if (QString(p.key) != "whip")
			addMenu->addAction(L(p.label), this, [addDestination, p] { addDestination({QString(p.label).section(' ', 0, 0), p.server, {}, true}); });
	addDest->setMenu(addMenu);
	auto *removeDest = new QPushButton(L("Remove"));
	connect(removeDest, &QPushButton::clicked, this, [this] { m_destinations->removeRow(m_destinations->currentRow()); });
	auto *destButtons = new QHBoxLayout;
	destButtons->addWidget(addDest);
	destButtons->addWidget(removeDest);
	destButtons->addStretch();
	form->addRow(L("Also stream to"), m_destinations);
	form->addRow("", destButtons);
	form->addRow("", hint(L("Multistream: the same picture goes to every platform ticked here at the same time. It "
			      "costs upload speed (one stream's bitrate per platform), not CPU.")));
	m_streamTrack = new QComboBox;
	for (int i = 1; i <= 6; i++)
		m_streamTrack->addItem(QString(L("Track %1")).arg(i), i);
	m_streamTrack->setCurrentIndex(std::clamp(s.value("stream/track", 1).toInt(), 1, 6) - 1);
	form->addRow(L("Stream sound from"), m_streamTrack);
	form->addRow("", hint(L("Every source is on all tracks unless you change it in its Sound settings. Leave "
			      "music off a track to keep it out of the stream (or out of the recording).")));
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
	m_serverHint->setText(L(p.hint));
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
	m_profile->addItem(L("Potato  (weak PCs: 720p 30 fps)"), "potato");
	m_profile->addItem(L("Balanced"), "balanced");
	m_profile->addItem(L("Ultra  (strong PCs: 1080p 60 fps)"), "ultra");
	m_profile->addItem(L("Custom"), "custom");
	m_profileHint = hint(L("Auto-Tune picked ") + profileName(pickProfile(hw)) + L(" for this PC. Change anything you like."));
	m_resolution = new QComboBox;
	for (const QSize &size : {QSize(1280, 720), QSize(1600, 900), QSize(1920, 1080), QSize(2560, 1440)})
		m_resolution->addItem(QString("%1 x %2").arg(size.width()).arg(size.height()), size);
	m_fps = new QComboBox;
	m_fps->addItem(L("30 fps"), 30);
	m_fps->addItem(L("60 fps"), 60);
	m_bitrate = new QSpinBox;
	m_bitrate->setRange(800, 20000);
	m_bitrate->setSingleStep(500);
	m_bitrate->setSuffix(" kbps");
	m_encoder = new QComboBox;
	if (hw.nvenc)
		m_encoder->addItem(L("NVIDIA NVENC (graphics card, recommended)"), "obs_nvenc_h264_tex");
	if (hw.amf)
		m_encoder->addItem(L("AMD AMF (graphics card, recommended)"), "h264_texture_amf");
	if (hw.qsv)
		m_encoder->addItem(L("Intel QuickSync (recommended)"), "obs_qsv11_v2");
	m_encoder->addItem(hw.anyHardwareEncoder() ? L("x264 (CPU, heavier)") : L("x264 (CPU)"), "obs_x264");
	m_pausePreview = new QCheckBox(L("Pause the preview while live or recording"));
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

	form->addRow(L("Profile"), m_profile);
	form->addRow("", m_profileHint);
	form->addRow(L("Resolution"), m_resolution);
	form->addRow(L("Frame rate"), m_fps);
	form->addRow(L("Bitrate"), m_bitrate);
	form->addRow("", hint(L("Twitch: up to 6000. YouTube: 4500-9000 for 1080p. Slow upload? Use about 70% of it.")));
	form->addRow(L("Encoder"), m_encoder);
	m_codec = new QComboBox;
	m_rate = new QComboBox;
	m_rate->addItem(L("Constant bitrate (best for streaming)"), "CBR");
	m_rate->addItem(L("Variable bitrate"), "VBR");
	m_rate->addItem(L("Constant quality (recording only)"), "CQP");
	m_rate->setCurrentIndex(std::max(0, m_rate->findData(s.value("video/rateControl", "CBR"))));
	m_quality = new QSpinBox;
	m_quality->setRange(0, 51);
	m_quality->setValue(s.value("video/quality", 23).toInt());
	m_quality->setToolTip(L("Lower is better and bigger. 18-23 looks great."));
	m_preset = new QComboBox;
	m_preset->addItem(L("Faster (lighter on the PC)"), 0);
	m_preset->addItem(L("Balanced"), 1);
	m_preset->addItem(L("Better quality (heavier)"), 2);
	m_preset->setCurrentIndex(std::clamp(s.value("video/preset", 1).toInt(), 0, 2));
	m_keyint = new QSpinBox;
	m_keyint->setRange(1, 10);
	m_keyint->setSuffix(" s");
	m_keyint->setValue(s.value("video/keyint", 2).toInt());
	auto fillCodecs = [this] {
		const QString family = m_encoder->currentData().toString();
		const QString want = m_codec->count() ? m_codec->currentData().toString() : Settings::get().value("video/codec", "h264").toString();
		m_codec->clear();
		for (const char *c : {"h264", "hevc", "av1"})
			if (codecAvailable(family, c))
				m_codec->addItem(codecLabel(c), c);
		m_codec->setCurrentIndex(std::max(0, m_codec->findData(want)));
	};
	fillCodecs();
	connect(m_encoder, &QComboBox::currentIndexChanged, this, fillCodecs);
	auto showQuality = [this] { m_quality->setEnabled(m_rate->currentData() == "CQP"); };
	showQuality();
	connect(m_rate, &QComboBox::currentIndexChanged, this, showQuality);
	form->addRow(L("Codec"), m_codec);
	form->addRow("", hint(L("H.264 works everywhere. HEVC and AV1 look better at the same bitrate; YouTube takes "
			      "both, Twitch is still testing them. Only codecs your graphics card can encode are listed.")));
	form->addRow(L("Rate control"), m_rate);
	form->addRow(L("Quality (constant quality)"), m_quality);
	form->addRow(L("Speed vs quality"), m_preset);
	form->addRow(L("Keyframe every"), m_keyint);
	form->addRow("", m_pausePreview);
	form->addRow("", hint(L("Pausing the preview frees your graphics card for the game. The stream is not affected.")));
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
	auto *browse = new QPushButton(L("Choose..."));
	connect(browse, &QPushButton::clicked, this, [this] {
		const QString dir = QFileDialog::getExistingDirectory(this, L("Save recordings to"), m_folder->text());
		if (!dir.isEmpty())
			m_folder->setText(dir);
	});
	auto *row = new QHBoxLayout;
	row->addWidget(m_folder, 1);
	row->addWidget(browse);
	form->addRow(L("Save recordings to"), row);
	form->addRow("", hint(L("\"Same as the stream\" costs almost nothing extra while live. A separate encode "
			      "looks better but uses more of your graphics card.")));
	Settings &rs = Settings::get();
	m_format = new QComboBox;
	m_format->addItem(L("MP4 (safe: survives a crash)"), "hybrid_mp4");
	m_format->addItem(L("MOV (safe: survives a crash)"), "hybrid_mov");
	m_format->addItem(L("MKV (safe, remux to MP4 for editors)"), "mkv");
	m_format->addItem(L("MP4 (classic; lost if the PC crashes)"), "mp4");
	m_format->addItem(L("MOV (classic)"), "mov");
	m_format->addItem("FLV", "flv");
	m_format->addItem(L("MPEG-TS"), "ts");
	m_format->setCurrentIndex(std::max(0, m_format->findData(rs.value("record/format", "hybrid_mp4"))));
	m_recQuality = new QComboBox;
	m_recQuality->addItem(L("Same as the stream (lightest)"), "same");
	m_recQuality->addItem(L("High quality (separate encode)"), "high");
	m_recQuality->addItem(L("Near lossless (big files, for editing)"), "lossless");
	m_recQuality->addItem(L("Smaller files"), "small");
	m_recQuality->setCurrentIndex(std::max(0, m_recQuality->findData(rs.value("record/quality", "same"))));
	m_recCodec = new QComboBox;
	for (const char *c : {"h264", "hevc", "av1"})
		if (codecAvailable(m_core->plan().encoder, c))
			m_recCodec->addItem(codecLabel(c), c);
	m_recCodec->setCurrentIndex(std::max(0, m_recCodec->findData(rs.value("record/codec", "h264"))));
	auto *tracks = new QHBoxLayout;
	const uint32_t mask = rs.value("record/tracks", 1).toUInt();
	for (int i = 0; i < 6; i++) {
		m_tracks[i] = new QCheckBox(QString::number(i + 1));
		m_tracks[i]->setChecked(mask & (1u << i));
		tracks->addWidget(m_tracks[i]);
	}
	tracks->addStretch();
	m_splitMinutes = new QSpinBox;
	m_splitMinutes->setRange(0, 1440);
	m_splitMinutes->setSuffix(" min");
	m_splitMinutes->setSpecialValueText(L("Never"));
	m_splitMinutes->setValue(rs.value("record/splitMinutes", 0).toInt());
	m_splitMb = new QSpinBox;
	m_splitMb->setRange(0, 1000000);
	m_splitMb->setSingleStep(1024);
	m_splitMb->setSuffix(" MB");
	m_splitMb->setSpecialValueText(L("Any size"));
	m_splitMb->setValue(rs.value("record/splitMb", 0).toInt());
	auto *split = new QHBoxLayout;
	split->addWidget(m_splitMinutes);
	split->addWidget(m_splitMb);
	m_nameFormat = new QLineEdit(rs.value("record/nameFormat", "yyyy-MM-dd hh-mm-ss").toString());
	form->addRow(L("Format"), m_format);
	form->addRow(L("Quality"), m_recQuality);
	form->addRow(L("Codec (own quality only)"), m_recCodec);
	form->addRow(L("Sound tracks"), tracks);
	form->addRow("", hint(L("Each ticked track is saved as its own sound in the file (pick which sources go where "
			      "in their Sound settings). Editors can then turn the game down without touching your voice.")));
	form->addRow(L("New file every"), split);
	form->addRow(L("File name"), m_nameFormat);
	form->addRow("", hint("yyyy year, MM month, dd day, hh hour, mm minute, ss second; {scene} and {profile} "
			      "work too."));
	m_replaySeconds = new QSpinBox;
	m_replaySeconds->setRange(5, 600);
	m_replaySeconds->setSingleStep(15);
	m_replaySeconds->setSuffix(" seconds");
	m_replaySeconds->setValue(Settings::get().value("replay/seconds", 30).toInt());
	m_replayAuto = new QCheckBox(L("Turn the replay buffer on when Zunimi Studio starts"));
	m_replayAuto->setChecked(Settings::get().value("replay/autoStart", false).toBool());
	form->addRow(L("Replay length"), m_replaySeconds);
	form->addRow("", m_replayAuto);
	form->addRow("", hint(L("The replay buffer keeps the last moments in memory. Save replay (button, tray or "
			      "hotkey) writes them to an MP4 in the same folder. While it is on, the encoder runs, "
			      "so it costs a little GPU or CPU even when you are not live.")));
	return page;
}

QWidget *SettingsDialog::audioTab()
{
	Settings &s = Settings::get();
	auto *page = new QWidget;
	auto *form = new QFormLayout(page);
	form->setContentsMargins(18, 18, 18, 18);
	form->setVerticalSpacing(12);
	m_monitor = new QComboBox;
	const QString current = s.value("audio/monitorId", "default").toString();
	m_monitor->addItem(L("Windows default"), QStringList{"Default", "default"});
	for (const auto &[name, id] : ObsCore::monitoringDevices())
		if (id != "default")
			m_monitor->addItem(name, QStringList{name, id});
	for (int i = 0; i < m_monitor->count(); i++)
		if (m_monitor->itemData(i).toStringList().value(1) == current)
			m_monitor->setCurrentIndex(i);
	form->addRow(L("Headphones (listen)"), m_monitor);
	form->addRow("", hint(L("Where \"Listen in headphones\" plays, set per source in its Sound settings (the ⋯ "
			      "button in the mixer).")));
	m_audioKbps = new QSpinBox;
	m_audioKbps->setRange(64, 320);
	m_audioKbps->setSingleStep(32);
	m_audioKbps->setSuffix(" kbps");
	m_audioKbps->setValue(s.value("audio/bitrate", 160).toInt());
	form->addRow(L("Sound quality"), m_audioKbps);
	form->addRow("", hint(L("160 kbps is plenty for streams. Music streams can use 320.")));
	form->addRow("", hint(L("Push to talk and push to mute are in Hotkeys.")));
	return page;
}

QWidget *SettingsDialog::connectionsTab()
{
	Settings &s = Settings::get();
	auto *page = new QWidget;
	auto *outer = new QVBoxLayout(page);
	outer->setContentsMargins(0, 0, 0, 0);
	auto *area = new QScrollArea;
	area->setWidgetResizable(true);
	area->setFrameShape(QFrame::NoFrame);
	area->setObjectName("clear");
	auto *inner = new QWidget;
	inner->setObjectName("clearInner");
	auto *form = new QFormLayout(inner);
	form->setContentsMargins(18, 18, 18, 18);
	form->setVerticalSpacing(10);
	auto link = [](const QString &text, const QString &url) {
		auto *l = new QLabel(QString(L("<a href='%1'>%2</a>")).arg(url, text));
		l->setOpenExternalLinks(true);
		l->setWordWrap(true);
		return l;
	};
	form->addRow("", hint(L("Chat, alerts, goals and widgets read from these. Nothing here is needed to stream.")));

	m_twitchChannel = new QLineEdit(s.value("events/twitchChannel").toString());
	m_twitchChannel->setPlaceholderText(L("your Twitch name"));
	form->addRow(L("Twitch channel"), m_twitchChannel);
	form->addRow("", hint(L("Chat, subs, gift subs, bits and raids work with just the name.")));
	m_twitchLogin = new QLabel;
	auto showLogin = [this] {
		const QString who = Settings::get().value("events/twitchLogin").toString();
		m_twitchLogin->setText(Settings::get().secret("events/twitchToken").isEmpty() ? L("Not logged in")
											     : L("Logged in as ") + who);
	};
	showLogin();
	auto *login = new QPushButton(L("Log in to Twitch"));
	auto *logout = new QPushButton(L("Log out"));
	connect(login, &QPushButton::clicked, this, [this, login, showLogin] {
		login->setEnabled(false);
		TwitchFollows::login(
			[this](const QString &code, const QString &url) {
				m_twitchLogin->setText(QString(L("Enter code <b>%1</b> at %2 (opened in your browser)")).arg(code, url));
			},
			[this, login, showLogin](const QString &error) {
				login->setEnabled(true);
				if (!error.isEmpty())
					m_twitchLogin->setText(error);
				else {
					showLogin();
					if (m_twitchChannel->text().isEmpty())
						m_twitchChannel->setText(Settings::get().value("events/twitchChannel").toString());
				}
			});
	});
	connect(logout, &QPushButton::clicked, this, [showLogin] {
		Settings::get().setSecret("events/twitchToken", {});
		Settings::get().setSecret("events/twitchRefresh", {});
		showLogin();
	});
	auto *loginRow = new QHBoxLayout;
	loginRow->addWidget(m_twitchLogin, 1);
	loginRow->addWidget(login);
	loginRow->addWidget(logout);
	form->addRow(L("Twitch login"), loginRow);
	form->addRow("", hint(L("Only needed for follow alerts and for typing in chat from here.")));

	m_youtube = new QLineEdit(s.value("events/youtube").toString());
	m_youtube->setPlaceholderText(L("@yourchannel, or a stream link"));
	form->addRow(L("YouTube"), m_youtube);
	form->addRow("", hint(L("Chat, Super Chats, Super Stickers, memberships and gifted memberships, with no login. "
			      "With a channel, Zunimi finds the stream as soon as you go live.")));
	m_kick = new QLineEdit(s.value("events/kickChannel").toString());
	m_kick->setPlaceholderText(L("your Kick name"));
	form->addRow(L("Kick"), m_kick);
	m_kickRoom = new QLineEdit(s.value("events/kickRoom").toString());
	m_kickRoom->setPlaceholderText(L("found by itself; paste it only if Kick blocks the lookup"));
	form->addRow(L("Kick chat room number"), m_kickRoom);

	m_seJwt = new QLineEdit(s.secret("events/seJwt"));
	m_seJwt->setEchoMode(QLineEdit::Password);
	m_seJwt->setPlaceholderText(L("JWT token"));
	form->addRow(L("StreamElements"), m_seJwt);
	form->addRow("", link(L("Find it: StreamElements > your account > Channels > Show secrets (JWT Token)"),
			      "https://streamelements.com/dashboard/account/channels"));
	m_slToken = new QLineEdit(s.secret("events/slToken"));
	m_slToken->setEchoMode(QLineEdit::Password);
	m_slToken->setPlaceholderText(L("Socket API token"));
	form->addRow(L("Streamlabs"), m_slToken);
	form->addRow("", link(L("Find it: Streamlabs > Settings > API Settings > API Tokens (Socket API Token)"),
			      "https://streamlabs.com/dashboard#/settings/api-settings"));
	form->addRow("", hint(L("Tips sent through StreamElements or Streamlabs (UPI, cards, PayPal) show as alerts here, "
			      "with your own designs.")));

	// Kick follows and Kicks, and Ko-fi, only send webhooks: they reach this PC through the ZunimiLabs relay.
	m_kickRelay = new QLabel;
	m_kickRelay->setWordWrap(true);
	auto showKick = [this] {
		const QString name = Settings::get().value("events/kickRelayName").toString();
		m_kickRelay->setText(name.isEmpty() ? L("Not connected") : QString(L("Connected as %1")).arg(name));
	};
	showKick();
	connect(&EventHub::get(), &EventHub::statusChanged, m_kickRelay, [showKick](const QString &service) {
		if (service == "relay")
			showKick();
	});
	auto *kickGo = new QPushButton(L("Connect Kick"));
	auto *kickOff = new QPushButton(L("Disconnect"));
	connect(kickGo, &QPushButton::clicked, this, [this, kickGo] {
		kickGo->setEnabled(false);
		m_kickRelay->setText(L("Opening Kick in your browser..."));
		RelayEvents::kickLogin([this, kickGo](const QString &error) {
			kickGo->setEnabled(true);
			m_kickRelay->setText(error.isEmpty() ? L("Log in on the Kick page in your browser, then come back here.") : error);
		});
	});
	connect(kickOff, &QPushButton::clicked, this, [showKick] {
		RelayEvents::kickLogout();
		showKick();
		EventHub::get().restart();
	});
	auto *kickRow = new QHBoxLayout;
	kickRow->addWidget(m_kickRelay, 1);
	kickRow->addWidget(kickGo);
	kickRow->addWidget(kickOff);
	form->addRow(L("Kick alerts"), kickRow);
	form->addRow("", hint(L("Follows, subs, gifted subs and Kicks from your Kick channel, through the ZunimiLabs relay "
			      "(Kick only sends these to a web address). Kick chat works without this.")));

	m_kofiLink = new QLineEdit(RelayEvents::kofiLink());
	m_kofiLink->setReadOnly(true);
	m_kofiLink->setPlaceholderText(L("press Get my link"));
	auto *kofiGet = new QPushButton(RelayEvents::hasChannel() ? L("Copy") : L("Get my link"));
	connect(kofiGet, &QPushButton::clicked, this, [this, kofiGet] {
		kofiGet->setEnabled(false);
		RelayEvents::ensureChannel([this, kofiGet](const QString &error) {
			kofiGet->setEnabled(true);
			if (!error.isEmpty()) {
				m_kofiLink->setText(error);
				return;
			}
			m_kofiLink->setText(RelayEvents::kofiLink());
			QApplication::clipboard()->setText(RelayEvents::kofiLink());
			kofiGet->setText(L("Copied"));
		});
	});
	auto *kofiRow = new QHBoxLayout;
	kofiRow->addWidget(m_kofiLink, 1);
	kofiRow->addWidget(kofiGet);
	form->addRow(L("Ko-fi webhook link"), kofiRow);
	m_kofiToken = new QLineEdit(s.secret("events/kofiToken"));
	m_kofiToken->setEchoMode(QLineEdit::Password);
	m_kofiToken->setPlaceholderText(L("Verification Token from Ko-fi"));
	form->addRow(L("Ko-fi token"), m_kofiToken);
	form->addRow("", link(L("On Ko-fi: More > API. Paste the link above as the Webhook URL, then copy the Verification "
				"Token here. Tips and memberships show as alerts."),
			      "https://ko-fi.com/manage/webhooks"));

	m_currency = new QComboBox;
	for (const char *c : {"INR", "USD", "EUR", "GBP", "BRL", "CAD", "AUD", "JPY"})
		m_currency->addItem(c, c);
	m_currency->setCurrentIndex(std::max(0, m_currency->findData(s.value("events/currency", "INR"))));
	form->addRow(L("Main currency"), m_currency);
	form->addRow("", hint(L("Tip goals and tip totals count this currency.")));

	m_twitchClient = new QLineEdit(s.value("events/twitchClientId").toString());
	m_twitchClient->setPlaceholderText(TwitchFollows::clientId().isEmpty() ? L("needed for the Twitch login") : L("built in"));
	form->addRow(L("Advanced: Twitch app id"), m_twitchClient);
	form->addRow("", hint(L("Make one at dev.twitch.tv > Applications (client type: Public, redirect "
			      "http://localhost). Leave empty to use Zunimi's own.")));
	area->setWidget(inner);
	outer->addWidget(area);
	return page;
}

QWidget *SettingsDialog::automationTab()
{
	Settings &s = Settings::get();
	auto *page = new QWidget;
	auto *outer = new QVBoxLayout(page);
	outer->setContentsMargins(0, 0, 0, 0);
	auto *area = new QScrollArea;
	area->setWidgetResizable(true);
	area->setFrameShape(QFrame::NoFrame);
	area->setObjectName("clear");
	auto *inner = new QWidget;
	inner->setObjectName("clearInner");
	auto *form = new QFormLayout(inner);
	form->setContentsMargins(18, 18, 18, 18);
	form->setVerticalSpacing(10);

	m_autoOn = new QCheckBox(L("Switch scenes by the app in front"));
	m_autoOn->setChecked(s.value("auto/on", false).toBool());
	form->addRow("", m_autoOn);
	m_rules = new QTableWidget(0, 2);
	m_rules->setHorizontalHeaderLabels({L("When this app is in front"), L("Switch to scene")});
	m_rules->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	m_rules->setMinimumHeight(130);
	const QJsonArray rules = QJsonDocument::fromJson(s.value("auto/rules").toByteArray()).array();
	auto addRule = [this](const QString &app, const QString &scene) {
		const int r = m_rules->rowCount();
		m_rules->insertRow(r);
		m_rules->setItem(r, 0, new QTableWidgetItem(app));
		auto *box = new QComboBox;
		box->addItems(m_core->scenes());
		box->setCurrentText(scene);
		m_rules->setCellWidget(r, 1, box);
	};
	for (const QJsonValue &v : rules)
		addRule(v.toObject().value("app").toString(), v.toObject().value("scene").toString());
	auto *add = new QPushButton(L("Add an open app..."));
	connect(add, &QPushButton::clicked, this, [this, addRule] {
		bool ok = false;
		const QString app = QInputDialog::getItem(this, L("Pick an app"), L("Apps with a window open now (start your game first):"),
							  Assist::runningApps(), 0, true, &ok);
		if (ok && !app.trimmed().isEmpty())
			addRule(app.trimmed(), m_core->currentScene());
	});
	auto *remove = new QPushButton(L("Remove"));
	connect(remove, &QPushButton::clicked, this, [this] { m_rules->removeRow(m_rules->currentRow()); });
	auto *ruleButtons = new QHBoxLayout;
	ruleButtons->addWidget(add);
	ruleButtons->addWidget(remove);
	ruleButtons->addStretch();
	form->addRow(m_rules);
	form->addRow(ruleButtons);
	form->addRow("", hint(L("For example VALORANT-Win64-Shipping.exe switches to \"Gaming\", Discord.exe to \"Just "
			      "chatting\". It only switches when the app in front changes, so your own switching sticks.")));

	m_privacyOn = new QCheckBox(L("Privacy guard: hide screen captures while a private window is in front"));
	m_privacyOn->setChecked(s.value("privacy/on", true).toBool());
	m_privacyLive = new QCheckBox(L("Only while live or recording"));
	m_privacyLive->setChecked(s.value("privacy/onlyLive", true).toBool());
	m_patterns = new QPlainTextEdit(s.value("privacy/patterns").toStringList().join("\n"));
	m_patterns->setPlaceholderText(L("password\nbitwarden\nstream key\nphonepe\npaytm\nwhatsapp\n(one word per line; empty uses these)"));
	m_patterns->setMaximumHeight(110);
	form->addRow("", m_privacyOn);
	form->addRow("", m_privacyLive);
	form->addRow(L("Private when the title has"), m_patterns);
	form->addRow("", hint(L("Screen and window captures are hidden until you switch away, then come back by themselves. "
			      "Your game capture stays.")));

	m_micMinutes = new QSpinBox;
	m_micMinutes->setRange(0, 60);
	m_micMinutes->setSuffix(" min");
	m_micMinutes->setSpecialValueText(L("Never"));
	m_micMinutes->setValue(s.value("alarms/micMinutes", 3).toInt());
	form->addRow(L("Warn when the mic is silent for"), m_micMinutes);
	m_blackAlarm = new QCheckBox(L("Warn when the stream goes black"));
	m_blackAlarm->setChecked(s.value("alarms/black", true).toBool());
	form->addRow("", m_blackAlarm);
	m_preflight = new QCheckBox(L("Check my setup before going live (empty scene, muted mic, disk space...)"));
	m_preflight->setChecked(s.value("preflight/on", true).toBool());
	form->addRow("", m_preflight);
	m_report = new QCheckBox(L("Show how the stream went when it ends"));
	m_report->setChecked(s.value("report/show", true).toBool());
	form->addRow("", m_report);
	area->setWidget(inner);
	outer->addWidget(area);
	return page;
}

QWidget *SettingsDialog::hotkeysTab()
{
	auto *page = new QWidget;
	auto *outer = new QVBoxLayout(page);
	outer->setContentsMargins(18, 18, 18, 18);
	outer->addWidget(hint(L("These work everywhere, even while a game is fullscreen. Click a box and press the "
			      "keys, for example Ctrl+F9. Use a combination games don't need.")));
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
		auto *clear = new QPushButton(L("Clear"));
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

namespace {
// A QR code as a picture, black on white with the quiet border scanners need.
QPixmap qrPixmap(const QString &text, int size)
{
	using qrcodegen::QrCode;
	const QrCode qr = QrCode::encodeText(text.toUtf8().constData(), QrCode::Ecc::MEDIUM);
	const int n = qr.getSize(), border = 4, cells = n + 2 * border;
	const int scale = std::max(1, size / cells);
	QImage img(cells * scale, cells * scale, QImage::Format_RGB32);
	img.fill(Qt::white);
	QPainter p(&img);
	for (int y = 0; y < n; ++y)
		for (int x = 0; x < n; ++x)
			if (qr.getModule(x, y))
				p.fillRect((x + border) * scale, (y + border) * scale, scale, scale, Qt::black);
	p.end();
	return QPixmap::fromImage(img);
}
} // namespace

QWidget *SettingsDialog::remoteTab()
{
	Settings &s = Settings::get();
	auto *page = new QWidget;
	auto *outer = new QVBoxLayout(page);
	outer->setContentsMargins(0, 0, 0, 0);
	auto *area = new QScrollArea;
	area->setWidgetResizable(true);
	area->setFrameShape(QFrame::NoFrame);
	area->setObjectName("clear");
	auto *inner = new QWidget;
	inner->setObjectName("clearInner");
	auto *form = new QFormLayout(inner);
	form->setContentsMargins(18, 18, 18, 18);
	form->setVerticalSpacing(10);

	m_remoteOn = new QCheckBox(L("Let a Stream Deck, Touch Portal or my phone control Zunimi"));
	m_remoteOn->setChecked(s.value("remote/on", false).toBool());
	form->addRow("", m_remoteOn);
	form->addRow("", hint("Zunimi speaks the same remote language as OBS Studio (obs-websocket 5), so anything "
			      "made for OBS works: the Stream Deck \"OBS Studio\" plugin, Touch Portal, Loupedeck, "
			      "Streamer.bot, Mix It Up."));
	m_remoteLan = new QCheckBox(L("Allow phones and PCs on my Wi-Fi"));
	m_remoteLan->setChecked(s.value("remote/lan", false).toBool());
	form->addRow("", m_remoteLan);
	form->addRow("", hint(L("Off: only programs on this PC can connect. On: devices on your home network can too "
			      "(Windows asks once whether to allow Zunimi on private networks: choose Allow).")));
	m_remotePort = new QSpinBox;
	m_remotePort->setRange(1024, 65535);
	m_remotePort->setValue(s.value("remote/port", Remote::DefaultPort).toInt());
	form->addRow(L("Port"), m_remotePort);
	form->addRow("", hint(QString(L("OBS Studio uses %1 too. If both are open at once, give one of them another port."))
				      .arg(Remote::DefaultPort)));

	m_remotePw = new QLineEdit(Remote::password());
	m_remotePw->setEchoMode(QLineEdit::Password);
	auto *show = new QPushButton(L("Show"));
	show->setCheckable(true);
	auto *copy = new QPushButton(L("Copy"));
	auto *fresh = new QPushButton(L("New"));
	connect(show, &QPushButton::toggled, this, [this, show](bool on) {
		m_remotePw->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
		show->setText(on ? L("Hide") : L("Show"));
	});
	connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(m_remotePw->text()); });
	connect(fresh, &QPushButton::clicked, this, [this] {
		// kept only when Save is pressed; until then the old password still works
		static const char abc[] = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
		QString pw;
		for (int i = 0; i < 16; ++i)
			pw += QChar(abc[QRandomGenerator::system()->bounded(int(sizeof(abc) - 1))]);
		m_remotePw->setText(pw);
	});
	auto *pwRow = new QHBoxLayout;
	pwRow->addWidget(m_remotePw, 1);
	pwRow->addWidget(show);
	pwRow->addWidget(copy);
	pwRow->addWidget(fresh);
	form->addRow(L("Password"), pwRow);
	form->addRow("", hint(L("Always needed (at least 6 characters). After five wrong tries a device has to wait "
			      "before trying again.")));

	m_remoteDeck = new QLabel;
	m_remoteDeck->setWordWrap(true);
	m_remoteDeck->setTextInteractionFlags(Qt::TextSelectableByMouse);
	form->addRow(L("Stream Deck"), m_remoteDeck);
	form->addRow(L("Clip Studio"),
		     hint(L("In Clip Studio > Settings > OBS or Zunimi Studio, use address 127.0.0.1 and the port and password "
			    "above. It then turns every recording into clips, and every Mark moment into a clip it won't miss.")));
	m_remoteQr = new QLabel;
	m_remoteLink = new QLabel;
	m_remoteLink->setWordWrap(true);
	m_remoteLink->setTextInteractionFlags(Qt::TextSelectableByMouse);
	auto *phone = new QVBoxLayout;
	phone->addWidget(m_remoteQr);
	phone->addWidget(m_remoteLink);
	form->addRow(L("Phone remote"), phone);

	for (QCheckBox *box : {m_remoteOn, m_remoteLan})
		connect(box, &QCheckBox::toggled, this, &SettingsDialog::showRemoteInfo);
	connect(m_remotePort, &QSpinBox::valueChanged, this, &SettingsDialog::showRemoteInfo);
	connect(m_remotePw, &QLineEdit::textChanged, this, &SettingsDialog::showRemoteInfo);
	showRemoteInfo();
	area->setWidget(inner);
	outer->addWidget(area);
	return page;
}

void SettingsDialog::showRemoteInfo()
{
	const bool on = m_remoteOn->isChecked(), lan = m_remoteLan->isChecked();
	const int port = m_remotePort->value();
	m_remoteLan->setEnabled(on);
	m_remoteDeck->setText(
		on ? QString(L("In the Stream Deck app, add any <b>OBS Studio</b> action and connect with: Server IP "
			     "<b>127.0.0.1</b> (this PC's network address when the Stream Deck is on another PC), port "
			     "<b>%1</b>, and the password above."))
			     .arg(port)
		   : L("Turn on the remote first."));
	const QStringList addresses = Remote::addresses();
	if (!on || !lan || addresses.isEmpty()) {
		m_remoteQr->clear();
		m_remoteQr->hide();
		m_remoteLink->setText(!on    ? L("Turn on the remote first.")
				      : !lan ? L("Turn on \"Allow phones and PCs on my Wi-Fi\" to use your phone as a remote.")
					     : L("This PC is not on a home network right now."));
		return;
	}
	const QString link = Remote::phoneLink(addresses.first(), port, m_remotePw->text().trimmed());
	m_remoteQr->setPixmap(qrPixmap(link, 200));
	m_remoteQr->show();
	m_remoteLink->setText(
		QString(L("Press Save, then scan this with the phone's camera (same Wi-Fi as this PC). It opens the remote "
			"with the password already in.<br>Or open <b>http://%1:%2</b> on the phone and type the password.%3"))
			.arg(addresses.first())
			.arg(port)
			.arg(addresses.size() > 1 ? L("<br>Other addresses of this PC: ") + addresses.mid(1).join(", ") : QString()));
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
	m_closeTray = new QCheckBox(L("Closing the window keeps Zunimi Studio in the tray"));
	m_closeTray->setChecked(s.value("tray/close", true).toBool());
	m_escTray = new QCheckBox(L("Esc hides it to the tray"));
	m_escTray->setChecked(s.value("tray/esc", true).toBool());
	m_language = new QComboBox;
	for (const QString &code : i18n::languages())
		m_language->addItem(i18n::languageName(code), code);
	m_language->setCurrentIndex(std::max(0, m_language->findData(i18n::language())));
	form->addRow(L("Language"), m_language);
	m_accent = new QComboBox;
	for (const QString &a : Theme::accents())
		m_accent->addItem(Theme::accentName(a), a);
	m_accent->setCurrentIndex(std::max(0, m_accent->findData(s.value("ui/accent", "neon"))));
	form->addRow(L("Accent colour"), m_accent);
	form->addRow("", hint(L("Zunimi restarts to switch language. Words without a translation yet stay in English.")));
	form->addRow(L("Window mode"), m_mode);
	form->addRow("", hint(L("Borderless fills the screen above the taskbar; Fullscreen covers it all. F11 switches "
			      "fullscreen on and off any time. Without a title bar, touch the top edge for the window "
			      "buttons.")));
	form->addRow("", m_closeTray);
	form->addRow("", m_escTray);
	m_escAsk = new QCheckBox(L("Ask before Esc hides it"));
	m_escAsk->setChecked(s.value("tray/escAsk", true).toBool());
	form->addRow("", m_escAsk);
	form->addRow("", hint(L("Esc first leaves a text box or fullscreen. Quit from the tray icon. Quitting while "
			      "live or recording always asks first.")));
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
		encoders << L("NVIDIA NVENC");
	if (hw.amf)
		encoders << L("AMD AMF");
	if (hw.qsv)
		encoders << L("Intel QuickSync");
	encoders << "x264";
	auto *title = new QLabel(QString("%1 %2").arg(ZUNIMI_NAME, ZUNIMI_VERSION));
	title->setObjectName("h2");
	auto *check = new QPushButton(L("Check for updates"));
	check->setCursor(Qt::PointingHandCursor);
	connect(check, &QPushButton::clicked, this, [this] {
		if (checkUpdates)
			checkUpdates();
	});
	auto *head = new QHBoxLayout;
	head->addWidget(title, 1);
	head->addWidget(check);
	layout->addLayout(head);
	m_autoUpdate = new QCheckBox(L("Check for updates when Zunimi Studio starts"));
	m_autoUpdate->setChecked(Settings::get().value("update/auto", true).toBool());
	layout->addWidget(m_autoUpdate);
	layout->addSpacing(8);
	layout->addWidget(hint(QString(L("Processor threads: %1\nMemory: %2 GB\nGraphics: %3\nEncoders: %4\n"
				       "Auto-Tune choice: %5"))
				       .arg(hw.cores)
				       .arg(hw.ramGb, 0, 'f', 1)
				       .arg(hw.gpu.isEmpty() ? "unknown" : hw.gpu)
				       .arg(encoders.join(", "))
				       .arg(profileName(pickProfile(hw)))));
	layout->addWidget(hint(L("Loaded modules (only what is switched on is loaded): ") + m_core->modules().join(", ")));
	layout->addWidget(hint(L("Built on libobs from the OBS Project. Zunimi Studio is free software under the GPL "
			       "version 2 or later.")));
	layout->addStretch();
	return page;
}

void SettingsDialog::save()
{
	Settings &s = Settings::get();
	s.set("stream/platform", m_platform->currentData());
	if (m_accent->currentData().toString() != s.value("ui/accent", "neon").toString()) {
		s.set("ui/accent", m_accent->currentData());
		Theme::apply(); // at once, no restart
	}
	if (m_language->currentData().toString() != i18n::language())
		s.set("ui/language", m_language->currentData());
	s.set("remote/on", m_remoteOn->isChecked());
	s.set("remote/lan", m_remoteLan->isChecked());
	s.set("remote/port", m_remotePort->value());
	if (m_remotePw->text().trimmed().size() >= 6)
		s.setSecret("remote/password", m_remotePw->text().trimmed());
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
	s.set("tray/escAsk", m_escAsk->isChecked());
	s.set("stream/delaySec", m_delay->value());
	s.set("stream/autoRecord", m_autoRecord->isChecked());
	s.set("stream/track", m_streamTrack->currentData());
	s.set("video/codec", m_codec->currentData());
	s.set("video/rateControl", m_rate->currentData());
	s.set("video/quality", m_quality->value());
	s.set("video/preset", m_preset->currentData());
	s.set("video/keyint", m_keyint->value());
	s.set("record/format", m_format->currentData());
	s.set("record/quality", m_recQuality->currentData());
	s.set("record/codec", m_recCodec->currentData());
	uint32_t tracks = 0;
	for (int i = 0; i < 6; i++)
		if (m_tracks[i]->isChecked())
			tracks |= 1u << i;
	s.set("record/tracks", tracks ? tracks : 1);
	s.set("record/splitMinutes", m_splitMinutes->value());
	s.set("record/splitMb", m_splitMb->value());
	s.set("record/nameFormat", m_nameFormat->text().trimmed().isEmpty() ? "yyyy-MM-dd hh-mm-ss" : m_nameFormat->text().trimmed());
	s.set("audio/bitrate", m_audioKbps->value());
	const auto device = m_monitor->currentData().toStringList();
	if (device.size() == 2) {
		s.set("audio/monitorName", device[0]);
		s.set("audio/monitorId", device[1]);
		m_core->applyMonitoringDevice();
	}
	m_core->outputSettingsChanged();
	QVector<ObsCore::Destination> dests;
	for (int r = 0; r < m_destinations->rowCount(); r++) {
		auto cell = [this, r](int c) { return m_destinations->item(r, c) ? m_destinations->item(r, c)->text().trimmed() : QString(); };
		auto *key = qobject_cast<QLineEdit *>(m_destinations->cellWidget(r, 3));
		dests.append({cell(1), cell(2), key ? key->text().trimmed() : QString(),
			      m_destinations->item(r, 0) && m_destinations->item(r, 0)->checkState() == Qt::Checked});
	}
	ObsCore::setDestinations(dests);
	QJsonArray rules;
	for (int r = 0; r < m_rules->rowCount(); r++) {
		auto *box = qobject_cast<QComboBox *>(m_rules->cellWidget(r, 1));
		if (m_rules->item(r, 0) && box)
			rules.append(QJsonObject{{"app", m_rules->item(r, 0)->text().trimmed()}, {"scene", box->currentText()}});
	}
	s.set("auto/rules", QJsonDocument(rules).toJson(QJsonDocument::Compact));
	s.set("auto/on", m_autoOn->isChecked());
	s.set("privacy/on", m_privacyOn->isChecked());
	s.set("privacy/onlyLive", m_privacyLive->isChecked());
	QStringList patterns;
	for (const QString &line : m_patterns->toPlainText().split('\n'))
		if (!line.trimmed().isEmpty())
			patterns << line.trimmed();
	s.set("privacy/patterns", patterns);
	s.set("alarms/micMinutes", m_micMinutes->value());
	s.set("alarms/black", m_blackAlarm->isChecked());
	s.set("preflight/on", m_preflight->isChecked());
	s.set("report/show", m_report->isChecked());
	const QStringList before = {s.value("events/twitchChannel").toString(), s.value("events/youtube").toString(),
				    s.value("events/kickChannel").toString(), s.value("events/kickRoom").toString(),
				    s.secret("events/seJwt"), s.secret("events/slToken"), s.value("events/twitchClientId").toString(),
				    s.secret("events/kofiToken")};
	s.setSecret("events/kofiToken", m_kofiToken->text().trimmed());
	s.set("events/twitchChannel", m_twitchChannel->text().trimmed());
	s.set("events/youtube", m_youtube->text().trimmed());
	s.set("events/kickChannel", m_kick->text().trimmed());
	s.set("events/kickRoom", m_kickRoom->text().trimmed());
	if (!m_kickRoom->text().trimmed().isEmpty())
		s.set("events/kickRoomFor", m_kick->text().trimmed().toLower());
	s.setSecret("events/seJwt", m_seJwt->text().trimmed());
	s.setSecret("events/slToken", m_slToken->text().trimmed());
	s.set("events/currency", m_currency->currentData());
	s.set("events/twitchClientId", m_twitchClient->text().trimmed());
	const QStringList after = {m_twitchChannel->text().trimmed(), m_youtube->text().trimmed(), m_kick->text().trimmed(),
				   m_kickRoom->text().trimmed(), m_seJwt->text().trimmed(), m_slToken->text().trimmed(),
				   m_twitchClient->text().trimmed(), m_kofiToken->text().trimmed()};
	if (before != after || !Settings::get().secret("events/twitchToken").isEmpty())
		EventHub::get().restart();
	QStringList bad;
	for (const auto &[id, edit] : m_keys) {
		const QKeySequence keys = edit->keySequence();
		if (!keys.isEmpty() && !Hotkeys::supported(keys))
			bad << keys.toString(QKeySequence::NativeText);
		s.set("hotkeys/" + id, Hotkeys::supported(keys) ? keys.toString(QKeySequence::PortableText) : QString());
	}
	if (!bad.isEmpty())
		QMessageBox::information(this, L("Hotkeys"), L("These keys can't be used as hotkeys, so they were left empty: ") +
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
			QMessageBox::warning(this, L("Settings"), L("Video settings were not changed: ") + error);
			return;
		}
	}
	s.set("video/profile", m_profile->currentData());
	accept();
}
