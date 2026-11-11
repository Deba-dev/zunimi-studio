#include "ui/YouTubeStreams.h"

#include "core/I18n.h"
#include "core/Settings.h"
#include "net/YouTubeAccount.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

YouTubeStreamsDialog::YouTubeStreamsDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(L("Your YouTube streams"));
	resize(560, 460);
	m_who = new QLabel;
	m_who->setObjectName("h2");
	m_sign = new QPushButton;
	m_reload = new QPushButton(L("Refresh"));
	m_list = new QListWidget;
	m_note = new QLabel;
	m_note->setObjectName("hint");
	m_note->setWordWrap(true);
	auto *schedule = new QPushButton(L("Schedule a stream on YouTube..."));
	m_use = new QPushButton(L("Use this stream"));
	m_use->setObjectName("primary");
	auto *close = new QPushButton(L("Close"));

	auto *top = new QHBoxLayout;
	top->addWidget(m_who, 1);
	top->addWidget(m_reload);
	top->addWidget(m_sign);
	auto *bottom = new QHBoxLayout;
	bottom->addWidget(schedule);
	bottom->addStretch();
	bottom->addWidget(close);
	bottom->addWidget(m_use);
	auto *col = new QVBoxLayout(this);
	col->setContentsMargins(18, 18, 18, 18);
	col->setSpacing(10);
	col->addLayout(top);
	col->addWidget(m_list, 1);
	col->addWidget(m_note);
	col->addLayout(bottom);

	YouTubeAccount &yt = YouTubeAccount::get();
	connect(m_sign, &QPushButton::clicked, this, [this, &yt] {
		if (yt.signedIn()) {
			yt.signOut();
		} else {
			m_note->setText(L("Your browser opened. Choose your channel's Google account and allow access, then come back here."));
			yt.signIn();
		}
	});
	connect(m_reload, &QPushButton::clicked, this, &YouTubeStreamsDialog::refresh);
	connect(schedule, &QPushButton::clicked, this,
		[] { QDesktopServices::openUrl(QUrl("https://studio.youtube.com/")); });
	connect(m_use, &QPushButton::clicked, this, &YouTubeStreamsDialog::use);
	connect(close, &QPushButton::clicked, this, &QDialog::reject);
	connect(m_list, &QListWidget::itemDoubleClicked, this, &YouTubeStreamsDialog::use);
	connect(m_list, &QListWidget::currentRowChanged, this, [this] { m_use->setEnabled(m_list->currentItem() != nullptr); });
	connect(&yt, &YouTubeAccount::signedInChanged, this, &YouTubeStreamsDialog::refresh);
	connect(&yt, &YouTubeAccount::failed, this, [this](const QString &why) { m_note->setText(why); });
	connect(&yt, &YouTubeAccount::broadcastsReady, this, [this](const QVector<YouTubeAccount::Broadcast> &list) {
		m_list->clear();
		for (const YouTubeAccount::Broadcast &b : list) {
			const QString when = b.status == "live" ? L("LIVE now")
								: b.start.isValid() ? QLocale().toString(b.start.toLocalTime(), QLocale::ShortFormat)
										    : L("not scheduled");
			auto *item = new QListWidgetItem(QString("%1\n%2%3").arg(b.title, when,
										b.key.isEmpty() ? L("  ·  no stream key yet") : QString()));
			item->setData(Qt::UserRole, b.server);
			item->setData(Qt::UserRole + 1, b.key);
			item->setData(Qt::UserRole + 2, b.title);
			if (b.key.isEmpty())
				item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
			m_list->addItem(item);
		}
		m_note->setText(list.isEmpty() ? L("No scheduled or live streams. Schedule one on YouTube, then press Refresh.")
					       : L("Pick a stream: Zunimi sends your stream to it when you press Start Streaming."));
		for (int i = 0; i < m_list->count(); i++)
			if (m_list->item(i)->flags() & Qt::ItemIsEnabled) {
				m_list->setCurrentRow(i);
				break;
			}
	});
	refresh();
}

void YouTubeStreamsDialog::refresh()
{
	YouTubeAccount &yt = YouTubeAccount::get();
	const bool in = yt.signedIn();
	m_list->clear();
	m_use->setEnabled(false);
	m_reload->setEnabled(in);
	if (!YouTubeAccount::configured()) {
		m_who->setText(L("YouTube sign-in"));
		m_sign->setEnabled(false);
		m_sign->setText(L("Sign in with YouTube"));
		m_note->setText(L("This copy of Zunimi Stream+ has no Google sign-in set up yet. You can still paste your "
				  "stream key in Settings > Stream."));
		return;
	}
	m_sign->setEnabled(true);
	m_sign->setText(in ? L("Sign out") : L("Sign in with YouTube"));
	m_who->setText(in ? (yt.channel().isEmpty() ? L("Signed in to YouTube") : yt.channel()) : L("Not signed in"));
	if (in) {
		m_note->setText(L("Loading your streams..."));
		yt.listBroadcasts();
	} else {
		m_note->setText(L("Sign in to see the streams you scheduled in YouTube Studio and go live to the one you pick. "
				  "Zunimi only reads your streams; it never changes your channel."));
	}
}

void YouTubeStreamsDialog::use()
{
	QListWidgetItem *item = m_list->currentItem();
	if (!item || !(item->flags() & Qt::ItemIsEnabled))
		return;
	Settings &st = Settings::get();
	st.set("stream/platform", "youtube");
	st.set("stream/server", item->data(Qt::UserRole).toString());
	st.setSecret("stream/key", item->data(Qt::UserRole + 1).toString());
	st.set("youtube/broadcastTitle", item->data(Qt::UserRole + 2).toString());
	m_picked = item->data(Qt::UserRole + 2).toString();
	accept();
}
