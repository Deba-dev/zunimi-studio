#include "events/Events.h"

#include "core/Settings.h"
#include "net/WebSocket.h"

#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace {
// A Twitch app registered for Zunimi Studio (public client, device login). Set ZUNIMI_TWITCH_CLIENT_ID at build
// time, or "events/twitchClientId" in settings, until the official one is in.
#ifndef ZUNIMI_TWITCH_CLIENT_ID
#define ZUNIMI_TWITCH_CLIENT_ID "eocu63kztqs36q1cd760nj87mul7nv" // Zunimi Studio on dev.twitch.tv (public client)
#endif
const char *YT_UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36";

QString money(double amount, const QString &currency)
{
	static const QHash<QString, QString> symbols = {{"INR", "₹"}, {"USD", "$"}, {"EUR", "€"}, {"GBP", "£"},
							{"JPY", "¥"}, {"BRL", "R$"}, {"CAD", "CA$"}, {"AUD", "A$"}};
	const QString sym = symbols.value(currency.toUpper(), currency);
	const QString n = std::fmod(amount, 1.0) == 0.0 ? QString::number(qlonglong(amount)) : QString::number(amount, 'f', 2);
	return sym.size() <= 3 && !sym.isEmpty() && !sym[0].isLetter() ? sym + n : n + " " + sym;
}

// "₹1,500.00" or "$5.00" -> amount and currency
void parseMoney(const QString &text, double *amount, QString *currency)
{
	QString digits;
	QString cur;
	for (const QChar c : text) {
		if (c.isDigit() || c == '.')
			digits += c;
		else if (c != ',' && !c.isSpace())
			cur += c;
	}
	*amount = digits.toDouble();
	static const QHash<QString, QString> codes = {{"₹", "INR"}, {"$", "USD"}, {"€", "EUR"}, {"£", "GBP"},
						      {"¥", "JPY"}, {"R$", "BRL"}, {"CA$", "CAD"}, {"A$", "AUD"}};
	*currency = codes.value(cur, cur);
}

QString runsText(const QJsonObject &message)
{
	if (message.contains("simpleText"))
		return message.value("simpleText").toString();
	QString out;
	for (const QJsonValue &run : message.value("runs").toArray()) {
		const QJsonObject r = run.toObject();
		if (r.contains("text"))
			out += r.value("text").toString();
		else if (r.contains("emoji")) {
			const QJsonObject e = r.value("emoji").toObject();
			const QJsonArray shortcuts = e.value("shortcuts").toArray();
			out += e.value("isCustomEmoji").toBool() && !shortcuts.isEmpty() ? shortcuts.first().toString()
											: e.value("emojiId").toString();
		}
	}
	return out;
}

QString ircUnescape(QString v)
{
	v.replace("\\s", " ").replace("\\:", ";").replace("\\r", "\r").replace("\\n", "\n").replace("\\\\", "\\");
	return v;
}
} // namespace

// ---- events
QString StreamEvent::amountText() const
{
	if (type == "tip" || type == "superchat")
		return money(amount, currency);
	if (type == "bits")
		return QString("%1 bits").arg(qlonglong(amount));
	if (type == "raid")
		return QString("%1 viewers").arg(count);
	if (type == "gift")
		return QString("%1 gift%2").arg(count).arg(count == 1 ? "" : "s");
	if (type == "resub" || (type == "member" && count > 1))
		return QString("%1 months").arg(count);
	return {};
}

QString StreamEvent::label() const
{
	static const QHash<QString, QString> names = {
		{"follow", "Follow"}, {"sub", "Sub"},         {"resub", "Resub"},   {"gift", "Gift subs"},
		{"bits", "Bits"},     {"raid", "Raid"},       {"tip", "Tip"},       {"superchat", "Super Chat"},
		{"member", "Member"}, {"redeem", "Redeem"}};
	return names.value(type, type);
}

EventHub &EventHub::get()
{
	static EventHub hub;
	return hub;
}

EventHub::EventHub()
{
	m_connectors.insert("twitch", new TwitchChat(this));
	m_connectors.insert("twitch_follows", new TwitchFollows(this));
	m_connectors.insert("youtube", new YouTubeChat(this));
	m_connectors.insert("kick", new KickChat(this));
	m_connectors.insert("streamelements", new StreamElementsEvents(this));
	m_connectors.insert("streamlabs", new StreamlabsEvents(this));
	Settings &st = Settings::get();
	for (const QString &k : {"follows", "subs", "bits", "tips", "members"})
		m_totals.insert(k, st.value("events/total_" + QString(k), 0.0).toDouble());
}

QNetworkAccessManager *EventHub::network()
{
	if (!m_net)
		m_net = new QNetworkAccessManager(this);
	return m_net;
}

QStringList EventHub::services() const
{
	return {"twitch", "twitch_follows", "youtube", "kick", "streamelements", "streamlabs"};
}

void EventHub::restart()
{
	for (Connector *c : m_connectors) {
		c->stop();
		c->start();
	}
}

void EventHub::stopAll()
{
	for (Connector *c : m_connectors)
		c->stop();
}

QString EventHub::status(const QString &service) const
{
	return m_status.value(service).first;
}

bool EventHub::connected(const QString &service) const
{
	return m_status.value(service).second;
}

void EventHub::setStatus(const QString &service, const QString &text, bool ok)
{
	m_status.insert(service, {text, ok});
	emit statusChanged(service);
}

void EventHub::emitEvent(const StreamEvent &e)
{
	// the same sub can arrive from Twitch chat and from StreamElements: keep the first
	const QString key = e.type + "|" + e.user.toLower() + "|" + QString::number(qlonglong(e.amount));
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (!e.test && m_seen.contains(key) && now - m_seen.value(key) < 15000)
		return;
	m_seen.insert(key, now);
	if (m_seen.size() > 500)
		m_seen.clear();
	m_recent.prepend(e);
	if (m_recent.size() > 100)
		m_recent.removeLast();
	if (!e.test) {
		Settings &st = Settings::get();
		const QString mainCurrency = st.value("events/currency", "INR").toString();
		auto add = [&](const QString &k, double v) {
			m_totals[k] += v;
			st.set("events/total_" + k, m_totals[k]);
		};
		if (e.type == "follow")
			add("follows", 1);
		else if (e.type == "sub" || e.type == "resub")
			add("subs", 1);
		else if (e.type == "gift")
			add("subs", std::max(1, e.count));
		else if (e.type == "bits")
			add("bits", e.amount);
		else if ((e.type == "tip" || e.type == "superchat") &&
			 (e.currency.isEmpty() || e.currency.compare(mainCurrency, Qt::CaseInsensitive) == 0))
			add("tips", e.amount);
		else if (e.type == "member")
			add("members", 1);
		m_latest.insert(e.type, e.user);
		if (e.amount > m_top.value(e.type).second)
			m_top.insert(e.type, {e.user, e.amount});
		emit totalsChanged();
	}
	emit event(e);
}

void EventHub::emitChat(const ChatMessage &m)
{
	emit chat(m);
}

double EventHub::total(const QString &key) const
{
	return m_totals.value(key);
}

void EventHub::resetTotals()
{
	for (auto it = m_totals.begin(); it != m_totals.end(); ++it) {
		it.value() = 0;
		Settings::get().set("events/total_" + it.key(), 0.0);
	}
	emit totalsChanged();
}

QString EventHub::latest(const QString &type) const
{
	return m_latest.value(type);
}

QString EventHub::top(const QString &type) const
{
	return m_top.value(type).first;
}

void EventHub::test(const QString &type)
{
	static const QStringList names = {"Aarav", "PixelNinja", "Kavya_plays", "NightOwl", "Rohan_07", "Sana"};
	StreamEvent e;
	e.platform = "test";
	e.type = type;
	e.user = names[QRandomGenerator::global()->bounded(int(names.size()))];
	e.test = true;
	if (type == "tip" || type == "superchat") {
		e.amount = QList<double>{50, 100, 500, 1000}[QRandomGenerator::global()->bounded(4)];
		e.currency = Settings::get().value("events/currency", "INR").toString();
		e.message = "Bhai kya clutch tha! 🔥";
	} else if (type == "bits") {
		e.amount = 500;
		e.message = "Cheer500 GG";
	} else if (type == "raid") {
		e.count = 42;
	} else if (type == "gift") {
		e.count = 5;
	} else if (type == "resub") {
		e.count = 7;
		e.message = "7 months already!";
	}
	emitEvent(e);
}

// ---- connector base
Connector::Connector(const QString &key, QObject *parent) : QObject(parent), m_key(key), m_retry(new QTimer(this))
{
	m_retry->setSingleShot(true);
	connect(m_retry, &QTimer::timeout, this, [this] {
		if (m_running)
			start();
	});
}

WebSocket *Connector::socket()
{
	if (!m_ws) {
		m_ws = new WebSocket(this);
		connect(m_ws, &WebSocket::closed, this, [this](const QString &why) {
			if (m_running)
				retryLater(why);
		});
	}
	return m_ws;
}

void Connector::stop()
{
	m_running = false;
	m_retry->stop();
	if (m_ws)
		m_ws->close();
	hub().setStatus(m_key, "Off", false);
}

void Connector::retryLater(const QString &why)
{
	m_failures++;
	const int seconds = std::min(120, 3 * (1 << std::min(m_failures, 5)));
	hub().setStatus(m_key, QString("%1 Trying again in %2 s.").arg(why).arg(seconds), false);
	m_retry->start(seconds * 1000);
}

void Connector::ok(const QString &text)
{
	m_failures = 0;
	hub().setStatus(m_key, text, true);
}

// ---- Twitch chat (anonymous IRC over websocket)
TwitchChat::TwitchChat(QObject *parent) : Connector("twitch", parent)
{
	connect(socket(), &WebSocket::opened, this, [this] {
		m_ws->sendText("CAP REQ :twitch.tv/tags twitch.tv/commands");
		m_ws->sendText("PASS SCHMOOPIIE");
		m_ws->sendText(QString("NICK justinfan%1").arg(QRandomGenerator::global()->bounded(10000, 99999)));
		m_ws->sendText("JOIN #" + m_channel);
	});
	connect(m_ws, &WebSocket::textMessage, this, [this](const QString &text) {
		for (const QString &line : text.split("\r\n", Qt::SkipEmptyParts)) {
			if (line.startsWith("PING")) {
				m_ws->sendText("PONG" + line.mid(4));
				continue;
			}
			if (line.contains(" JOIN #"))
				ok("Reading #" + m_channel + " chat");
			ChatMessage chat;
			StreamEvent event;
			if (!parseLine(line, &chat, &event))
				continue;
			if (!chat.text.isEmpty() || !chat.user.isEmpty())
				hub().emitChat(chat);
			if (!event.type.isEmpty())
				hub().emitEvent(event);
		}
	});
}

void TwitchChat::start()
{
	m_channel = Settings::get().value("events/twitchChannel").toString().trimmed().toLower().remove('#');
	m_channel.remove(QRegularExpression("^https?://(www\\.)?twitch\\.tv/"));
	if (m_channel.isEmpty()) {
		hub().setStatus(m_key, "Not set up", false);
		return;
	}
	m_running = true;
	hub().setStatus(m_key, "Connecting...", false);
	socket()->open(QUrl("wss://irc-ws.chat.twitch.tv:443"));
}

bool TwitchChat::parseLine(const QString &raw, ChatMessage *chat, StreamEvent *event)
{
	// @tags :prefix COMMAND #channel :text
	QString line = raw;
	QHash<QString, QString> tags;
	if (line.startsWith('@')) {
		const int sp = int(line.indexOf(' '));
		for (const QString &kv : line.mid(1, sp - 1).split(';')) {
			const int eq = int(kv.indexOf('='));
			tags.insert(kv.left(eq), ircUnescape(kv.mid(eq + 1)));
		}
		line = line.mid(sp + 1);
	}
	const QStringList parts = line.split(' ');
	if (parts.size() < 3)
		return false;
	const QString command = parts[1];
	const int textAt = int(line.indexOf(" :", 1));
	const QString text = textAt >= 0 ? line.mid(textAt + 2) : QString();
	const QString login = parts[0].mid(1).section('!', 0, 0);
	const QString name = tags.value("display-name").isEmpty() ? login : tags.value("display-name");
	const QString badges = tags.value("badges");
	if (command == "PRIVMSG") {
		chat->platform = "twitch";
		chat->id = tags.value("id");
		chat->user = name;
		chat->color = tags.value("color");
		chat->text = text.startsWith("\x01" "ACTION ") ? text.mid(8).chopped(1) : text;
		chat->mod = badges.contains("moderator/");
		chat->sub = badges.contains("subscriber/") || badges.contains("founder/");
		chat->vip = badges.contains("vip/");
		chat->owner = badges.contains("broadcaster/");
		const int bits = tags.value("bits").toInt();
		if (bits > 0) {
			event->platform = "twitch";
			event->type = "bits";
			event->user = name;
			event->amount = bits;
			event->message = text;
		}
		return true;
	}
	if (command == "USERNOTICE") {
		const QString id = tags.value("msg-id");
		event->platform = "twitch";
		event->user = name;
		event->message = text;
		event->tier = tags.value("msg-param-sub-plan");
		if (id == "sub") {
			event->type = "sub";
			event->count = 1;
		} else if (id == "resub") {
			event->type = "resub";
			event->count = tags.value("msg-param-cumulative-months").toInt();
		} else if (id == "submysterygift") {
			event->type = "gift";
			event->count = tags.value("msg-param-mass-gift-count").toInt();
		} else if (id == "subgift" && tags.value("msg-param-community-gift-id").isEmpty()) {
			event->type = "gift"; // a single gift; ones inside a gift bomb are already counted above
			event->count = 1;
			event->message = "to " + tags.value("msg-param-recipient-display-name");
		} else if (id == "raid") {
			event->type = "raid";
			event->user = tags.value("msg-param-displayName", name);
			event->count = tags.value("msg-param-viewerCount").toInt();
		} else {
			return false;
		}
		return true;
	}
	return false;
}

// ---- Twitch follows (EventSub, needs a login)
TwitchFollows::TwitchFollows(QObject *parent) : Connector("twitch_follows", parent)
{
	connect(socket(), &WebSocket::textMessage, this, [this](const QString &text) {
		const QJsonObject msg = QJsonDocument::fromJson(text.toUtf8()).object();
		const QString type = msg.value("metadata").toObject().value("message_type").toString();
		const QJsonObject payload = msg.value("payload").toObject();
		if (type == "session_welcome") {
			subscribe(payload.value("session").toObject().value("id").toString());
		} else if (type == "session_reconnect") {
			socket()->open(QUrl(payload.value("session").toObject().value("reconnect_url").toString()));
		} else if (type == "notification") {
			const QJsonObject ev = payload.value("event").toObject();
			StreamEvent e;
			e.platform = "twitch";
			e.type = "follow";
			e.user = ev.value("user_name").toString();
			hub().emitEvent(e);
		} else if (type == "revocation") {
			retryLater("Twitch stopped the follow alerts (the login may have expired).");
		}
	});
}

QString TwitchFollows::clientId()
{
	const QString custom = Settings::get().value("events/twitchClientId").toString().trimmed();
	return custom.isEmpty() ? QString(ZUNIMI_TWITCH_CLIENT_ID) : custom;
}

void TwitchFollows::start()
{
	if (Settings::get().secret("events/twitchToken").isEmpty() || clientId().isEmpty()) {
		hub().setStatus(m_key, clientId().isEmpty() ? "Needs a Twitch app id" : "Not logged in", false);
		return;
	}
	m_running = true;
	hub().setStatus(m_key, "Connecting...", false);
	socket()->open(QUrl("wss://eventsub.wss.twitch.tv/ws?keepalive_timeout_seconds=30"));
}

void TwitchFollows::subscribe(const QString &session)
{
	Settings &st = Settings::get();
	const QString user = st.value("events/twitchUserId").toString();
	QJsonObject body{{"type", "channel.follow"},
			 {"version", "2"},
			 {"condition", QJsonObject{{"broadcaster_user_id", user}, {"moderator_user_id", user}}},
			 {"transport", QJsonObject{{"method", "websocket"}, {"session_id", session}}}};
	QNetworkRequest req(QUrl("https://api.twitch.tv/helix/eventsub/subscriptions"));
	req.setRawHeader("Client-Id", clientId().toUtf8());
	req.setRawHeader("Authorization", "Bearer " + st.secret("events/twitchToken").toUtf8());
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	QNetworkReply *reply = hub().network()->post(req, QJsonDocument(body).toJson());
	connect(reply, &QNetworkReply::finished, this, [this, reply, session] {
		reply->deleteLater();
		const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (code == 202) {
			ok("Follow alerts on");
		} else if (code == 401) {
			refresh([this, session](bool good) {
				if (good)
					subscribe(session);
				else
					stop(), hub().setStatus(m_key, "Log in to Twitch again (Settings > Connections)", false);
			});
		} else {
			retryLater(QString("Twitch said no to follow alerts (%1).").arg(code));
		}
	});
}

void TwitchFollows::refresh(std::function<void(bool)> then)
{
	Settings &st = Settings::get();
	QUrlQuery q;
	q.addQueryItem("client_id", clientId());
	q.addQueryItem("grant_type", "refresh_token");
	q.addQueryItem("refresh_token", st.secret("events/twitchRefresh"));
	QNetworkRequest req(QUrl("https://id.twitch.tv/oauth2/token"));
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
	QNetworkReply *reply = hub().network()->post(req, q.toString(QUrl::FullyEncoded).toUtf8());
	connect(reply, &QNetworkReply::finished, this, [reply, then] {
		reply->deleteLater();
		const QJsonObject r = QJsonDocument::fromJson(reply->readAll()).object();
		if (!r.contains("access_token")) {
			then(false);
			return;
		}
		Settings::get().setSecret("events/twitchToken", r.value("access_token").toString());
		Settings::get().setSecret("events/twitchRefresh", r.value("refresh_token").toString());
		then(true);
	});
}

void TwitchFollows::login(std::function<void(const QString &, const QString &)> show, std::function<void(const QString &)> done)
{
	const QString id = clientId();
	if (id.isEmpty()) {
		done("This build has no Twitch app id yet. Add one in Settings > Connections > Advanced.");
		return;
	}
	const QString scopes = "moderator:read:followers user:write:chat";
	QUrlQuery q;
	q.addQueryItem("client_id", id);
	q.addQueryItem("scopes", scopes);
	QNetworkRequest req(QUrl("https://id.twitch.tv/oauth2/device"));
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
	QNetworkReply *reply = EventHub::get().network()->post(req, q.toString(QUrl::FullyEncoded).toUtf8());
	QObject::connect(reply, &QNetworkReply::finished, reply, [reply, id, scopes, show, done] {
		reply->deleteLater();
		const QJsonObject r = QJsonDocument::fromJson(reply->readAll()).object();
		const QString device = r.value("device_code").toString();
		if (device.isEmpty()) {
			done("Twitch did not start the login: " + r.value("message").toString());
			return;
		}
		show(r.value("user_code").toString(), r.value("verification_uri").toString());
		QDesktopServices::openUrl(QUrl(r.value("verification_uri").toString()));
		// ask every few seconds until the streamer confirms (or the code runs out)
		auto *timer = new QTimer(&EventHub::get());
		const qint64 until = QDateTime::currentSecsSinceEpoch() + r.value("expires_in").toInt(1800);
		QObject::connect(timer, &QTimer::timeout, timer, [timer, id, scopes, device, until, done] {
			if (QDateTime::currentSecsSinceEpoch() > until) {
				timer->deleteLater();
				done("The code ran out. Try again.");
				return;
			}
			QUrlQuery t;
			t.addQueryItem("client_id", id);
			t.addQueryItem("scopes", scopes);
			t.addQueryItem("device_code", device);
			t.addQueryItem("grant_type", "urn:ietf:params:oauth:grant-type:device_code");
			QNetworkRequest req(QUrl("https://id.twitch.tv/oauth2/token"));
			req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
			QNetworkReply *poll = EventHub::get().network()->post(req, t.toString(QUrl::FullyEncoded).toUtf8());
			QObject::connect(poll, &QNetworkReply::finished, poll, [poll, timer, id, done] {
				poll->deleteLater();
				const QJsonObject tok = QJsonDocument::fromJson(poll->readAll()).object();
				const QString access = tok.value("access_token").toString();
				if (access.isEmpty())
					return; // still waiting for the streamer
				timer->deleteLater();
				Settings::get().setSecret("events/twitchToken", access);
				Settings::get().setSecret("events/twitchRefresh", tok.value("refresh_token").toString());
				QNetworkRequest who(QUrl("https://api.twitch.tv/helix/users"));
				who.setRawHeader("Client-Id", id.toUtf8());
				who.setRawHeader("Authorization", "Bearer " + access.toUtf8());
				QNetworkReply *me = EventHub::get().network()->get(who);
				QObject::connect(me, &QNetworkReply::finished, me, [me, done] {
					me->deleteLater();
					const QJsonObject user = QJsonDocument::fromJson(me->readAll())
									 .object()
									 .value("data")
									 .toArray()
									 .first()
									 .toObject();
					Settings::get().set("events/twitchUserId", user.value("id").toString());
					Settings::get().set("events/twitchLogin", user.value("display_name").toString());
					if (Settings::get().value("events/twitchChannel").toString().isEmpty())
						Settings::get().set("events/twitchChannel", user.value("login").toString());
					done({});
				});
			});
		});
		timer->start(std::max(2, r.value("interval").toInt(5)) * 1000);
	});
}

// ---- YouTube (the live chat page's own feed)
YouTubeChat::YouTubeChat(QObject *parent) : Connector("youtube", parent), m_poll(new QTimer(this))
{
	m_poll->setSingleShot(true);
	connect(m_poll, &QTimer::timeout, this, &YouTubeChat::poll);
}

void YouTubeChat::stop()
{
	m_poll->stop();
	Connector::stop();
}

void YouTubeChat::start()
{
	if (Settings::get().value("events/youtube").toString().trimmed().isEmpty()) {
		hub().setStatus(m_key, "Not set up", false);
		return;
	}
	m_running = true;
	m_first = true;
	hub().setStatus(m_key, "Looking for your live stream...", false);
	findVideo();
}

void YouTubeChat::findVideo()
{
	const QString input = Settings::get().value("events/youtube").toString().trimmed();
	static const QRegularExpression videoId("(?:v=|youtu\\.be/|/live/|^)([A-Za-z0-9_-]{11})(?:[&?#]|$)");
	const auto m = videoId.match(input);
	if (m.hasMatch() && !input.startsWith('@') && !input.contains("/channel/") && !input.contains("/@")) {
		openChat(m.captured(1));
		return;
	}
	// a channel: its /live page points at the stream that is on now
	QString channel = input;
	if (!channel.startsWith("http"))
		channel = channel.startsWith("UC") && channel.size() == 24 ? "https://www.youtube.com/channel/" + channel
									   : "https://www.youtube.com/" + (channel.startsWith('@') ? channel : "@" + channel);
	QNetworkRequest req(QUrl(channel.section('?', 0, 0).remove(QRegularExpression("/+$")) + "/live"));
	req.setRawHeader("User-Agent", YT_UA);
	req.setRawHeader("Cookie", "CONSENT=YES+1; SOCS=CAI");
	req.setRawHeader("Accept-Language", "en-US,en;q=0.9");
	QNetworkReply *reply = hub().network()->get(req);
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		reply->deleteLater();
		if (!m_running)
			return;
		const QString html = QString::fromUtf8(reply->readAll());
		static const QRegularExpression canonical("<link rel=\"canonical\" href=\"https://www\\.youtube\\.com/watch\\?v=([A-Za-z0-9_-]{11})\"");
		const auto m = canonical.match(html);
		if (!m.hasMatch() || !html.contains("\"isLive\":true")) {
			hub().setStatus(m_key, "Waiting for you to go live on YouTube...", false);
			m_retry->start(60000); // check again in a minute
			return;
		}
		openChat(m.captured(1));
	});
}

void YouTubeChat::openChat(const QString &video)
{
	QNetworkRequest req(QUrl("https://www.youtube.com/live_chat?is_popout=1&v=" + video));
	req.setRawHeader("User-Agent", YT_UA);
	req.setRawHeader("Cookie", "CONSENT=YES+1; SOCS=CAI");
	req.setRawHeader("Accept-Language", "en-US,en;q=0.9");
	QNetworkReply *reply = hub().network()->get(req);
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		reply->deleteLater();
		if (!m_running)
			return;
		const QString html = QString::fromUtf8(reply->readAll());
		static const QRegularExpression key("\"INNERTUBE_API_KEY\":\"([^\"]+)\"");
		static const QRegularExpression ver("\"INNERTUBE_CLIENT_VERSION\":\"([^\"]+)\"");
		static const QRegularExpression cont("\"continuation\":\"([^\"]+)\"");
		m_key = key.match(html).captured(1);
		m_version = ver.match(html).captured(1);
		m_continuation = cont.match(html).captured(1);
		if (m_key.isEmpty() || m_continuation.isEmpty()) {
			retryLater("This stream's chat is off or not live yet.");
			return;
		}
		ok("Reading YouTube chat");
		poll();
	});
}

void YouTubeChat::poll()
{
	if (!m_running)
		return;
	QJsonObject body{{"context", QJsonObject{{"client", QJsonObject{{"clientName", "WEB"}, {"clientVersion", m_version}}}}},
			 {"continuation", m_continuation}};
	QNetworkRequest req(QUrl("https://www.youtube.com/youtubei/v1/live_chat/get_live_chat?key=" + m_key + "&prettyPrint=false"));
	req.setRawHeader("User-Agent", YT_UA);
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	QNetworkReply *reply = hub().network()->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		reply->deleteLater();
		if (!m_running)
			return;
		const QJsonObject live = QJsonDocument::fromJson(reply->readAll())
						 .object()
						 .value("continuationContents")
						 .toObject()
						 .value("liveChatContinuation")
						 .toObject();
		if (live.isEmpty()) {
			retryLater("YouTube chat ended or stopped answering.");
			return;
		}
		QVector<ChatMessage> chat;
		QVector<StreamEvent> events;
		// the first page is the backlog: show it in chat, but don't fire old Super Chats as alerts
		parseActions(live.value("actions").toArray(), !m_first, &chat, &events);
		m_first = false;
		for (const ChatMessage &m : chat)
			hub().emitChat(m);
		for (const StreamEvent &e : events)
			hub().emitEvent(e);
		int wait = 3000;
		for (const QJsonValue &c : live.value("continuations").toArray()) {
			const QJsonObject o = c.toObject();
			for (const char *k : {"invalidationContinuationData", "timedContinuationData", "reloadContinuationData"}) {
				if (o.contains(k)) {
					m_continuation = o.value(k).toObject().value("continuation").toString();
					wait = std::clamp(o.value(k).toObject().value("timeoutMs").toInt(3000), 1000, 10000);
				}
			}
		}
		m_poll->start(wait);
	});
}

void YouTubeChat::parseActions(const QJsonArray &actions, bool alerts, QVector<ChatMessage> *chat, QVector<StreamEvent> *events)
{
	for (const QJsonValue &a : actions) {
		const QJsonObject item = a.toObject().value("addChatItemAction").toObject().value("item").toObject();
		for (auto it = item.begin(); it != item.end(); ++it) {
			const QJsonObject r = it.value().toObject();
			ChatMessage m;
			m.platform = "youtube";
			m.id = r.value("id").toString();
			m.user = runsText(r.value("authorName").toObject());
			m.text = runsText(r.value("message").toObject());
			for (const QJsonValue &b : r.value("authorBadges").toArray()) {
				const QString kind = b.toObject().value("liveChatAuthorBadgeRenderer").toObject().value("icon").toObject().value("iconType").toString();
				m.mod |= kind == "MODERATOR";
				m.owner |= kind == "OWNER";
				m.sub |= kind.isEmpty(); // members have a picture badge, no icon
			}
			StreamEvent e;
			e.platform = "youtube";
			e.user = m.user;
			const QString kind = it.key();
			if (kind == "liveChatPaidMessageRenderer" || kind == "liveChatPaidStickerRenderer") {
				e.type = "superchat";
				parseMoney(r.value("purchaseAmountText").toObject().value("simpleText").toString(), &e.amount, &e.currency);
				e.message = m.text;
			} else if (kind == "liveChatMembershipItemRenderer") {
				e.type = "member";
				e.message = runsText(r.value("headerSubtext").toObject());
				static const QRegularExpression months("(\\d+)\\s+month");
				e.count = months.match(runsText(r.value("headerPrimaryText").toObject())).captured(1).toInt();
			} else if (kind == "liveChatSponsorshipsGiftPurchaseAnnouncementRenderer") {
				const QJsonObject header = r.value("header").toObject().value("liveChatSponsorshipsHeaderRenderer").toObject();
				e.user = runsText(header.value("authorName").toObject());
				e.type = "gift";
				static const QRegularExpression n("(\\d+)");
				e.count = n.match(runsText(header.value("primaryText").toObject())).captured(1).toInt();
			} else if (kind != "liveChatTextMessageRenderer") {
				continue;
			}
			if (!m.text.isEmpty())
				chat->append(m);
			if (alerts && !e.type.isEmpty())
				events->append(e);
		}
	}
}

// ---- Kick (public chat socket)
KickChat::KickChat(QObject *parent) : Connector("kick", parent)
{
	connect(socket(), &WebSocket::textMessage, this, [this](const QString &text) {
		const QJsonObject packet = QJsonDocument::fromJson(text.toUtf8()).object();
		const QString ev = packet.value("event").toString();
		if (ev == "pusher:connection_established") {
			const QString room = Settings::get().value("events/kickRoom").toString();
			m_ws->sendText(QJsonDocument(QJsonObject{{"event", "pusher:subscribe"},
								 {"data", QJsonObject{{"auth", ""}, {"channel", "chatrooms." + room + ".v2"}}}})
					       .toJson(QJsonDocument::Compact));
		} else if (ev == "pusher_internal:subscription_succeeded") {
			ok("Reading Kick chat");
		} else if (ev == "pusher:ping") {
			m_ws->sendText("{\"event\":\"pusher:pong\",\"data\":{}}");
		} else {
			ChatMessage chat;
			StreamEvent event;
			if (parse(packet, &chat, &event)) {
				if (!chat.text.isEmpty())
					hub().emitChat(chat);
				if (!event.type.isEmpty())
					hub().emitEvent(event);
			}
		}
	});
}

void KickChat::start()
{
	QString slug = Settings::get().value("events/kickChannel").toString().trimmed().toLower();
	slug.remove(QRegularExpression("^https?://(www\\.)?kick\\.com/"));
	if (slug.isEmpty()) {
		hub().setStatus(m_key, "Not set up", false);
		return;
	}
	m_running = true;
	const QString room = Settings::get().value("events/kickRoom").toString();
	if (!room.isEmpty() && Settings::get().value("events/kickRoomFor").toString() == slug) {
		join(room);
		return;
	}
	hub().setStatus(m_key, "Finding the chat room...", false);
	QNetworkRequest req(QUrl("https://kick.com/api/v2/channels/" + slug));
	req.setRawHeader("User-Agent", YT_UA);
	req.setRawHeader("Accept", "application/json");
	QNetworkReply *reply = hub().network()->get(req);
	connect(reply, &QNetworkReply::finished, this, [this, reply, slug] {
		reply->deleteLater();
		const QString id = QString::number(
			QJsonDocument::fromJson(reply->readAll()).object().value("chatroom").toObject().value("id").toInteger());
		if (id == "0") {
			retryLater("Kick didn't share the chat room. Paste its number in Settings > Connections.");
			return;
		}
		Settings::get().set("events/kickRoom", id);
		Settings::get().set("events/kickRoomFor", slug);
		join(id);
	});
}

void KickChat::join(const QString &)
{
	hub().setStatus(m_key, "Connecting...", false);
	socket()->open(QUrl("wss://ws-us2.pusher.com/app/32cbd69e4b950bf97679?protocol=7&client=js&version=8.4.0&flash=false"));
}

bool KickChat::parse(const QJsonObject &packet, ChatMessage *chat, StreamEvent *event)
{
	const QString ev = packet.value("event").toString();
	const QJsonObject data = QJsonDocument::fromJson(packet.value("data").toString().toUtf8()).object();
	if (ev == "App\\Events\\ChatMessageEvent") {
		const QJsonObject sender = data.value("sender").toObject();
		chat->platform = "kick";
		chat->id = data.value("id").toString();
		chat->user = sender.value("username").toString();
		chat->color = sender.value("identity").toObject().value("color").toString();
		static const QRegularExpression emote("\\[emote:\\d+:([^\\]]+)\\]");
		chat->text = data.value("content").toString().replace(emote, ":\\1:");
		for (const QJsonValue &b : sender.value("identity").toObject().value("badges").toArray()) {
			const QString type = b.toObject().value("type").toString();
			chat->mod |= type == "moderator";
			chat->sub |= type == "subscriber" || type == "founder";
			chat->vip |= type == "vip";
			chat->owner |= type == "broadcaster";
		}
		return true;
	}
	if (ev == "App\\Events\\SubscriptionEvent") {
		event->platform = "kick";
		event->user = data.value("username").toString();
		event->count = data.value("months").toInt(1);
		event->type = event->count > 1 ? "resub" : "sub";
		return true;
	}
	if (ev == "App\\Events\\GiftedSubscriptionsEvent") {
		event->platform = "kick";
		event->type = "gift";
		event->user = data.value("gifter_username").toString();
		event->count = int(data.value("gifted_usernames").toArray().size());
		return true;
	}
	return false;
}

// ---- StreamElements (Astro socket)
StreamElementsEvents::StreamElementsEvents(QObject *parent) : Connector("streamelements", parent)
{
	connect(socket(), &WebSocket::opened, this, [this] {
		const QJsonObject sub{{"type", "subscribe"},
				      {"nonce", QUuid::createUuid().toString(QUuid::WithoutBraces)},
				      {"data", QJsonObject{{"topic", "channel.activities"},
							   {"token", Settings::get().secret("events/seJwt")},
							   {"token_type", "jwt"}}}};
		m_ws->sendText(QJsonDocument(sub).toJson(QJsonDocument::Compact));
	});
	connect(m_ws, &WebSocket::textMessage, this, [this](const QString &text) {
		const QJsonObject msg = QJsonDocument::fromJson(text.toUtf8()).object();
		const QString type = msg.value("type").toString();
		if (type == "response") {
			if (msg.value("error").toString().isEmpty())
				ok("StreamElements events on");
			else {
				m_running = false;
				hub().setStatus(m_key, "StreamElements refused the token: " + msg.value("data").toObject().value("message").toString(), false);
			}
		} else if (type == "message") {
			StreamEvent e;
			if (parse(msg.value("data").toObject(), &e))
				hub().emitEvent(e);
		}
	});
}

void StreamElementsEvents::start()
{
	if (Settings::get().secret("events/seJwt").isEmpty()) {
		hub().setStatus(m_key, "Not set up", false);
		return;
	}
	m_running = true;
	hub().setStatus(m_key, "Connecting...", false);
	socket()->open(QUrl("wss://astro.streamelements.com"));
}

bool StreamElementsEvents::parse(const QJsonObject &activity, StreamEvent *e)
{
	const QJsonObject d = activity.value("data").toObject();
	const QString type = activity.value("type").toString();
	e->platform = activity.value("provider").toString("streamelements");
	e->user = d.value("displayName").toString(d.value("username").toString());
	e->message = d.value("message").toString();
	if (type == "follow") {
		e->type = "follow";
	} else if (type == "tip") {
		e->type = "tip";
		e->amount = d.value("amount").toDouble();
		e->currency = d.value("currency").toString();
		e->platform = "streamelements";
	} else if (type == "subscriber") {
		if (d.value("gifted").toBool() || d.value("bulkGifted").toBool()) {
			e->type = "gift";
			e->user = d.value("sender").toString(e->user);
			e->count = std::max(1, d.value("amount").toInt(1));
		} else {
			e->count = d.value("amount").toInt(1);
			e->type = e->count > 1 ? "resub" : "sub";
		}
		e->tier = d.value("tier").toString();
	} else if (type == "cheer") {
		e->type = "bits";
		e->amount = d.value("amount").toDouble();
	} else if (type == "raid") {
		e->type = "raid";
		e->count = d.value("amount").toInt();
	} else if (type == "superchat") {
		e->type = "superchat";
		e->amount = d.value("amount").toDouble();
		e->currency = d.value("currency").toString();
	} else if (type == "sponsor") {
		e->type = "member";
		e->count = d.value("amount").toInt(1);
	} else {
		return false;
	}
	return true;
}

// ---- Streamlabs (socket.io over websocket)
StreamlabsEvents::StreamlabsEvents(QObject *parent) : Connector("streamlabs", parent), m_ping(new QTimer(this))
{
	connect(m_ping, &QTimer::timeout, this, [this] {
		if (m_ws && m_ws->isOpen())
			m_ws->sendText("2");
	});
	connect(socket(), &WebSocket::textMessage, this, [this](const QString &text) {
		if (text.startsWith("0")) {
			const int interval = QJsonDocument::fromJson(text.mid(1).toUtf8()).object().value("pingInterval").toInt(25000);
			m_ping->start(std::max(5000, interval));
		} else if (text == "40") {
			ok("Streamlabs events on");
		} else if (text.startsWith("42")) {
			const QJsonArray arr = QJsonDocument::fromJson(text.mid(2).toUtf8()).array();
			if (arr.size() >= 2 && arr[0].toString() == "event")
				for (const StreamEvent &e : parse(arr[1].toObject()))
					hub().emitEvent(e);
		} else if (text.startsWith("44")) {
			m_running = false;
			hub().setStatus(m_key, "Streamlabs refused the socket token.", false);
		}
	});
}

void StreamlabsEvents::start()
{
	const QString token = Settings::get().secret("events/slToken");
	if (token.isEmpty()) {
		hub().setStatus(m_key, "Not set up", false);
		return;
	}
	m_running = true;
	hub().setStatus(m_key, "Connecting...", false);
	socket()->open(QUrl("wss://sockets.streamlabs.com/socket.io/?EIO=3&transport=websocket&token=" +
			    QString::fromUtf8(QUrl::toPercentEncoding(token))));
}

void StreamlabsEvents::stop()
{
	m_ping->stop();
	Connector::stop();
}

QVector<StreamEvent> StreamlabsEvents::parse(const QJsonObject &payload)
{
	QVector<StreamEvent> out;
	const QString type = payload.value("type").toString();
	const QString from = payload.value("for").toString();
	const QString platform = from.startsWith("twitch") ? "twitch" : from.startsWith("youtube") ? "youtube" : "streamlabs";
	for (const QJsonValue &v : payload.value("message").toArray()) {
		const QJsonObject m = v.toObject();
		StreamEvent e;
		e.platform = platform;
		e.user = m.value("name").toString(m.value("display_name").toString());
		e.message = m.value("message").toString();
		if (type == "donation") {
			e.type = "tip";
			e.platform = "streamlabs";
			e.amount = m.value("amount").toVariant().toDouble();
			e.currency = m.value("currency").toString();
		} else if (type == "follow" || (type == "subscription" && platform == "youtube")) {
			e.type = platform == "youtube" ? "member" : "follow";
		} else if (type == "subscription" || type == "resub") {
			e.count = m.value("months").toInt(1);
			e.type = type == "resub" || e.count > 1 ? "resub" : "sub";
			e.tier = m.value("sub_plan").toString();
		} else if (type == "bits") {
			e.type = "bits";
			e.amount = m.value("amount").toVariant().toDouble();
		} else if (type == "raid") {
			e.type = "raid";
			e.count = m.value("raiders").toInt();
		} else if (type == "superchat") {
			e.type = "superchat";
			e.amount = m.value("amount").toVariant().toDouble() / 1000000.0; // micros
			e.currency = m.value("currency").toString();
		} else if (type == "membershipGift") {
			e.type = "gift";
			e.count = m.value("giftMembershipsCount").toInt(1);
		} else {
			continue;
		}
		out << e;
	}
	return out;
}
