#pragma once
#include <QDateTime>
#include <QJsonArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include <functional>

class QNetworkAccessManager;
class QTimer;
class WebSocket;

// Something that happened on stream. type: follow, sub, resub, gift, bits, raid, tip, superchat, member, redeem.
struct StreamEvent {
	QString platform; // twitch, youtube, kick, streamelements, streamlabs, kofi, test
	QString type;
	QString user;
	QString message;
	double amount = 0; // money, bits, months, viewers or gifts, depending on type
	QString currency;  // for money: "INR", "USD", or a symbol
	int count = 0;     // gifts in a bomb, raiders, months
	QString tier;      // subs: "1000", "2000", "3000", "Prime"
	QDateTime at = QDateTime::currentDateTime();
	bool test = false;

	QString amountText() const; // "₹500", "100 bits", "12 months"...
	QString label() const;      // "Follow", "Super Chat"... for lists
};

struct ChatMessage {
	QString platform;
	QString id;
	QString user;
	QString color; // the name's colour, #RRGGBB, or empty
	QString text;
	bool mod = false, sub = false, vip = false, owner = false;
	QDateTime at = QDateTime::currentDateTime();
};

class Connector;

// Collects events and chat from every connected service and hands them to alerts, chat, goals and widgets.
// Duplicates (the same sub reported by Twitch and by StreamElements) are dropped.
class EventHub : public QObject {
	Q_OBJECT
public:
	static EventHub &get();

	void restart();              // (re)connects everything set up in Settings > Connections
	void stopAll();
	QStringList services() const; // keys of the connectors
	QString status(const QString &service) const;
	bool connected(const QString &service) const;

	void emitEvent(const StreamEvent &e); // connectors call these
	void emitChat(const ChatMessage &m);
	void setStatus(const QString &service, const QString &text, bool ok);
	void test(const QString &type);      // a pretend event of that type, for trying alerts

	const QVector<StreamEvent> &recent() const { return m_recent; }
	// Running totals since the counters were last reset (goals and labels read these).
	double total(const QString &key) const; // follows, subs, bits, tips (in the main currency), members
	void resetTotals();
	QString latest(const QString &type) const;      // the newest user for a type
	QString top(const QString &type) const;         // the biggest tipper/cheerer this session
	QNetworkAccessManager *network();

signals:
	void event(const StreamEvent &e);
	void chat(const ChatMessage &m);
	void statusChanged(const QString &service);
	void totalsChanged();

private:
	EventHub();
	QHash<QString, Connector *> m_connectors;
	QHash<QString, QPair<QString, bool>> m_status;
	QVector<StreamEvent> m_recent;
	QHash<QString, double> m_totals;
	QHash<QString, QString> m_latest;
	QHash<QString, QPair<QString, double>> m_top;
	QHash<QString, qint64> m_seen; // dedup key -> when
	QNetworkAccessManager *m_net = nullptr;
};

// One service. Reconnects by itself with a growing pause after a failure.
class Connector : public QObject {
	Q_OBJECT
public:
	Connector(const QString &key, QObject *parent);
	virtual void start() = 0;
	virtual void stop();
	QString key() const { return m_key; }

protected:
	void retryLater(const QString &why);
	void ok(const QString &text);
	EventHub &hub() { return EventHub::get(); }
	WebSocket *socket();
	QString m_key;
	WebSocket *m_ws = nullptr;
	QTimer *m_retry;
	int m_failures = 0;
	bool m_running = false;
};

// Twitch chat without logging in (read only). Also brings subs, resubs, gift subs, bits and raids.
class TwitchChat : public Connector {
	Q_OBJECT
public:
	explicit TwitchChat(QObject *parent);
	void start() override;
	static bool parseLine(const QString &line, ChatMessage *chat, StreamEvent *event); // true: something to emit

private:
	QString m_channel;
};

// Twitch follows (the one thing chat doesn't tell), through EventSub with the streamer's own login.
class TwitchFollows : public Connector {
	Q_OBJECT
public:
	explicit TwitchFollows(QObject *parent);
	void start() override;
	static QString clientId();
	// Device login: shows a code, the streamer confirms it on twitch.tv/activate. Calls done(error) at the end.
	static void login(std::function<void(const QString &code, const QString &url)> show,
			  std::function<void(const QString &error)> done);

private:
	void subscribe(const QString &session);
	void refresh(std::function<void(bool)> then);
};

// YouTube chat, Super Chats, Super Stickers and memberships, read from YouTube's own live chat page (no login,
// no API quota). Works with a channel (@handle, /channel/ID) while it's live, or a stream link.
class YouTubeChat : public Connector {
	Q_OBJECT
public:
	explicit YouTubeChat(QObject *parent);
	void start() override;
	void stop() override;
	static void parseActions(const QJsonArray &actions, bool alerts, QVector<ChatMessage> *chat,
				 QVector<StreamEvent> *events);

private:
	void findVideo();
	void openChat(const QString &video);
	void poll();
	QTimer *m_poll;
	QString m_key, m_version, m_continuation;
	bool m_first = true;
};

// Kick chat, subs and gifted subs, from Kick's public chat socket.
class KickChat : public Connector {
	Q_OBJECT
public:
	explicit KickChat(QObject *parent);
	void start() override;
	static bool parse(const QJsonObject &packet, ChatMessage *chat, StreamEvent *event);

private:
	void join(const QString &room);
};

// StreamElements: tips, follows, subs, cheers, raids and Super Chats from its Astro socket, with the JWT token
// from the StreamElements dashboard.
class StreamElementsEvents : public Connector {
	Q_OBJECT
public:
	explicit StreamElementsEvents(QObject *parent);
	void start() override;
	static bool parse(const QJsonObject &activity, StreamEvent *event);
};

// Services that only send webhooks (Kick follows and Kicks, Ko-fi tips), through the ZunimiLabs relay: the relay
// receives them and passes them on over one websocket. Each install has its own relay channel (relay/id + key).
class RelayEvents : public Connector {
	Q_OBJECT
public:
	explicit RelayEvents(QObject *parent);
	void start() override;
	static QString base(); // https://relay.zunimilabs.online, or ZUNIMI_RELAY_URL for testing
	static bool hasChannel();
	static void ensureChannel(std::function<void(const QString &error)> done); // makes the channel once
	static QString kofiLink(); // the webhook link to paste into Ko-fi (empty until there is a channel)
	// Asks the relay for Kick's login page and opens it in the browser.
	static void kickLogin(std::function<void(const QString &error)> done);
	static void kickLogout();
	static bool parse(const QJsonObject &message, StreamEvent *event);
};

// Streamlabs: donations and the platform events it relays, from its socket API with the socket token.
class StreamlabsEvents : public Connector {
	Q_OBJECT
public:
	explicit StreamlabsEvents(QObject *parent);
	void start() override;
	void stop() override;
	static QVector<StreamEvent> parse(const QJsonObject &payload);

private:
	QTimer *m_ping;
};
