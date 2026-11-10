#include "net/WebSocket.h"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QSslSocket>
#include <QTimer>

namespace {
const QByteArray GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const int MAX_MESSAGE = 16 * 1024 * 1024;
} // namespace

WebSocket::WebSocket(QObject *parent) : QObject(parent), m_socket(new QSslSocket(this)), m_timeout(new QTimer(this))
{
	m_timeout->setSingleShot(true);
	connect(m_timeout, &QTimer::timeout, this, [this] { fail("Timed out connecting."); });
	connect(m_socket, &QSslSocket::readyRead, this, &WebSocket::onReadyRead);
	connect(m_socket, &QAbstractSocket::disconnected, this, [this] {
		if (m_state != State::Closed)
			fail("The connection closed.");
	});
	connect(m_socket, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
		if (m_state != State::Closed)
			fail(m_socket->errorString());
	});
	auto handshake = [this] {
		m_state = State::Handshake;
		QByteArray raw(16, 0);
		for (char &c : raw)
			c = char(QRandomGenerator::global()->bounded(256));
		m_key = raw.toBase64();
		QByteArray path = m_url.path(QUrl::FullyEncoded).toUtf8();
		if (path.isEmpty())
			path = "/";
		if (m_url.hasQuery())
			path += "?" + m_url.query(QUrl::FullyEncoded).toUtf8();
		QByteArray req = "GET " + path + " HTTP/1.1\r\nHost: " + m_url.host().toUtf8() +
				 "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
				 "Sec-WebSocket-Key: " + m_key + "\r\nUser-Agent: ZunimiStudio\r\n";
		for (const auto &[k, v] : m_headers)
			req += k + ": " + v + "\r\n";
		m_socket->write(req + "\r\n");
	};
	connect(m_socket, &QSslSocket::encrypted, this, handshake);
	connect(m_socket, &QAbstractSocket::connected, this, [this, handshake] {
		if (m_url.scheme() == "ws")
			handshake();
	});
}

WebSocket::~WebSocket()
{
	m_state = State::Closed;
	m_socket->abort();
}

void WebSocket::open(const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &headers)
{
	close();
	m_url = url;
	m_headers = headers;
	m_buffer.clear();
	m_fragments.clear();
	m_state = State::Connecting;
	m_timeout->start(15000);
	const bool secure = url.scheme() == "wss";
	const quint16 port = quint16(url.port(secure ? 443 : 80));
	if (secure)
		m_socket->connectToHostEncrypted(url.host(), port);
	else
		m_socket->connectToHost(url.host(), port);
}

void WebSocket::close()
{
	if (m_state == State::Open)
		sendFrame(8, {});
	m_state = State::Closed;
	m_timeout->stop();
	m_socket->abort();
}

void WebSocket::fail(const QString &why)
{
	const bool was = m_state != State::Closed;
	m_state = State::Closed;
	m_timeout->stop();
	m_socket->abort();
	if (was)
		emit closed(why);
}

void WebSocket::sendText(const QString &text)
{
	if (m_state == State::Open)
		sendFrame(1, text.toUtf8());
}

void WebSocket::sendFrame(int opcode, const QByteArray &payload)
{
	QByteArray frame;
	frame.append(char(0x80 | opcode));
	const qint64 n = payload.size();
	if (n < 126) {
		frame.append(char(0x80 | n));
	} else if (n < 65536) {
		frame.append(char(0x80 | 126));
		frame.append(char((n >> 8) & 0xFF));
		frame.append(char(n & 0xFF));
	} else {
		frame.append(char(0x80 | 127));
		for (int i = 7; i >= 0; i--)
			frame.append(char((n >> (8 * i)) & 0xFF));
	}
	char mask[4];
	for (char &c : mask)
		c = char(QRandomGenerator::global()->bounded(256));
	frame.append(mask, 4);
	QByteArray body = payload;
	for (qint64 i = 0; i < n; i++)
		body[i] = char(body[i] ^ mask[i % 4]);
	m_socket->write(frame + body);
}

void WebSocket::onReadyRead()
{
	m_buffer += m_socket->readAll();
	if (m_state == State::Handshake) {
		const int end = int(m_buffer.indexOf("\r\n\r\n"));
		if (end < 0) {
			if (m_buffer.size() > 65536)
				fail("The server's answer was not a websocket.");
			return;
		}
		const QByteArray head = m_buffer.left(end);
		m_buffer.remove(0, end + 4);
		const QByteArray expect =
			QCryptographicHash::hash(m_key + GUID, QCryptographicHash::Sha1).toBase64();
		if (!head.startsWith("HTTP/1.1 101") || !head.toLower().contains("sec-websocket-accept: " + expect.toLower())) {
			fail("The server refused the connection (" + QString::fromUtf8(head.left(head.indexOf('\r'))) + ").");
			return;
		}
		m_state = State::Open;
		m_timeout->stop();
		emit opened();
	}
	if (m_state == State::Open)
		parseFrames();
}

void WebSocket::parseFrames()
{
	while (m_state == State::Open && m_buffer.size() >= 2) {
		const uchar b0 = uchar(m_buffer[0]), b1 = uchar(m_buffer[1]);
		const bool fin = b0 & 0x80;
		const int opcode = b0 & 0x0F;
		const bool masked = b1 & 0x80;
		qint64 len = b1 & 0x7F;
		int pos = 2;
		if (len == 126) {
			if (m_buffer.size() < 4)
				return;
			len = (qint64(uchar(m_buffer[2])) << 8) | uchar(m_buffer[3]);
			pos = 4;
		} else if (len == 127) {
			if (m_buffer.size() < 10)
				return;
			len = 0;
			for (int i = 0; i < 8; i++)
				len = (len << 8) | uchar(m_buffer[2 + i]);
			pos = 10;
		}
		if (len > MAX_MESSAGE) {
			fail("A message was too big.");
			return;
		}
		const int maskPos = pos;
		if (masked)
			pos += 4;
		if (m_buffer.size() < pos + len)
			return;
		QByteArray payload = m_buffer.mid(pos, int(len));
		if (masked)
			for (qint64 i = 0; i < len; i++)
				payload[i] = char(payload[i] ^ m_buffer[maskPos + int(i % 4)]);
		m_buffer.remove(0, int(pos + len));

		switch (opcode) {
		case 0: // continuation
			m_fragments += payload;
			if (fin) {
				if (m_fragmentOpcode == 1)
					emit textMessage(QString::fromUtf8(m_fragments));
				m_fragments.clear();
			}
			break;
		case 1:
		case 2:
			if (fin) {
				if (opcode == 1)
					emit textMessage(QString::fromUtf8(payload));
			} else {
				m_fragmentOpcode = opcode;
				m_fragments = payload;
			}
			break;
		case 8:
			sendFrame(8, {});
			fail("The server closed the connection.");
			return;
		case 9:
			sendFrame(10, payload);
			break;
		default:
			break; // pong and unknown frames
		}
	}
}
