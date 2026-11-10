#include "net/Remote.h"

#include "core/ObsCore.h"
#include "core/I18n.h"
#include "core/Settings.h"
#include "version.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QPointer>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <obs.h>
#include <util/platform.h>

#include <cmath>

namespace {
const QByteArray WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const char *WS_VERSION = "5.5.2"; // the obs-websocket version whose protocol this follows
const int MAX_HEADER = 16 * 1024;
const int MAX_MESSAGE = 1024 * 1024;
const int MAX_CLIENTS = 32;

// event subscription bits (obs-websocket EventSubscription)
enum Intent { General = 1, Config = 2, Scenes = 4, Inputs = 8, Outputs = 64, SceneItems = 128, Ui = 1024 };
const int ALL_EVENTS = 2047;

// request status codes (obs-websocket RequestStatus)
enum Code {
	Success = 100,
	UnknownRequestType = 204,
	MissingRequestField = 300,
	InvalidRequestField = 400,
	OutputRunning = 500,
	OutputNotRunning = 501,
	OutputPaused = 502,
	OutputNotPaused = 503,
	StudioModeNotActive = 506,
	ResourceNotFound = 600,
	RequestProcessingFailed = 702,
};

QString randomText(int length)
{
	static const char abc[] = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
	QString out;
	for (int i = 0; i < length; ++i)
		out += QChar(abc[QRandomGenerator::system()->bounded(int(sizeof(abc) - 1))]);
	return out;
}

QByteArray sha256b64(const QByteArray &data)
{
	return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toBase64();
}

QString uuidOf(obs_source_t *source)
{
	return source ? QString::fromUtf8(obs_source_get_uuid(source)) : QString();
}

QString nameOf(obs_source_t *source)
{
	return source ? QString::fromUtf8(obs_source_get_name(source)) : QString();
}

double toDb(float mul)
{
	return mul <= 0.0f ? -100.0 : 20.0 * std::log10(double(mul));
}

QString timecode(qint64 ms)
{
	return QString("%1:%2:%3.%4")
		.arg(ms / 3600000, 2, 10, QChar('0'))
		.arg(ms / 60000 % 60, 2, 10, QChar('0'))
		.arg(ms / 1000 % 60, 2, 10, QChar('0'))
		.arg(ms % 1000, 3, 10, QChar('0'));
}

// A source by name or uuid from the request (inputName/inputUuid, sceneName/sceneUuid, ...). Caller releases.
obs_source_t *findSource(const QJsonObject &data, const QString &prefix)
{
	const QString uuid = data.value(prefix + "Uuid").toString();
	if (!uuid.isEmpty())
		return obs_get_source_by_uuid(uuid.toUtf8().constData());
	const QString name = data.value(prefix + "Name").toString();
	if (!name.isEmpty())
		return obs_get_source_by_name(name.toUtf8().constData());
	return nullptr;
}

QJsonObject itemJson(obs_sceneitem_t *item, int index)
{
	obs_source_t *source = obs_sceneitem_get_source(item);
	const bool scene = obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE;
	QJsonObject o;
	o["sceneItemId"] = qint64(obs_sceneitem_get_id(item));
	o["sceneItemIndex"] = index;
	o["sourceName"] = nameOf(source);
	o["sourceUuid"] = uuidOf(source);
	o["sourceType"] = scene ? "OBS_SOURCE_TYPE_SCENE" : "OBS_SOURCE_TYPE_INPUT";
	o["inputKind"] = scene ? QJsonValue() : QJsonValue(QString::fromUtf8(obs_source_get_id(source)));
	o["isGroup"] = obs_sceneitem_is_group(item);
	o["sceneItemEnabled"] = obs_sceneitem_visible(item);
	o["sceneItemLocked"] = obs_sceneitem_locked(item);
	return o;
}

QVector<obs_sceneitem_t *> sceneItems(obs_scene_t *scene) // bottom first, not referenced
{
	QVector<obs_sceneitem_t *> out;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *p) {
			static_cast<QVector<obs_sceneitem_t *> *>(p)->append(item);
			return true;
		},
		&out);
	return out;
}

QVector<obs_source_t *> inputs() // every input with a name, not referenced
{
	QVector<obs_source_t *> out;
	obs_enum_sources(
		[](void *p, obs_source_t *s) {
			if (obs_source_get_type(s) == OBS_SOURCE_TYPE_INPUT && obs_source_get_name(s) && *obs_source_get_name(s))
				static_cast<QVector<obs_source_t *> *>(p)->append(s);
			return true;
		},
		&out);
	return out;
}

// A field counts only when it is there and not null (some clients send every optional field, as null).
bool has(const QJsonObject &data, const char *key)
{
	return data.contains(QLatin1String(key)) && !data.value(QLatin1String(key)).isNull();
}

const char *PHONE_PAGE_FALLBACK = "<!doctype html><title>Zunimi remote</title><p>The remote page is missing.";
} // namespace

struct Remote::Client {
	QTcpSocket *socket = nullptr;
	QString ip;
	QByteArray buffer;
	bool ws = false, identified = false;
	QByteArray challenge;
	int events = ALL_EVENTS;
	QByteArray fragments;
	int fragmentOpcode = 0;
	QTimer *loginTimer = nullptr;
};

Remote::Remote(ObsCore *core, QObject *parent) : QObject(parent), m_core(core), m_poll(new QTimer(this))
{
	m_poll->setInterval(200);
	connect(m_poll, &QTimer::timeout, this, &Remote::poll);
	auto outputState = [](bool on) { return on ? "OBS_WEBSOCKET_OUTPUT_STARTED" : "OBS_WEBSOCKET_OUTPUT_STOPPED"; };
	connect(core, &ObsCore::streamingChanged, this, [this, outputState](bool on) {
		emitEvent("StreamStateChanged", Outputs, {{"outputActive", on}, {"outputState", outputState(on)}});
	});
	connect(core, &ObsCore::recordingChanged, this, [this, outputState](bool on, const QString &file) {
		emitEvent("RecordStateChanged", Outputs,
			  {{"outputActive", on}, {"outputState", outputState(on)}, {"outputPath", on ? QJsonValue() : QJsonValue(file)}});
	});
	connect(core, &ObsCore::recordingPausedChanged, this, [this](bool paused) {
		emitEvent("RecordStateChanged", Outputs,
			  {{"outputActive", true},
			   {"outputState", paused ? "OBS_WEBSOCKET_OUTPUT_PAUSED" : "OBS_WEBSOCKET_OUTPUT_RESUMED"},
			   {"outputPath", QJsonValue()}});
	});
	connect(core, &ObsCore::replayChanged, this, [this, outputState](bool on) {
		emitEvent("ReplayBufferStateChanged", Outputs, {{"outputActive", on}, {"outputState", outputState(on)}});
	});
	connect(core, &ObsCore::replaySaved, this, [this](const QString &file) {
		m_lastReplay = file;
		emitEvent("ReplayBufferSaved", Outputs, {{"savedReplayPath", file}});
	});
	connect(core, &ObsCore::virtualCamChanged, this, [this, outputState](bool on) {
		emitEvent("VirtualcamStateChanged", Outputs, {{"outputActive", on}, {"outputState", outputState(on)}});
	});
	connect(core, &ObsCore::studioModeChanged, this,
		[this](bool on) { emitEvent("StudioModeStateChanged", Ui, {{"studioModeEnabled", on}}); });
	connect(core, &ObsCore::collectionChanged, this, [this] {
		emitEvent("CurrentSceneCollectionChanged", Config, {{"sceneCollectionName", m_core->collection()}});
	});
}

Remote::~Remote()
{
	stop();
}

QStringList Remote::actions()
{
	return {"live",   "record",     "record_pause", "replay_toggle", "replay_save", "mic_mute",
		"desktop_mute", "screenshot", "studio_go", "vcam",  "mark",          "undo"};
}

QString Remote::password()
{
	QString pw = Settings::get().secret("remote/password");
	return pw.isEmpty() ? newPassword() : pw;
}

QString Remote::newPassword()
{
	const QString pw = randomText(16);
	Settings::get().setSecret("remote/password", pw);
	return pw;
}

QStringList Remote::addresses()
{
	QStringList best, other;
	for (const QNetworkInterface &nic : QNetworkInterface::allInterfaces()) {
		const auto f = nic.flags();
		if (!(f & QNetworkInterface::IsUp) || !(f & QNetworkInterface::IsRunning) || (f & QNetworkInterface::IsLoopBack))
			continue;
		const QString name = nic.humanReadableName();
		const bool virt = nic.type() == QNetworkInterface::Virtual || name.contains("vEthernet") ||
				  name.contains("VirtualBox") || name.contains("VMware") || name.contains("WSL") ||
				  name.contains("Tailscale") || name.contains("ZeroTier");
		for (const QNetworkAddressEntry &e : nic.addressEntries()) {
			const QHostAddress a = e.ip();
			if (a.protocol() != QAbstractSocket::IPv4Protocol || a.isLoopback() || a.isLinkLocal())
				continue;
			const bool home = a.isPrivateUse();
			(home && !virt && (nic.type() == QNetworkInterface::Wifi || nic.type() == QNetworkInterface::Ethernet)
				 ? best
				 : other)
				.append(a.toString());
		}
	}
	return best + other;
}

QString Remote::phoneLink(const QString &address, int port, const QString &password)
{
	// The password rides in the #part, which browsers never send over the network.
	return QString("http://%1:%2/#%3").arg(address).arg(port).arg(QString::fromUtf8(QUrl::toPercentEncoding(password)));
}

bool Remote::running() const
{
	return m_server && m_server->isListening();
}

int Remote::clients() const
{
	int n = 0;
	for (const Client *c : m_clients)
		n += c->identified;
	return n;
}

bool Remote::apply(QString *error)
{
	Settings &s = Settings::get();
	if (!s.value("remote/on", false).toBool()) {
		stop();
		return true;
	}
	const int port = s.value("remote/port", DefaultPort).toInt();
	const bool lan = s.value("remote/lan", false).toBool();
	const QString pw = password();
	const QString config = QString("%1|%2|%3").arg(port).arg(lan).arg(QString::fromUtf8(sha256b64(pw.toUtf8())));
	if (running() && config == m_config)
		return true;
	stop();
	m_server = new QTcpServer(this);
	m_server->setMaxPendingConnections(MAX_CLIENTS);
	if (!m_server->listen(lan ? QHostAddress::AnyIPv4 : QHostAddress::LocalHost, quint16(port))) {
		const QString why = m_server->errorString();
		delete m_server;
		m_server = nullptr;
		if (error)
			*error = QString("Port %1 is busy (%2). OBS Studio's own remote uses %3 too: close OBS, or pick another "
					 "port in Settings > Remote.")
					 .arg(port)
					 .arg(why)
					 .arg(DefaultPort);
		return false;
	}
	connect(m_server, &QTcpServer::newConnection, this, &Remote::onConnection);
	m_port = port;
	m_config = config;
	QByteArray raw(32, 0);
	for (char &c : raw)
		c = char(QRandomGenerator::system()->bounded(256));
	m_salt = raw.toBase64();
	m_polled = false;
	poll(); // a baseline, so the first events are real changes
	m_poll->start();
	blog(LOG_INFO, "remote: listening on %s:%d", lan ? "every network" : "this PC only", port);
	return true;
}

void Remote::stop(bool exiting)
{
	if (exiting)
		emitEvent("ExitStarted", General, {});
	for (Client *c : m_clients.values())
		closeClient(c, 1001, exiting ? "Zunimi Studio is closing." : "The remote was turned off.");
	if (m_server) {
		m_server->close();
		m_server->deleteLater();
		m_server = nullptr;
		blog(LOG_INFO, "remote: stopped");
	}
	m_poll->stop();
	m_config.clear();
}

// ---- connections

bool Remote::blocked(const QString &ip)
{
	const Fails f = m_fails.value(ip);
	return f.until > QDateTime::currentMSecsSinceEpoch();
}

void Remote::failedLogin(const QString &ip)
{
	Fails &f = m_fails[ip];
	if (++f.count >= 5) { // five wrong passwords: that address waits a minute, then two minutes, ...
		f.until = QDateTime::currentMSecsSinceEpoch() + 60000LL * (f.count - 4);
		blog(LOG_WARNING, "remote: %s blocked after %d wrong passwords", ip.toUtf8().constData(), f.count);
	}
}

void Remote::onConnection()
{
	while (m_server && m_server->hasPendingConnections()) {
		QTcpSocket *socket = m_server->nextPendingConnection();
		if (m_clients.size() >= MAX_CLIENTS) {
			socket->abort();
			socket->deleteLater();
			continue;
		}
		auto *c = new Client;
		c->socket = socket;
		QHostAddress peer = socket->peerAddress();
		bool ok = false;
		const quint32 v4 = peer.toIPv4Address(&ok);
		c->ip = ok ? QHostAddress(v4).toString() : peer.toString();
		m_clients.insert(socket, c);
		connect(socket, &QTcpSocket::readyRead, this, [this, c] { onReadyRead(c); });
		connect(socket, &QTcpSocket::disconnected, this, [this, c] { drop(c); });
		c->loginTimer = new QTimer(socket);
		c->loginTimer->setSingleShot(true);
		connect(c->loginTimer, &QTimer::timeout, this, [this, c] {
			if (c->ws)
				closeClient(c, 4007, "Log in with Identify first.");
			else
				drop(c);
		});
		c->loginTimer->start(15000);
	}
}

void Remote::drop(Client *c)
{
	if (!m_clients.contains(c->socket))
		return;
	const bool counted = c->identified;
	m_clients.remove(c->socket);
	c->socket->disconnect(this);
	c->socket->abort();
	c->socket->deleteLater();
	delete c;
	if (counted)
		emit clientsChanged(clients());
}

void Remote::closeClient(Client *c, int code, const QString &reason)
{
	if (c->ws) {
		QByteArray payload;
		payload.append(char(code >> 8)).append(char(code & 0xFF)).append(reason.toUtf8().left(120));
		sendFrame(c, 8, payload);
		c->socket->flush();
	}
	drop(c);
}

void Remote::onReadyRead(Client *c)
{
	QTcpSocket *socket = c->socket;
	c->buffer += socket->readAll();
	if (!c->ws && !handleHttp(c))
		return;
	if (m_clients.contains(socket)) // handleHttp may have dropped it
		parseFrames(c);
}

// Returns true once the connection is a websocket (frames may follow in the buffer).
bool Remote::handleHttp(Client *c)
{
	const int end = c->buffer.indexOf("\r\n\r\n");
	if (end < 0) {
		if (c->buffer.size() > MAX_HEADER)
			drop(c);
		return false;
	}
	const QList<QByteArray> lines = c->buffer.left(end).split('\n');
	c->buffer.remove(0, end + 4);
	const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
	QHash<QByteArray, QByteArray> h;
	for (int i = 1; i < lines.size(); ++i) {
		const int colon = lines[i].indexOf(':');
		if (colon > 0)
			h.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
	}
	auto reply = [c, this](const QByteArray &status, const QByteArray &type, const QByteArray &body) {
		QByteArray r = "HTTP/1.1 " + status + "\r\nContent-Type: " + type +
			       "\r\nContent-Length: " + QByteArray::number(body.size()) +
			       "\r\nCache-Control: no-store\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n"
			       "X-Content-Type-Options: nosniff\r\n"
			       "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
			       "img-src 'self' data:; connect-src *; base-uri 'none'; form-action 'none'\r\n"
			       "Connection: close\r\n\r\n" + body;
		c->socket->write(r);
		c->socket->flush();
		c->socket->disconnectFromHost();
		QTimer::singleShot(3000, c->socket, [this, c] { drop(c); });
	};
	if (first.size() < 2 || first[0] != "GET") {
		reply("405 Method Not Allowed", "text/plain", "Only GET.");
		return false;
	}
	const QByteArray path = first[1].split('?').value(0);
	if (h.value("upgrade").toLower() == "websocket" && h.contains("sec-websocket-key")) {
		if (blocked(c->ip)) {
			reply("429 Too Many Requests", "text/plain", "Too many wrong passwords. Wait a minute and try again.");
			return false;
		}
		const QByteArray accept =
			QCryptographicHash::hash(h.value("sec-websocket-key") + WS_GUID, QCryptographicHash::Sha1).toBase64();
		QByteArray r = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
			       "Sec-WebSocket-Accept: " + accept + "\r\n";
		// obs-websocket clients may ask for the JSON or the MessagePack flavour; only JSON is spoken here.
		const QList<QByteArray> protocols = h.value("sec-websocket-protocol").split(',');
		for (const QByteArray &p : protocols)
			if (p.trimmed() == "obswebsocket.json") {
				r += "Sec-WebSocket-Protocol: obswebsocket.json\r\n";
				break;
			}
		c->socket->write(r + "\r\n");
		c->ws = true;
		QByteArray raw(32, 0);
		for (char &ch : raw)
			ch = char(QRandomGenerator::system()->bounded(256));
		c->challenge = raw.toBase64();
		send(c, 0,
		     {{"obsWebSocketVersion", WS_VERSION},
		      {"rpcVersion", 1},
		      {"authentication", QJsonObject{{"challenge", QString::fromLatin1(c->challenge)},
						     {"salt", QString::fromLatin1(m_salt)}}}});
		return true;
	}
	if (path == "/" || path == "/remote" || path == "/index.html") {
		QFile f(":/remote.html");
		QByteArray page = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(PHONE_PAGE_FALLBACK);
		if (i18n::language() != "en") // the page has its own words for each language Zunimi speaks
			page.replace("<html lang=\"en\">", "<html lang=\"" + i18n::language().toLatin1() + "\">");
		reply("200 OK", "text/html; charset=utf-8", page);
	} else if (path == "/logo.png" || path == "/favicon.ico") {
		QFile f(":/logo-256.png");
		reply("200 OK", "image/png", f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray());
	} else {
		reply("404 Not Found", "text/plain", "Not here.");
	}
	return false;
}

void Remote::parseFrames(Client *c)
{
	// Anything below may drop the client (and an action may even run a dialog), so it is looked up again by its
	// socket after each message instead of trusting c.
	QPointer<QTcpSocket> socket = c->socket;
	while (socket && m_clients.contains(socket)) {
		c = m_clients.value(socket);
		QByteArray &b = c->buffer;
		if (b.size() < 2)
			return;
		const quint8 b0 = quint8(b[0]), b1 = quint8(b[1]);
		const bool fin = b0 & 0x80, masked = b1 & 0x80;
		const int opcode = b0 & 0x0F;
		quint64 len = b1 & 0x7F;
		int pos = 2;
		if (len == 126) {
			if (b.size() < 4)
				return;
			len = (quint64(quint8(b[2])) << 8) | quint8(b[3]);
			pos = 4;
		} else if (len == 127) {
			if (b.size() < 10)
				return;
			len = 0;
			for (int i = 0; i < 8; ++i)
				len = (len << 8) | quint8(b[2 + i]);
			pos = 10;
		}
		if (!masked) { // a client must mask what it sends
			closeClient(c, 1002, "Frames must be masked.");
			return;
		}
		if (len > quint64(MAX_MESSAGE)) {
			closeClient(c, 1009, "Message too big.");
			return;
		}
		if (quint64(b.size()) < pos + 4 + len)
			return;
		const QByteArray mask = b.mid(pos, 4);
		QByteArray payload = b.mid(pos + 4, int(len));
		for (int i = 0; i < payload.size(); ++i)
			payload[i] = char(payload[i] ^ mask[i % 4]);
		b.remove(0, pos + 4 + int(len));

		if (opcode == 8) {
			const int code = payload.size() >= 2 ? (quint8(payload[0]) << 8 | quint8(payload[1])) : 1000;
			Q_UNUSED(code);
			closeClient(c, 1000, {});
			return;
		}
		if (opcode == 9) {
			sendFrame(c, 10, payload);
			continue;
		}
		if (opcode == 10)
			continue;
		if (opcode == 2) {
			closeClient(c, 4002, "Only JSON text messages are understood.");
			return;
		}
		if (opcode == 1 || opcode == 0) {
			if (opcode == 1)
				c->fragments.clear();
			c->fragments += payload;
			if (c->fragments.size() > MAX_MESSAGE) {
				closeClient(c, 1009, "Message too big.");
				return;
			}
			if (fin) {
				const QByteArray text = c->fragments;
				c->fragments.clear();
				onMessage(c, text);
			}
		}
	}
}

void Remote::sendFrame(Client *c, int opcode, const QByteArray &payload)
{
	QByteArray frame;
	frame.append(char(0x80 | opcode));
	const qint64 n = payload.size();
	if (n < 126) {
		frame.append(char(n));
	} else if (n < 65536) {
		frame.append(char(126)).append(char(n >> 8)).append(char(n & 0xFF));
	} else {
		frame.append(char(127));
		for (int i = 7; i >= 0; --i)
			frame.append(char((n >> (8 * i)) & 0xFF));
	}
	c->socket->write(frame + payload);
}

void Remote::send(Client *c, int op, const QJsonObject &d)
{
	sendFrame(c, 1, QJsonDocument(QJsonObject{{"op", op}, {"d", d}}).toJson(QJsonDocument::Compact));
}

void Remote::onMessage(Client *c, const QByteArray &text)
{
	QJsonParseError err;
	const QJsonDocument doc = QJsonDocument::fromJson(text, &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject()) {
		closeClient(c, 4002, "That message is not JSON.");
		return;
	}
	const int op = doc.object().value("op").toInt(-1);
	const QJsonObject d = doc.object().value("d").toObject();
	if (op == 1) { // Identify
		if (c->identified) {
			closeClient(c, 4008, "Already identified.");
			return;
		}
		if (d.value("rpcVersion").toInt() != 1) {
			closeClient(c, 4010, "Only rpcVersion 1 is supported.");
			return;
		}
		const QByteArray secret = sha256b64(password().toUtf8() + m_salt);
		const QByteArray expected = sha256b64(secret + c->challenge);
		if (d.value("authentication").toString().toLatin1() != expected) {
			failedLogin(c->ip);
			blog(LOG_WARNING, "remote: wrong password from %s", c->ip.toUtf8().constData());
			closeClient(c, 4009, "Wrong password.");
			return;
		}
		m_fails.remove(c->ip);
		c->identified = true;
		c->loginTimer->stop();
		if (d.contains("eventSubscriptions"))
			c->events = d.value("eventSubscriptions").toInt(ALL_EVENTS);
		send(c, 2, {{"negotiatedRpcVersion", 1}});
		blog(LOG_INFO, "remote: %s connected", c->ip.toUtf8().constData());
		emit clientsChanged(clients());
		return;
	}
	if (!c->identified) {
		closeClient(c, 4007, "Log in with Identify first.");
		return;
	}
	if (op == 3) { // Reidentify
		if (d.contains("eventSubscriptions"))
			c->events = d.value("eventSubscriptions").toInt(ALL_EVENTS);
		send(c, 2, {{"negotiatedRpcVersion", 1}});
	} else if (op == 6) { // Request
		int code = Success;
		QString comment;
		const QString type = d.value("requestType").toString();
		QPointer<QTcpSocket> socket = c->socket;
		const QJsonObject out = request(type, d.value("requestData").toObject(), &code, &comment);
		if (!socket || !m_clients.contains(socket))
			return;
		c = m_clients.value(socket);
		QJsonObject status{{"result", code == Success}, {"code", code}};
		if (!comment.isEmpty())
			status["comment"] = comment;
		QJsonObject reply{{"requestType", type}, {"requestId", d.value("requestId")}, {"requestStatus", status}};
		if (code == Success && !out.isEmpty())
			reply["responseData"] = out;
		send(c, 7, reply);
	} else if (op == 8) { // RequestBatch
		QPointer<QTcpSocket> socket = c->socket;
		QJsonArray results;
		const bool halt = d.value("haltOnFailure").toBool();
		for (const QJsonValue &v : d.value("requests").toArray()) {
			const QJsonObject r = v.toObject();
			int code = Success;
			QString comment;
			const QString type = r.value("requestType").toString();
			const QJsonObject out = type == "Sleep" ? QJsonObject() : request(type, r.value("requestData").toObject(), &code, &comment);
			QJsonObject status{{"result", code == Success}, {"code", code}};
			if (!comment.isEmpty())
				status["comment"] = comment;
			QJsonObject one{{"requestType", type}, {"requestStatus", status}};
			if (r.contains("requestId"))
				one["requestId"] = r.value("requestId");
			if (code == Success && !out.isEmpty())
				one["responseData"] = out;
			results.append(one);
			if (!socket || !m_clients.contains(socket))
				return;
			if (halt && code != Success)
				break;
		}
		c = m_clients.value(socket);
		send(c, 9, {{"requestId", d.value("requestId")}, {"results", results}});
	} else {
		closeClient(c, 4006, "Unknown op code.");
	}
}

// ---- requests

QJsonArray Remote::sceneList() const
{
	// obs-websocket lists scenes bottom first: index 0 is the last scene in the list on screen.
	const QStringList names = m_core->scenes();
	QJsonArray out;
	for (int i = names.size() - 1, index = 0; i >= 0; --i, ++index) {
		obs_source_t *s = obs_get_source_by_name(names[i].toUtf8().constData());
		out.append(QJsonObject{{"sceneIndex", index}, {"sceneName", names[i]}, {"sceneUuid", uuidOf(s)}});
		obs_source_release(s);
	}
	return out;
}

QJsonObject Remote::request(const QString &type, const QJsonObject &data, int *code, QString *comment)
{
	auto fail = [code, comment](int c, const QString &why) {
		*code = c;
		*comment = why;
		return QJsonObject();
	};
	auto run = [this](const char *what) {
		if (action)
			action(what);
	};
	auto sceneRef = [](const QString &name) {
		obs_source_t *s = obs_get_source_by_name(name.toUtf8().constData());
		const QString uuid = uuidOf(s);
		obs_source_release(s);
		return uuid;
	};
	// a scene named in the request, as a source the caller releases
	auto findScene = [&data](obs_scene_t **scene) -> obs_source_t * {
		obs_source_t *s = findSource(data, "scene");
		*scene = s ? obs_scene_from_source(s) : nullptr;
		if (!*scene) {
			if (s && obs_source_is_group(s))
				*scene = obs_group_from_source(s);
			if (!*scene) {
				obs_source_release(s);
				return nullptr;
			}
		}
		return s;
	};

	// general
	if (type == "GetVersion") {
		static const QStringList kinds = {
			"GetVersion", "GetStats", "GetHotkeyList", "TriggerHotkeyByName", "BroadcastCustomEvent", "CallVendorRequest",
			"GetSceneList", "GetCurrentProgramScene", "SetCurrentProgramScene", "GetCurrentPreviewScene",
			"SetCurrentPreviewScene", "GetStudioModeEnabled", "SetStudioModeEnabled", "TriggerStudioModeTransition",
			"GetSceneCollectionList", "SetCurrentSceneCollection", "GetProfileList", "SetCurrentProfile",
			"GetInputList", "GetSpecialInputs", "GetInputMute", "SetInputMute", "ToggleInputMute", "GetInputVolume",
			"SetInputVolume", "GetSourceActive", "GetSceneItemList", "GetSceneItemId", "GetSceneItemEnabled",
			"SetSceneItemEnabled", "GetSceneItemLocked", "SetSceneItemLocked", "GetStreamStatus", "StartStream",
			"StopStream", "ToggleStream", "GetRecordStatus", "StartRecord", "StopRecord", "ToggleRecord",
			"PauseRecord", "ResumeRecord", "ToggleRecordPause", "GetRecordDirectory", "GetReplayBufferStatus",
			"StartReplayBuffer", "StopReplayBuffer", "ToggleReplayBuffer", "SaveReplayBuffer",
			"GetLastReplayBufferReplay", "GetVirtualCamStatus", "StartVirtualCam", "StopVirtualCam",
			"ToggleVirtualCam", "Sleep"};
		return {{"obsVersion", QString::fromUtf8(obs_get_version_string())},
			{"obsWebSocketVersion", WS_VERSION},
			{"rpcVersion", 1},
			{"availableRequests", QJsonArray::fromStringList(kinds)},
			{"supportedImageFormats", QJsonArray()},
			{"platform", "windows"},
			{"platformDescription", QString("%1 %2").arg(ZUNIMI_NAME, ZUNIMI_VERSION)}};
	}
	if (type == "GetStats") {
		video_t *video = obs_get_video();
		const QString folder = Settings::get().value("record/folder").toString();
		return {{"cpuUsage", 0.0},
			{"memoryUsage", double(os_get_proc_resident_size()) / (1024.0 * 1024.0)},
			{"availableDiskSpace",
			 folder.isEmpty() ? 0.0 : double(os_get_free_disk_space(folder.toUtf8().constData())) / (1024.0 * 1024.0)},
			{"activeFps", obs_get_active_fps()},
			{"averageFrameRenderTime", double(obs_get_average_frame_time_ns()) / 1000000.0},
			{"renderSkippedFrames", qint64(obs_get_lagged_frames())},
			{"renderTotalFrames", qint64(obs_get_total_frames())},
			{"outputSkippedFrames", qint64(video ? video_output_get_skipped_frames(video) : 0)},
			{"outputTotalFrames", qint64(video ? video_output_get_total_frames(video) : 0)},
			{"webSocketSessionIncomingMessages", 0},
			{"webSocketSessionOutgoingMessages", 0}};
	}
	if (type == "GetHotkeyList") {
		QJsonArray list;
		for (const QString &a : actions())
			list.append("zunimi." + a);
		return {{"hotkeys", list}};
	}
	if (type == "TriggerHotkeyByName") {
		const QString name = data.value("hotkeyName").toString();
		if (name.isEmpty())
			return fail(MissingRequestField, "hotkeyName is missing.");
		const QString a = name.startsWith("zunimi.") ? name.mid(7) : name;
		if (!actions().contains(a))
			return fail(ResourceNotFound, "No hotkey called " + name + ". GetHotkeyList lists them.");
		run(a.toUtf8().constData());
		return {};
	}
	if (type == "CallVendorRequest") {
		const QString vendor = data.value("vendorName").toString(), what = data.value("requestType").toString();
		if (vendor != "zunimi" && vendor != "zunimi-studio")
			return fail(ResourceNotFound, "Unknown vendor. Zunimi answers to \"zunimi\".");
		if (!actions().contains(what))
			return fail(UnknownRequestType, "Unknown request. Try one of: " + actions().join(", "));
		run(what.toUtf8().constData());
		return {{"vendorName", vendor}, {"requestType", what}, {"responseData", QJsonObject()}};
	}
	if (type == "BroadcastCustomEvent") {
		if (!has(data, "eventData"))
			return fail(MissingRequestField, "eventData is missing.");
		emitEvent("CustomEvent", General, data.value("eventData").toObject());
		return {};
	}

	// scenes
	if (type == "GetSceneList") {
		const bool studio = m_core->studioMode();
		return {{"currentProgramSceneName", m_core->programScene()},
			{"currentProgramSceneUuid", sceneRef(m_core->programScene())},
			{"currentPreviewSceneName", studio ? QJsonValue(m_core->currentScene()) : QJsonValue()},
			{"currentPreviewSceneUuid", studio ? QJsonValue(sceneRef(m_core->currentScene())) : QJsonValue()},
			{"scenes", sceneList()}};
	}
	if (type == "GetCurrentProgramScene") {
		const QString name = m_core->programScene(), uuid = sceneRef(name);
		return {{"sceneName", name}, {"sceneUuid", uuid}, {"currentProgramSceneName", name}, {"currentProgramSceneUuid", uuid}};
	}
	if (type == "GetCurrentPreviewScene") {
		if (!m_core->studioMode())
			return fail(StudioModeNotActive, "Studio mode is off.");
		const QString name = m_core->currentScene(), uuid = sceneRef(name);
		return {{"sceneName", name}, {"sceneUuid", uuid}, {"currentPreviewSceneName", name}, {"currentPreviewSceneUuid", uuid}};
	}
	if (type == "SetCurrentProgramScene" || type == "SetCurrentPreviewScene") {
		obs_scene_t *scene = nullptr;
		obs_source_t *s = findScene(&scene);
		if (!s)
			return fail(has(data, "sceneName") || has(data, "sceneUuid") ? ResourceNotFound : MissingRequestField,
				    "No such scene.");
		const QString name = nameOf(s);
		obs_source_release(s);
		if (!m_core->scenes().contains(name))
			return fail(ResourceNotFound, "That scene is not in this collection.");
		if (type == "SetCurrentPreviewScene") {
			if (!m_core->studioMode())
				return fail(StudioModeNotActive, "Studio mode is off.");
			m_core->setScene(name);
		} else if (m_core->studioMode()) {
			const QString preview = m_core->currentScene();
			m_core->setScene(name);
			m_core->transitionNow();
			m_core->setScene(preview); // the preview stays where the streamer left it
		} else {
			m_core->setScene(name);
		}
		return {};
	}
	if (type == "GetStudioModeEnabled")
		return {{"studioModeEnabled", m_core->studioMode()}};
	if (type == "SetStudioModeEnabled") {
		if (!has(data, "studioModeEnabled"))
			return fail(MissingRequestField, "studioModeEnabled is missing.");
		m_core->setStudioMode(data.value("studioModeEnabled").toBool());
		return {};
	}
	if (type == "TriggerStudioModeTransition") {
		if (!m_core->studioMode())
			return fail(StudioModeNotActive, "Studio mode is off.");
		m_core->transitionNow();
		return {};
	}
	if (type == "GetSceneCollectionList")
		return {{"currentSceneCollectionName", m_core->collection()},
			{"sceneCollections", QJsonArray::fromStringList(m_core->collections())}};
	if (type == "SetCurrentSceneCollection") {
		const QString name = data.value("sceneCollectionName").toString();
		if (!m_core->collections().contains(name))
			return fail(ResourceNotFound, "No scene collection called " + name + ".");
		QString error;
		if (name != m_core->collection() && !m_core->switchCollection(name, &error))
			return fail(RequestProcessingFailed, error);
		return {};
	}
	if (type == "GetProfileList")
		return {{"currentProfileName", m_core->profile()}, {"profiles", QJsonArray::fromStringList(m_core->profiles())}};
	if (type == "SetCurrentProfile") {
		const QString name = data.value("profileName").toString();
		if (!m_core->profiles().contains(name))
			return fail(ResourceNotFound, "No profile called " + name + ".");
		QString error;
		if (name != m_core->profile() && !m_core->switchProfile(name, &error))
			return fail(RequestProcessingFailed, error);
		return {};
	}

	// inputs
	if (type == "GetInputList") {
		const QString kind = data.value("inputKind").toString();
		QJsonArray list;
		for (obs_source_t *s : inputs()) {
			const QString id = QString::fromUtf8(obs_source_get_id(s));
			if (!kind.isEmpty() && id != kind)
				continue;
			list.append(QJsonObject{{"inputName", nameOf(s)},
						{"inputUuid", uuidOf(s)},
						{"inputKind", id},
						{"unversionedInputKind", QString::fromUtf8(obs_source_get_unversioned_id(s))}});
		}
		return {{"inputs", list}};
	}
	if (type == "GetSpecialInputs")
		return {{"desktop1", m_core->desktopAudio() ? QJsonValue(nameOf(m_core->desktopAudio())) : QJsonValue()},
			{"desktop2", QJsonValue()},
			{"mic1", m_core->microphone() ? QJsonValue(nameOf(m_core->microphone())) : QJsonValue()},
			{"mic2", QJsonValue()},
			{"mic3", QJsonValue()},
			{"mic4", QJsonValue()}};
	if (type == "GetInputMute" || type == "SetInputMute" || type == "ToggleInputMute" || type == "GetInputVolume" ||
	    type == "SetInputVolume" || type == "GetSourceActive") {
		obs_source_t *s = findSource(data, type == "GetSourceActive" ? "source" : "input");
		if (!s)
			return fail(has(data, "inputName") || has(data, "inputUuid") || has(data, "sourceName")
					    ? ResourceNotFound
					    : MissingRequestField,
				    "No such input.");
		QJsonObject out;
		const bool audio = obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO;
		if (type == "GetSourceActive") {
			out = {{"videoActive", obs_source_active(s)}, {"videoShowing", obs_source_showing(s)}};
		} else if (!audio) {
			obs_source_release(s);
			return fail(InvalidRequestField, "That input has no sound.");
		} else if (type == "GetInputMute") {
			out = {{"inputMuted", obs_source_muted(s)}};
		} else if (type == "SetInputMute" || type == "ToggleInputMute") {
			if (type == "SetInputMute" && !has(data, "inputMuted")) {
				obs_source_release(s);
				return fail(MissingRequestField, "inputMuted is missing.");
			}
			const bool muted = type == "SetInputMute" ? data.value("inputMuted").toBool() : !obs_source_muted(s);
			obs_source_set_muted(s, muted);
			m_core->saveAudioSettings(s);
			emit audioChanged();
			// told right away (two quick changes could fall between two looks); the next look sees no change
			m_lastAudio[uuidOf(s)].first = muted;
			emitEvent("InputMuteStateChanged", Inputs, {{"inputName", nameOf(s)}, {"inputUuid", uuidOf(s)}, {"inputMuted", muted}});
			if (type == "ToggleInputMute")
				out = {{"inputMuted", muted}};
		} else if (type == "GetInputVolume") {
			const float mul = obs_source_get_volume(s);
			out = {{"inputVolumeMul", double(mul)}, {"inputVolumeDb", toDb(mul)}};
		} else { // SetInputVolume
			double mul;
			if (has(data, "inputVolumeMul"))
				mul = data.value("inputVolumeMul").toDouble();
			else if (has(data, "inputVolumeDb"))
				mul = data.value("inputVolumeDb").toDouble() <= -100 ? 0.0
										     : std::pow(10.0, data.value("inputVolumeDb").toDouble() / 20.0);
			else {
				obs_source_release(s);
				return fail(MissingRequestField, "inputVolumeMul or inputVolumeDb is missing.");
			}
			if (!(mul >= 0.0 && mul <= 20.0)) {
				obs_source_release(s);
				return fail(InvalidRequestField, "The volume is out of range.");
			}
			obs_source_set_volume(s, float(mul));
			m_core->saveAudioSettings(s);
			emit audioChanged();
			const float now = obs_source_get_volume(s);
			m_lastAudio[uuidOf(s)].second = now;
			emitEvent("InputVolumeChanged", Inputs,
				  {{"inputName", nameOf(s)}, {"inputUuid", uuidOf(s)}, {"inputVolumeMul", double(now)}, {"inputVolumeDb", toDb(now)}});
		}
		obs_source_release(s);
		return out;
	}

	// scene items
	if (type == "GetSceneItemList" || type == "GetSceneItemId" || type == "GetSceneItemEnabled" ||
	    type == "SetSceneItemEnabled" || type == "GetSceneItemLocked" || type == "SetSceneItemLocked") {
		obs_scene_t *scene = nullptr;
		obs_source_t *s = findScene(&scene);
		if (!s)
			return fail(has(data, "sceneName") || has(data, "sceneUuid") ? ResourceNotFound : MissingRequestField,
				    "No such scene.");
		QJsonObject out;
		const QVector<obs_sceneitem_t *> items = sceneItems(scene);
		if (type == "GetSceneItemList") {
			QJsonArray list;
			for (int i = 0; i < items.size(); ++i)
				list.append(itemJson(items[i], i));
			out = {{"sceneItems", list}};
		} else if (type == "GetSceneItemId") {
			const QString name = data.value("sourceName").toString();
			const int offset = data.value("searchOffset").toInt(0);
			int seen = 0;
			for (obs_sceneitem_t *item : items)
				if (nameOf(obs_sceneitem_get_source(item)) == name && seen++ == offset) {
					out = {{"sceneItemId", qint64(obs_sceneitem_get_id(item))}};
					break;
				}
			if (out.isEmpty()) {
				obs_source_release(s);
				return fail(ResourceNotFound, "No source called " + name + " in that scene.");
			}
		} else {
			if (!has(data, "sceneItemId")) {
				obs_source_release(s);
				return fail(MissingRequestField, "sceneItemId is missing.");
			}
			obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, data.value("sceneItemId").toInteger());
			if (!item) {
				obs_source_release(s);
				return fail(ResourceNotFound, "No item with that id in that scene.");
			}
			const QString itemName = nameOf(obs_sceneitem_get_source(item));
			const bool here = scene == m_core->scene();
			if (type == "GetSceneItemEnabled") {
				out = {{"sceneItemEnabled", obs_sceneitem_visible(item)}};
			} else if (type == "GetSceneItemLocked") {
				out = {{"sceneItemLocked", obs_sceneitem_locked(item)}};
			} else if (type == "SetSceneItemEnabled") {
				const bool on = data.value("sceneItemEnabled").toBool();
				if (here)
					m_core->setVisible(itemName, on); // also refreshes the source list
				else {
					obs_sceneitem_set_visible(item, on);
					m_core->save();
				}
				const qint64 id = obs_sceneitem_get_id(item);
				m_lastItems[uuidOf(s) + "/" + QString::number(id)] = on;
				emitEvent("SceneItemEnableStateChanged", SceneItems,
					  {{"sceneName", nameOf(s)}, {"sceneUuid", uuidOf(s)}, {"sceneItemId", id}, {"sceneItemEnabled", on}});
			} else {
				const bool on = data.value("sceneItemLocked").toBool();
				if (here)
					m_core->setLocked(itemName, on);
				else {
					obs_sceneitem_set_locked(item, on);
					m_core->save();
				}
			}
		}
		obs_source_release(s);
		return out;
	}

	// outputs
	auto status = [](const OutputStats &st) {
		return QJsonObject{{"outputActive", st.active},
				   {"outputReconnecting", false},
				   {"outputTimecode", timecode(st.seconds * 1000)},
				   {"outputDuration", st.seconds * 1000},
				   {"outputCongestion", st.congestion / 100.0},
				   {"outputBytes", st.bytes},
				   {"outputSkippedFrames", st.droppedFrames},
				   {"outputTotalFrames", st.totalFrames}};
	};
	if (type == "GetStreamStatus")
		return status(m_core->streamStats());
	if (type == "StartStream" || type == "StopStream" || type == "ToggleStream") {
		const bool on = m_core->streaming();
		if (type == "StartStream" && on)
			return fail(OutputRunning, "Already live.");
		if (type == "StopStream" && !on)
			return fail(OutputNotRunning, "Not live.");
		run("live");
		return type == "ToggleStream" ? QJsonObject{{"outputActive", !on}} : QJsonObject();
	}
	if (type == "GetRecordStatus") {
		QJsonObject o = status(m_core->recordStats());
		o.remove("outputCongestion");
		o.remove("outputReconnecting");
		o["outputPaused"] = m_core->recordingPaused();
		return o;
	}
	if (type == "StartRecord" || type == "StopRecord" || type == "ToggleRecord") {
		const bool on = m_core->recording();
		if (type == "StartRecord" && on)
			return fail(OutputRunning, "Already recording.");
		if (type == "StopRecord" && !on)
			return fail(OutputNotRunning, "Not recording.");
		run("record");
		return type == "ToggleRecord" ? QJsonObject{{"outputActive", !on}} : QJsonObject();
	}
	if (type == "PauseRecord" || type == "ResumeRecord" || type == "ToggleRecordPause") {
		if (!m_core->recording())
			return fail(OutputNotRunning, "Not recording.");
		const bool paused = m_core->recordingPaused();
		if (type == "PauseRecord" && paused)
			return fail(OutputPaused, "Already paused.");
		if (type == "ResumeRecord" && !paused)
			return fail(OutputNotPaused, "Not paused.");
		run("record_pause");
		return {};
	}
	if (type == "GetRecordDirectory")
		return {{"recordDirectory", Settings::get().value("record/folder").toString()}};
	if (type == "GetReplayBufferStatus")
		return {{"outputActive", m_core->replaying()}};
	if (type == "StartReplayBuffer" || type == "StopReplayBuffer" || type == "ToggleReplayBuffer") {
		const bool on = m_core->replaying();
		if (type == "StartReplayBuffer" && on)
			return fail(OutputRunning, "The replay buffer is already on.");
		if (type == "StopReplayBuffer" && !on)
			return fail(OutputNotRunning, "The replay buffer is off.");
		run("replay_toggle");
		return type == "ToggleReplayBuffer" ? QJsonObject{{"outputActive", !on}} : QJsonObject();
	}
	if (type == "SaveReplayBuffer") {
		if (!m_core->replaying())
			return fail(OutputNotRunning, "The replay buffer is off.");
		run("replay_save");
		return {};
	}
	if (type == "GetLastReplayBufferReplay") {
		if (!m_core->replaying())
			return fail(OutputNotRunning, "The replay buffer is off.");
		return {{"savedReplayPath", m_lastReplay}};
	}
	if (type == "GetVirtualCamStatus")
		return {{"outputActive", m_core->virtualCam()}};
	if (type == "StartVirtualCam" || type == "StopVirtualCam" || type == "ToggleVirtualCam") {
		const bool on = m_core->virtualCam();
		if (type == "StartVirtualCam" && on)
			return fail(OutputRunning, "The virtual camera is already on.");
		if (type == "StopVirtualCam" && !on)
			return fail(OutputNotRunning, "The virtual camera is off.");
		run("vcam");
		return type == "ToggleVirtualCam" ? QJsonObject{{"outputActive", !on}} : QJsonObject();
	}
	return fail(UnknownRequestType, type.isEmpty() ? "requestType is missing." : "Zunimi does not know " + type + " yet.");
}

// ---- events

void Remote::announceMark(const QString &label, double recordingSeconds)
{
	emitEvent("CustomEvent", General, {{"zunimi", "mark"}, {"label", label}, {"recordingSeconds", recordingSeconds}});
}

void Remote::emitEvent(const QString &type, int intent, const QJsonObject &data)
{
	QJsonObject d{{"eventType", type}, {"eventIntent", intent}};
	if (!data.isEmpty())
		d["eventData"] = data;
	const QByteArray text = QJsonDocument(QJsonObject{{"op", 5}, {"d", d}}).toJson(QJsonDocument::Compact);
	for (Client *c : m_clients.values())
		if (c->identified && (c->events & intent))
			sendFrame(c, 1, text);
}

// Some changes have no signal of their own (a scene picked, a slider moved, a source hidden), so a cheap look five
// times a second finds them while the remote is on.
void Remote::poll()
{
	if (!running())
		return;
	const bool events = m_polled;
	m_polled = true;
	auto sceneRef = [](const QString &name) {
		obs_source_t *s = obs_get_source_by_name(name.toUtf8().constData());
		const QString uuid = uuidOf(s);
		obs_source_release(s);
		return uuid;
	};

	const QStringList scenes = m_core->scenes();
	if (scenes != m_lastScenes) {
		m_lastScenes = scenes;
		if (events)
			emitEvent("SceneListChanged", Scenes, {{"scenes", sceneList()}});
	}
	const QString program = m_core->programScene();
	if (program != m_lastProgram) {
		m_lastProgram = program;
		if (events)
			emitEvent("CurrentProgramSceneChanged", Scenes, {{"sceneName", program}, {"sceneUuid", sceneRef(program)}});
	}
	const QString preview = m_core->studioMode() ? m_core->currentScene() : QString();
	if (preview != m_lastPreview) {
		m_lastPreview = preview;
		if (events && !preview.isEmpty())
			emitEvent("CurrentPreviewSceneChanged", Scenes, {{"sceneName", preview}, {"sceneUuid", sceneRef(preview)}});
	}

	QHash<QString, QPair<bool, float>> audio;
	for (obs_source_t *s : inputs()) {
		if (!(obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO))
			continue;
		const QString uuid = uuidOf(s);
		const QPair<bool, float> now{obs_source_muted(s), obs_source_get_volume(s)};
		audio.insert(uuid, now);
		const auto was = m_lastAudio.constFind(uuid);
		if (!events || was == m_lastAudio.constEnd())
			continue;
		if (was->first != now.first)
			emitEvent("InputMuteStateChanged", Inputs, {{"inputName", nameOf(s)}, {"inputUuid", uuid}, {"inputMuted", now.first}});
		if (was->second != now.second)
			emitEvent("InputVolumeChanged", Inputs,
				  {{"inputName", nameOf(s)}, {"inputUuid", uuid}, {"inputVolumeMul", double(now.second)}, {"inputVolumeDb", toDb(now.second)}});
	}
	m_lastAudio = audio;

	QHash<QString, bool> items;
	for (const QString &name : scenes) {
		obs_source_t *src = obs_get_source_by_name(name.toUtf8().constData());
		obs_scene_t *scene = src ? obs_scene_from_source(src) : nullptr;
		if (scene) {
			const QString uuid = uuidOf(src);
			for (obs_sceneitem_t *item : sceneItems(scene)) {
				const qint64 id = obs_sceneitem_get_id(item);
				const QString key = uuid + "/" + QString::number(id);
				const bool on = obs_sceneitem_visible(item);
				items.insert(key, on);
				const auto was = m_lastItems.constFind(key);
				if (events && was != m_lastItems.constEnd() && *was != on)
					emitEvent("SceneItemEnableStateChanged", SceneItems,
						  {{"sceneName", name}, {"sceneUuid", uuid}, {"sceneItemId", id}, {"sceneItemEnabled", on}});
			}
		}
		obs_source_release(src);
	}
	m_lastItems = items;
}
