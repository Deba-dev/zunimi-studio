#include "ui/SetupWizard.h"
#include "core/I18n.h"

#include "core/ObsCore.h"
#include "core/Settings.h"
#include "version.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

namespace {
struct Platform {
	const char *key, *label, *server, *keyUrl, *channelHint;
};
const Platform PLATFORMS[] = {
	{"twitch", "Twitch", "rtmp://live.twitch.tv/app", "https://dashboard.twitch.tv/settings/stream", "your Twitch name"},
	{"youtube", "YouTube", "rtmps://a.rtmps.youtube.com:443/live2", "https://studio.youtube.com/channel/UC/livestreaming",
	 "@yourchannel"},
	{"kick", "Kick", "", "https://kick.com/dashboard/settings/stream", "your Kick name"},
	{"custom", "Somewhere else", "", "", ""},
	{"", "Just recording for now", "", "", ""},
};

const Platform &platformOf(const QString &key)
{
	for (const Platform &p : PLATFORMS)
		if (key == p.key)
			return p;
	return PLATFORMS[std::size(PLATFORMS) - 1];
}

QLabel *title(const QString &text)
{
	auto *l = new QLabel(text);
	l->setObjectName("h1");
	l->setWordWrap(true);
	return l;
}

QLabel *hint(const QString &text)
{
	auto *l = new QLabel(text);
	l->setObjectName("hint");
	l->setWordWrap(true);
	return l;
}

QPushButton *choice(const QString &head, const QString &body)
{
	auto *b = new QPushButton(head + "\n" + body);
	b->setObjectName("choice");
	b->setCheckable(true);
	b->setCursor(Qt::PointingHandCursor);
	return b;
}
} // namespace

SetupWizard::SetupWizard(ObsCore *core, QWidget *parent) : QDialog(parent), m_core(core)
{
	setWindowTitle(L("Set up " ZUNIMI_NAME));
	resize(660, 560);
	m_pages = new QStackedWidget;
	m_pages->addWidget(levelPage());
	m_pages->addWidget(platformPage());
	m_pages->addWidget(showPage());
	m_pages->addWidget(readyPage());
	m_step = new QLabel;
	m_step->setObjectName("mute");
	m_skip = new QPushButton(L("Skip setup"));
	m_back = new QPushButton(L("Back"));
	m_next = new QPushButton(L("Next"));
	m_next->setObjectName("primary");
	connect(m_skip, &QPushButton::clicked, this, &QDialog::reject);
	connect(m_back, &QPushButton::clicked, this, [this] { go(m_pages->currentIndex() - 1); });
	connect(m_next, &QPushButton::clicked, this, [this] {
		if (m_pages->currentIndex() == m_pages->count() - 1)
			accept();
		else
			go(m_pages->currentIndex() + 1);
	});
	auto *buttons = new QHBoxLayout;
	buttons->addWidget(m_step);
	buttons->addStretch();
	buttons->addWidget(m_skip);
	buttons->addWidget(m_back);
	buttons->addWidget(m_next);
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(26, 22, 26, 18);
	layout->addWidget(m_pages, 1);
	layout->addLayout(buttons);
	go(0);
}

void SetupWizard::go(int page)
{
	page = std::clamp(page, 0, m_pages->count() - 1);
	if (page == m_pages->count() - 1)
		fillReady();
	m_pages->setCurrentIndex(page);
	m_step->setText(QString(L("Step %1 of %2")).arg(page + 1).arg(m_pages->count()));
	m_back->setVisible(page > 0);
	m_skip->setVisible(page < m_pages->count() - 1);
	m_next->setText(page == m_pages->count() - 1 ? L("Done") : L("Next"));
}

bool SetupWizard::obsFound()
{
	const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/obs-studio/basic/scenes";
	return !QDir(dir).entryList({"*.json"}, QDir::Files).isEmpty();
}

QVector<QPair<QString, QString>> SetupWizard::webcams()
{
	QVector<QPair<QString, QString>> out;
	obs_properties_t *props = obs_get_source_properties("dshow_input");
	if (!props)
		return out;
	if (obs_property_t *p = obs_properties_get(props, "video_device_id"))
		for (size_t i = 0; i < obs_property_list_item_count(p); ++i) {
			const QString name = QString::fromUtf8(obs_property_list_item_name(p, i));
			const QString id = QString::fromUtf8(obs_property_list_item_string(p, i));
			// the virtual camera would show Zunimi inside itself
			if (!id.isEmpty() && !name.contains("Virtual Camera", Qt::CaseInsensitive))
				out.append({name, id});
		}
	obs_properties_destroy(props);
	return out;
}

QWidget *SetupWizard::levelPage()
{
	auto *page = new QWidget;
	auto *col = new QVBoxLayout(page);
	col->setSpacing(12);
	col->addWidget(title(L("Welcome to " ZUNIMI_NAME)));
	col->addWidget(hint(L("A few quick questions and you're ready to go live. Everything here can be changed later in "
			    "Settings.")));
	col->addSpacing(6);
	auto *simple = choice(L("Keep it simple"),
			      L("Just what you need to go live and record. Good if you're new to streaming."));
	auto *pro = choice(L("Show me everything"),
			   L("Studio mode, transitions, filters, profiles, projectors. Good if you know OBS."));
	m_level = new QButtonGroup(this);
	m_level->addButton(simple, 0);
	m_level->addButton(pro, 1);
	simple->setChecked(true);
	col->addWidget(simple);
	col->addWidget(pro);
	col->addWidget(hint(L("Switch any time from Menu.")));
	m_import = new QCheckBox(L("Bring in my scenes from OBS Studio (OBS itself is not changed)"));
	m_import->setVisible(obsFound());
	m_import->setChecked(obsFound());
	col->addSpacing(8);
	col->addWidget(m_import);
	col->addStretch();
	return page;
}

QWidget *SetupWizard::platformPage()
{
	auto *page = new QWidget;
	auto *col = new QVBoxLayout(page);
	col->setSpacing(10);
	col->addWidget(title(L("Where do you stream?")));
	auto *grid = new QHBoxLayout;
	grid->setSpacing(8);
	m_platform = new QButtonGroup(this);
	int i = 0;
	for (const Platform &p : PLATFORMS) {
		auto *b = new QPushButton(L(p.label));
		b->setObjectName("choice");
		b->setCheckable(true);
		b->setCursor(Qt::PointingHandCursor);
		b->setProperty("key", QString(p.key));
		m_platform->addButton(b, i++);
		grid->addWidget(b);
	}
	const QString saved = Settings::get().value("stream/platform", "twitch").toString();
	for (QAbstractButton *b : m_platform->buttons())
		if (b->property("key").toString() == saved)
			b->setChecked(true);
	col->addLayout(grid);
	col->addSpacing(6);
	m_serverLabel = new QLabel(L("Server address"));
	m_server = new QLineEdit(Settings::get().value("stream/server").toString());
	m_server->setPlaceholderText("rtmp://... or srt://...");
	col->addWidget(m_serverLabel);
	col->addWidget(m_server);
	col->addWidget(new QLabel(L("Stream key")));
	m_key = new QLineEdit(Settings::get().secret("stream/key"));
	m_key->setEchoMode(QLineEdit::Password);
	m_key->setPlaceholderText(L("paste it here (it stays on this PC, encrypted)"));
	col->addWidget(m_key);
	m_keyHelp = new QLabel;
	m_keyHelp->setOpenExternalLinks(true);
	m_keyHelp->setWordWrap(true);
	col->addWidget(m_keyHelp);
	col->addWidget(hint(L("Never show your stream key on stream: anyone with it can stream to your channel.")));
	col->addSpacing(6);
	m_channelLabel = new QLabel(L("Your channel, for chat and alerts"));
	m_channel = new QLineEdit;
	col->addWidget(m_channelLabel);
	col->addWidget(m_channel);
	col->addStretch();
	connect(m_platform, &QButtonGroup::idClicked, this, [this] { platformChanged(); });
	platformChanged();
	return page;
}

void SetupWizard::platformChanged()
{
	QAbstractButton *b = m_platform->checkedButton();
	const QString key = b ? b->property("key").toString() : QString();
	const Platform &p = platformOf(key);
	const bool streams = !key.isEmpty();
	const bool needsServer = key == "kick" || key == "custom";
	m_serverLabel->setVisible(needsServer);
	m_server->setVisible(needsServer);
	if (!needsServer)
		m_server->setText(p.server);
	m_key->setEnabled(streams);
	m_keyHelp->setText(!streams ? L("No problem: add a stream key later in Settings > Stream.")
			   : QString(p.keyUrl).isEmpty()
				   ? L("Your platform shows the server address and key in its streaming settings.")
				   : QString(L("Find it here: <a href='%1'>%2 stream key page</a>"
					     "%3"))
					     .arg(p.keyUrl, L(p.label),
						  key == "kick" ? L(" (Kick shows a Stream URL for the server box too).") : "."));
	const bool chat = key == "twitch" || key == "youtube" || key == "kick";
	m_channelLabel->setVisible(chat);
	m_channel->setVisible(chat);
	m_channel->setPlaceholderText(L(p.channelHint));
	Settings &s = Settings::get();
	m_channel->setText(key == "twitch"	? s.value("events/twitchChannel").toString()
			   : key == "youtube" ? s.value("events/youtube").toString()
			   : key == "kick"    ? s.value("events/kickChannel").toString()
					      : QString());
}

QWidget *SetupWizard::showPage()
{
	auto *page = new QWidget;
	auto *col = new QVBoxLayout(page);
	col->setSpacing(10);
	col->addWidget(title(L("What should viewers see?")));
	col->addWidget(hint(L("Pick as many as you like. You can move and resize each one in the preview afterwards.")));
	col->addSpacing(4);
	m_game = new QCheckBox(L("My game"));
	col->addWidget(m_game);
	col->addWidget(hint(L("Shows whichever game is fullscreen. Valorant and other anti-cheat games need \"My screen\" "
			    "instead for now.")));
	m_screen = new QCheckBox(L("My screen"));
	m_screens = new QComboBox;
	for (const auto &[label, id] : m_core->screens())
		m_screens->addItem(label, id);
	m_screen->setChecked(true);
	auto *screenRow = new QHBoxLayout;
	screenRow->addWidget(m_screen);
	screenRow->addWidget(m_screens, 1);
	col->addLayout(screenRow);
	m_webcam = new QCheckBox(L("My webcam"));
	m_webcams = new QComboBox;
	for (const auto &[name, id] : webcams())
		m_webcams->addItem(name, id);
	m_webcam->setEnabled(m_webcams->count() > 0);
	m_webcams->setEnabled(m_webcams->count() > 0);
	if (m_webcams->count() == 0)
		m_webcams->addItem(L("No webcam found"));
	auto *camRow = new QHBoxLayout;
	camRow->addWidget(m_webcam);
	camRow->addWidget(m_webcams, 1);
	col->addLayout(camRow);
	col->addSpacing(6);
	m_alerts = new QCheckBox(L("Alerts for follows, subs, tips and raids"));
	m_chat = new QCheckBox(L("Chat on screen"));
	m_alerts->setChecked(true);
	col->addWidget(m_alerts);
	col->addWidget(m_chat);
	col->addWidget(hint(L("Both are built in: no browser source, nearly no extra load. Your mic and desktop sound are "
			    "already in.")));
	connect(m_screen, &QCheckBox::toggled, m_screens, &QWidget::setEnabled);
	col->addStretch();
	return page;
}

QWidget *SetupWizard::readyPage()
{
	auto *page = new QWidget;
	auto *col = new QVBoxLayout(page);
	col->setSpacing(10);
	col->addWidget(title(L("You're set")));
	m_summary = new QLabel;
	m_summary->setWordWrap(true);
	m_summary->setTextFormat(Qt::RichText);
	col->addWidget(m_summary);
	col->addStretch();
	return page;
}

void SetupWizard::fillReady()
{
	const SetupChoices c = choices();
	const VideoPlan &plan = m_core->plan();
	QStringList lines;
	lines << QString(L("<b>Quality:</b> Auto-Tune picked %1p at %2 fps for this PC (%3 kbps). Settings > Video "
			 "changes it."))
			 .arg(plan.height)
			 .arg(plan.fps)
			 .arg(plan.bitrate);
	if (c.platform.isEmpty())
		lines << L("<b>Streaming:</b> not set up yet; Record works right away.");
	else if (c.key.isEmpty())
		lines << QString(L("<b>Streaming:</b> %1, but no stream key yet. Go Live asks for it."))
				 .arg(L(platformOf(c.platform).label));
	else
		lines << QString(L("<b>Streaming:</b> %1, key saved.")).arg(L(platformOf(c.platform).label));
	QStringList shown;
	if (c.game)
		shown << L("your game");
	if (!c.screenId.isEmpty())
		shown << L("your screen");
	if (!c.webcamId.isEmpty())
		shown << L("your webcam");
	if (c.alerts)
		shown << "alerts";
	if (c.chat)
		shown << "chat";
	lines << L("<b>On screen:</b> ") + (shown.isEmpty() ? QString(L("nothing yet; use Add source.")) : shown.join(", ") + ".");
	if (c.importObs)
		lines << L("<b>OBS scenes:</b> you'll pick which collection to bring in next.");
	lines << "";
	lines << L("<b>Good to know</b>");
	lines << L("• The coloured dot at the top shows how the stream is doing. When something goes wrong, a <b>Fix it</b> "
		 "button appears.");
	lines << L("• Mark a moment while live (Menu, or a hotkey) and Zunimi saves the replay and a chapter for you.");
	lines << L("• Settings > Remote turns your phone or a Stream Deck into a remote.");
	m_summary->setText(lines.join("<br>"));
}

SetupChoices SetupWizard::choices() const
{
	SetupChoices c;
	c.simple = m_level->checkedId() != 1;
	c.importObs = m_import->isVisible() && m_import->isChecked();
	QAbstractButton *b = m_platform->checkedButton();
	c.platform = b ? b->property("key").toString() : QString();
	if (!c.platform.isEmpty()) {
		c.server = m_server->text().trimmed();
		c.key = m_key->text().trimmed();
		if (m_channel->isVisible())
			c.channel = m_channel->text().trimmed();
	}
	c.game = m_game->isChecked();
	if (m_screen->isChecked() && m_screens->count() > 0) {
		c.screenId = m_screens->currentData().toString();
		c.screenLabel = m_screens->currentText();
	}
	if (m_webcam->isChecked() && m_webcams->isEnabled()) {
		c.webcamId = m_webcams->currentData().toString();
		c.webcamName = m_webcams->currentText();
	}
	c.alerts = m_alerts->isChecked();
	c.chat = m_chat->isChecked();
	return c;
}
