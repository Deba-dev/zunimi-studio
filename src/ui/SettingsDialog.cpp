#include "ui/SettingsDialog.h"

#include "core/ObsCore.h"
#include "core/Assist.h"
#include "core/Settings.h"
#include "events/Events.h"
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

QString defaultFolder()
{
	return QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + "/Zunimi Studio";
}
} // namespace

SettingsDialog::SettingsDialog(ObsCore *core, WindowModes *modes, QWidget *parent)
	: QDialog(parent), m_core(core), m_modes(modes)
{
	setWindowTitle("Settings");
	resize(680, 600);
	m_tabs = new QTabWidget;
	m_tabs->addTab(streamTab(), "Stream");
	m_tabs->addTab(videoTab(), "Video");
	m_tabs->addTab(recordingTab(), "Recording");
	m_tabs->addTab(audioTab(), "Audio");
	m_tabs->addTab(connectionsTab(), "Connections");
	m_tabs->addTab(automationTab(), "Automation");
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
	m_delay = new QSpinBox;
	m_delay->setRange(0, 600);
	m_delay->setSuffix(" seconds");
	m_delay->setSpecialValueText("No delay");
	m_delay->setValue(s.value("stream/delaySec", 0).toInt());
	form->addRow("Stream delay", m_delay);
	form->addRow("", hint("A delay hides what's on your screen from stream snipers. Viewers see everything that "
			      "many seconds later."));
	m_autoRecord = new QCheckBox("Record automatically while live (and stop with the stream)");
	m_autoRecord->setChecked(s.value("stream/autoRecord", false).toBool());
	form->addRow("", m_autoRecord);
	m_destinations = new QTableWidget(0, 4);
	m_destinations->setHorizontalHeaderLabels({"On", "Name", "Server", "Stream key"});
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
	auto *addDest = new QPushButton("Add a platform");
	auto *addMenu = new QMenu(addDest);
	for (const Platform &p : PLATFORMS)
		if (QString(p.key) != "whip")
			addMenu->addAction(p.label, this, [addDestination, p] { addDestination({QString(p.label).section(' ', 0, 0), p.server, {}, true}); });
	addDest->setMenu(addMenu);
	auto *removeDest = new QPushButton("Remove");
	connect(removeDest, &QPushButton::clicked, this, [this] { m_destinations->removeRow(m_destinations->currentRow()); });
	auto *destButtons = new QHBoxLayout;
	destButtons->addWidget(addDest);
	destButtons->addWidget(removeDest);
	destButtons->addStretch();
	form->addRow("Also stream to", m_destinations);
	form->addRow("", destButtons);
	form->addRow("", hint("Multistream: the same picture goes to every platform ticked here at the same time. It "
			      "costs upload speed (one stream's bitrate per platform), not CPU."));
	m_streamTrack = new QComboBox;
	for (int i = 1; i <= 6; i++)
		m_streamTrack->addItem(QString("Track %1").arg(i), i);
	m_streamTrack->setCurrentIndex(std::clamp(s.value("stream/track", 1).toInt(), 1, 6) - 1);
	form->addRow("Stream sound from", m_streamTrack);
	form->addRow("", hint("Every source is on all tracks unless you change it in its Sound settings. Leave "
			      "music off a track to keep it out of the stream (or out of the recording)."));
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
	m_codec = new QComboBox;
	m_rate = new QComboBox;
	m_rate->addItem("Constant bitrate (best for streaming)", "CBR");
	m_rate->addItem("Variable bitrate", "VBR");
	m_rate->addItem("Constant quality (recording only)", "CQP");
	m_rate->setCurrentIndex(std::max(0, m_rate->findData(s.value("video/rateControl", "CBR"))));
	m_quality = new QSpinBox;
	m_quality->setRange(0, 51);
	m_quality->setValue(s.value("video/quality", 23).toInt());
	m_quality->setToolTip("Lower is better and bigger. 18-23 looks great.");
	m_preset = new QComboBox;
	m_preset->addItem("Faster (lighter on the PC)", 0);
	m_preset->addItem("Balanced", 1);
	m_preset->addItem("Better quality (heavier)", 2);
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
	form->addRow("Codec", m_codec);
	form->addRow("", hint("H.264 works everywhere. HEVC and AV1 look better at the same bitrate; YouTube takes "
			      "both, Twitch is still testing them. Only codecs your graphics card can encode are listed."));
	form->addRow("Rate control", m_rate);
	form->addRow("Quality (constant quality)", m_quality);
	form->addRow("Speed vs quality", m_preset);
	form->addRow("Keyframe every", m_keyint);
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
	form->addRow("", hint("\"Same as the stream\" costs almost nothing extra while live. A separate encode "
			      "looks better but uses more of your graphics card."));
	Settings &rs = Settings::get();
	m_format = new QComboBox;
	m_format->addItem("MP4 (safe: survives a crash)", "hybrid_mp4");
	m_format->addItem("MOV (safe: survives a crash)", "hybrid_mov");
	m_format->addItem("MKV (safe, remux to MP4 for editors)", "mkv");
	m_format->addItem("MP4 (classic; lost if the PC crashes)", "mp4");
	m_format->addItem("MOV (classic)", "mov");
	m_format->addItem("FLV", "flv");
	m_format->addItem("MPEG-TS", "ts");
	m_format->setCurrentIndex(std::max(0, m_format->findData(rs.value("record/format", "hybrid_mp4"))));
	m_recQuality = new QComboBox;
	m_recQuality->addItem("Same as the stream (lightest)", "same");
	m_recQuality->addItem("High quality (separate encode)", "high");
	m_recQuality->addItem("Near lossless (big files, for editing)", "lossless");
	m_recQuality->addItem("Smaller files", "small");
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
	m_splitMinutes->setSpecialValueText("Never");
	m_splitMinutes->setValue(rs.value("record/splitMinutes", 0).toInt());
	m_splitMb = new QSpinBox;
	m_splitMb->setRange(0, 1000000);
	m_splitMb->setSingleStep(1024);
	m_splitMb->setSuffix(" MB");
	m_splitMb->setSpecialValueText("Any size");
	m_splitMb->setValue(rs.value("record/splitMb", 0).toInt());
	auto *split = new QHBoxLayout;
	split->addWidget(m_splitMinutes);
	split->addWidget(m_splitMb);
	m_nameFormat = new QLineEdit(rs.value("record/nameFormat", "yyyy-MM-dd hh-mm-ss").toString());
	form->addRow("Format", m_format);
	form->addRow("Quality", m_recQuality);
	form->addRow("Codec (own quality only)", m_recCodec);
	form->addRow("Sound tracks", tracks);
	form->addRow("", hint("Each ticked track is saved as its own sound in the file (pick which sources go where "
			      "in their Sound settings). Editors can then turn the game down without touching your voice."));
	form->addRow("New file every", split);
	form->addRow("File name", m_nameFormat);
	form->addRow("", hint("yyyy year, MM month, dd day, hh hour, mm minute, ss second; {scene} and {profile} "
			      "work too."));
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

QWidget *SettingsDialog::audioTab()
{
	Settings &s = Settings::get();
	auto *page = new QWidget;
	auto *form = new QFormLayout(page);
	form->setContentsMargins(18, 18, 18, 18);
	form->setVerticalSpacing(12);
	m_monitor = new QComboBox;
	const QString current = s.value("audio/monitorId", "default").toString();
	m_monitor->addItem("Windows default", QStringList{"Default", "default"});
	for (const auto &[name, id] : ObsCore::monitoringDevices())
		if (id != "default")
			m_monitor->addItem(name, QStringList{name, id});
	for (int i = 0; i < m_monitor->count(); i++)
		if (m_monitor->itemData(i).toStringList().value(1) == current)
			m_monitor->setCurrentIndex(i);
	form->addRow("Headphones (listen)", m_monitor);
	form->addRow("", hint("Where \"Listen in headphones\" plays, set per source in its Sound settings (the ⋯ "
			      "button in the mixer)."));
	m_audioKbps = new QSpinBox;
	m_audioKbps->setRange(64, 320);
	m_audioKbps->setSingleStep(32);
	m_audioKbps->setSuffix(" kbps");
	m_audioKbps->setValue(s.value("audio/bitrate", 160).toInt());
	form->addRow("Sound quality", m_audioKbps);
	form->addRow("", hint("160 kbps is plenty for streams. Music streams can use 320."));
	form->addRow("", hint("Push to talk and push to mute are in Hotkeys."));
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
		auto *l = new QLabel(QString("<a href='%1' style='color:#22D3EE'>%2</a>").arg(url, text));
		l->setOpenExternalLinks(true);
		return l;
	};
	form->addRow("", hint("Chat, alerts, goals and widgets read from these. Nothing here is needed to stream."));

	m_twitchChannel = new QLineEdit(s.value("events/twitchChannel").toString());
	m_twitchChannel->setPlaceholderText("your Twitch name");
	form->addRow("Twitch channel", m_twitchChannel);
	form->addRow("", hint("Chat, subs, gift subs, bits and raids work with just the name."));
	m_twitchLogin = new QLabel;
	auto showLogin = [this] {
		const QString who = Settings::get().value("events/twitchLogin").toString();
		m_twitchLogin->setText(Settings::get().secret("events/twitchToken").isEmpty() ? "Not logged in"
											     : "Logged in as " + who);
	};
	showLogin();
	auto *login = new QPushButton("Log in to Twitch");
	auto *logout = new QPushButton("Log out");
	connect(login, &QPushButton::clicked, this, [this, login, showLogin] {
		login->setEnabled(false);
		TwitchFollows::login(
			[this](const QString &code, const QString &url) {
				m_twitchLogin->setText(QString("Enter code <b>%1</b> at %2 (opened in your browser)").arg(code, url));
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
	form->addRow("Twitch login", loginRow);
	form->addRow("", hint("Only needed for follow alerts and for typing in chat from here."));

	m_youtube = new QLineEdit(s.value("events/youtube").toString());
	m_youtube->setPlaceholderText("@yourchannel, or a stream link");
	form->addRow("YouTube", m_youtube);
	form->addRow("", hint("Chat, Super Chats, Super Stickers, memberships and gifted memberships, with no login. "
			      "With a channel, Zunimi finds the stream as soon as you go live."));
	m_kick = new QLineEdit(s.value("events/kickChannel").toString());
	m_kick->setPlaceholderText("your Kick name");
	form->addRow("Kick", m_kick);
	m_kickRoom = new QLineEdit(s.value("events/kickRoom").toString());
	m_kickRoom->setPlaceholderText("found by itself; paste it only if Kick blocks the lookup");
	form->addRow("Kick chat room number", m_kickRoom);

	m_seJwt = new QLineEdit(s.secret("events/seJwt"));
	m_seJwt->setEchoMode(QLineEdit::Password);
	m_seJwt->setPlaceholderText("JWT token");
	form->addRow("StreamElements", m_seJwt);
	form->addRow("", link("Find it: StreamElements > your account > Channels > Show secrets (JWT Token)",
			      "https://streamelements.com/dashboard/account/channels"));
	m_slToken = new QLineEdit(s.secret("events/slToken"));
	m_slToken->setEchoMode(QLineEdit::Password);
	m_slToken->setPlaceholderText("Socket API token");
	form->addRow("Streamlabs", m_slToken);
	form->addRow("", link("Find it: Streamlabs > Settings > API Settings > API Tokens (Socket API Token)",
			      "https://streamlabs.com/dashboard#/settings/api-settings"));
	form->addRow("", hint("Tips sent through StreamElements or Streamlabs (UPI, cards, PayPal) show as alerts here, "
			      "with your own designs."));

	m_currency = new QComboBox;
	for (const char *c : {"INR", "USD", "EUR", "GBP", "BRL", "CAD", "AUD", "JPY"})
		m_currency->addItem(c, c);
	m_currency->setCurrentIndex(std::max(0, m_currency->findData(s.value("events/currency", "INR"))));
	form->addRow("Main currency", m_currency);
	form->addRow("", hint("Tip goals and tip totals count this currency."));

	m_twitchClient = new QLineEdit(s.value("events/twitchClientId").toString());
	m_twitchClient->setPlaceholderText(TwitchFollows::clientId().isEmpty() ? "needed for the Twitch login" : "built in");
	form->addRow("Advanced: Twitch app id", m_twitchClient);
	form->addRow("", hint("Make one at dev.twitch.tv > Applications (client type: Public, redirect "
			      "http://localhost). Leave empty to use Zunimi's own."));
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

	m_autoOn = new QCheckBox("Switch scenes by the app in front");
	m_autoOn->setChecked(s.value("auto/on", false).toBool());
	form->addRow("", m_autoOn);
	m_rules = new QTableWidget(0, 2);
	m_rules->setHorizontalHeaderLabels({"When this app is in front", "Switch to scene"});
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
	auto *add = new QPushButton("Add an open app...");
	connect(add, &QPushButton::clicked, this, [this, addRule] {
		bool ok = false;
		const QString app = QInputDialog::getItem(this, "Pick an app", "Apps with a window open now (start your game first):",
							  Assist::runningApps(), 0, true, &ok);
		if (ok && !app.trimmed().isEmpty())
			addRule(app.trimmed(), m_core->currentScene());
	});
	auto *remove = new QPushButton("Remove");
	connect(remove, &QPushButton::clicked, this, [this] { m_rules->removeRow(m_rules->currentRow()); });
	auto *ruleButtons = new QHBoxLayout;
	ruleButtons->addWidget(add);
	ruleButtons->addWidget(remove);
	ruleButtons->addStretch();
	form->addRow(m_rules);
	form->addRow(ruleButtons);
	form->addRow("", hint("For example VALORANT-Win64-Shipping.exe switches to \"Gaming\", Discord.exe to \"Just "
			      "chatting\". It only switches when the app in front changes, so your own switching sticks."));

	m_privacyOn = new QCheckBox("Privacy guard: hide screen captures while a private window is in front");
	m_privacyOn->setChecked(s.value("privacy/on", true).toBool());
	m_privacyLive = new QCheckBox("Only while live or recording");
	m_privacyLive->setChecked(s.value("privacy/onlyLive", true).toBool());
	m_patterns = new QPlainTextEdit(s.value("privacy/patterns").toStringList().join("\n"));
	m_patterns->setPlaceholderText("password\nbitwarden\nstream key\nphonepe\npaytm\nwhatsapp\n(one word per line; empty uses these)");
	m_patterns->setMaximumHeight(110);
	form->addRow("", m_privacyOn);
	form->addRow("", m_privacyLive);
	form->addRow("Private when the title has", m_patterns);
	form->addRow("", hint("Screen and window captures are hidden until you switch away, then come back by themselves. "
			      "Your game capture stays."));

	m_micMinutes = new QSpinBox;
	m_micMinutes->setRange(0, 60);
	m_micMinutes->setSuffix(" min");
	m_micMinutes->setSpecialValueText("Never");
	m_micMinutes->setValue(s.value("alarms/micMinutes", 3).toInt());
	form->addRow("Warn when the mic is silent for", m_micMinutes);
	m_blackAlarm = new QCheckBox("Warn when the stream goes black");
	m_blackAlarm->setChecked(s.value("alarms/black", true).toBool());
	form->addRow("", m_blackAlarm);
	m_preflight = new QCheckBox("Check my setup before going live (empty scene, muted mic, disk space...)");
	m_preflight->setChecked(s.value("preflight/on", true).toBool());
	form->addRow("", m_preflight);
	m_report = new QCheckBox("Show how the stream went when it ends");
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
	m_escAsk = new QCheckBox("Ask before Esc hides it");
	m_escAsk->setChecked(s.value("tray/escAsk", true).toBool());
	form->addRow("", m_escAsk);
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
				    s.secret("events/seJwt"), s.secret("events/slToken"), s.value("events/twitchClientId").toString()};
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
				   m_twitchClient->text().trimmed()};
	if (before != after || m_twitchLogin->text().startsWith("Logged in"))
		EventHub::get().restart();
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
