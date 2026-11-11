#include "ui/LivePanels.h"
#include "core/I18n.h"

#include "core/Settings.h"
#include "events/Events.h"
#include "overlays/Overlays.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QScrollBar>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "events/Events.h"

namespace {
QString statusLine()
{
	EventHub &hub = EventHub::get();
	QStringList on, off;
	static const QHash<QString, QString> names = {{"twitch", L("Twitch chat")},       {"twitch_follows", L("Twitch follows")},
						      {"youtube", L("YouTube")},          {"kick", L("Kick")},
						      {"streamelements", L("StreamElements")}, {"streamlabs", L("Streamlabs")},
						      {"relay", L("Kick alerts and Ko-fi")}};
	for (const QString &s : hub.services()) {
		const QString st = hub.status(s);
		if (hub.connected(s))
			on << names.value(s);
		else if (!st.isEmpty() && st != L("Off") && st != L("Not set up"))
			off << names.value(s) + ": " + st;
	}
	QString text = on.isEmpty() ? L("Nothing connected yet.") : L("Connected: ") + on.join(", ");
	if (!off.isEmpty())
		text += "\n" + off.join("\n");
	return text;
}

QLabel *muted(const QString &text)
{
	auto *l = new QLabel(text);
	l->setObjectName("mute");
	l->setWordWrap(true);
	return l;
}
} // namespace

ChatPanel::ChatPanel(QWidget *parent) : QWidget(parent)
{
	auto *col = new QVBoxLayout(this);
	col->setContentsMargins(4, 4, 4, 4);
	col->setSpacing(6);
	m_view = new QTextBrowser;
	m_view->setOpenExternalLinks(false);
	m_view->setStyleSheet("QTextBrowser { background: transparent; border: none; }");
	m_view->setMinimumHeight(24); // the tab it shares with Stats can stay small; it grows when there is room
	m_status = muted({});
	auto *setup = new QPushButton(L("Connect chat..."));
	connect(setup, &QPushButton::clicked, this, &ChatPanel::setupRequested);
	m_input = new QLineEdit;
	m_input->setPlaceholderText(L("Message to Twitch chat (log in under Connections)"));
	connect(m_input, &QLineEdit::returnPressed, this, &ChatPanel::send);
	col->addWidget(m_view, 1);
	col->addWidget(m_status);
	col->addWidget(m_input);
	col->addWidget(setup);

	auto show = [this] {
		m_status->setText(statusLine());
		m_input->setEnabled(!Settings::get().secret("events/twitchToken").isEmpty());
	};
	show();
	connect(&EventHub::get(), &EventHub::statusChanged, this, show);
	connect(&EventHub::get(), &EventHub::chat, this, [this](const ChatMessage &m) {
		static const QHash<QString, QString> tags = {{"twitch", L("<span style='color:#9146FF'>TW</span>")},
							     {"youtube", L("<span style='color:#FF3B3B'>YT</span>")},
							     {"kick", L("<span style='color:#53FC18'>K</span>")}};
		const QString color = m.color.isEmpty() ? "#7CC4FF" : m.color;
		QString badge = m.owner ? "🎙 " : m.mod ? "🛡 " : m.vip ? "💎 " : "";
		const bool atBottom = m_view->verticalScrollBar()->value() >= m_view->verticalScrollBar()->maximum() - 4;
		m_view->append(QString(L("%1 <b style='color:%2'>%3%4</b>&nbsp; %5"))
				       .arg(tags.value(m.platform), color, badge.toHtmlEscaped(), m.user.toHtmlEscaped(),
					    m.text.toHtmlEscaped()));
		if (++m_lines > 400) { // keep it light
			m_view->clear();
			m_lines = 0;
		}
		if (atBottom)
			m_view->verticalScrollBar()->setValue(m_view->verticalScrollBar()->maximum());
	});
}

void ChatPanel::send()
{
	const QString text = m_input->text().trimmed();
	Settings &st = Settings::get();
	const QString token = st.secret("events/twitchToken"), user = st.value("events/twitchUserId").toString();
	if (text.isEmpty() || token.isEmpty() || user.isEmpty())
		return;
	QNetworkRequest req(QUrl("https://api.twitch.tv/helix/chat/messages"));
	req.setRawHeader("Client-Id", TwitchFollows::clientId().toUtf8());
	req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	const QJsonObject body{{"broadcaster_id", user}, {"sender_id", user}, {"message", text}};
	QNetworkReply *reply = EventHub::get().network()->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		reply->deleteLater();
		if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
			m_status->setText(L("Twitch didn't take the message. Log in again under Connections."));
	});
	m_input->clear();
}

EventsPanel::EventsPanel(QWidget *parent) : QWidget(parent)
{
	auto *col = new QVBoxLayout(this);
	col->setContentsMargins(4, 4, 4, 4);
	col->setSpacing(6);
	m_list = new QListWidget;
	m_list->setToolTip(L("Double-click an event to show its alert again"));
	m_list->setMinimumHeight(24);
	m_status = muted({});
	auto *test = new QPushButton(L("Test an alert"));
	auto *menu = new QMenu(test);
	for (const QString &t : overlays::alertTypes())
		menu->addAction(overlays::alertTypeLabel(t), this, [t] { EventHub::get().test(t); });
	test->setMenu(menu);
	auto *reset = new QPushButton(L("Reset counters"));
	reset->setToolTip(L("Goals, labels and totals start from zero (for example at the start of a stream)"));
	connect(reset, &QPushButton::clicked, this, [] { EventHub::get().resetTotals(); });
	auto *setup = new QPushButton(L("Connections..."));
	connect(setup, &QPushButton::clicked, this, &EventsPanel::setupRequested);
	auto *row = new QHBoxLayout;
	row->addWidget(test);
	row->addWidget(reset);
	col->addWidget(m_list, 1);
	col->addWidget(m_status);
	col->addLayout(row);
	col->addWidget(setup);
	connect(m_list, &QListWidget::itemDoubleClicked, this, [this] {
		const int i = m_list->currentRow();
		if (i >= 0 && i < EventHub::get().recent().size())
			overlays::Engine::get().replay(EventHub::get().recent()[i]);
	});
	connect(&EventHub::get(), &EventHub::event, this, &EventsPanel::refresh);
	connect(&EventHub::get(), &EventHub::statusChanged, this, &EventsPanel::refresh);
	refresh();
}

void EventsPanel::refresh()
{
	m_list->clear();
	for (const StreamEvent &e : EventHub::get().recent()) {
		QString line = QString("%1  %2  ·  %3").arg(e.at.toString("hh:mm"), e.user, e.label());
		if (!e.amountText().isEmpty())
			line += "  " + e.amountText();
		if (e.test)
			line += L("  (test)");
		auto *item = new QListWidgetItem(line);
		if (!e.message.isEmpty())
			item->setToolTip(e.message);
		m_list->addItem(item);
	}
	m_status->setText(statusLine());
}
