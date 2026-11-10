#pragma once
#include <QByteArray>
#include <QObject>
#include <QUrl>

class QSslSocket;
class QTimer;

// A small websocket client (RFC 6455) for ws:// and wss://: text messages, ping/pong, close. Enough for chat and
// alert services, without pulling in another Qt module.
class WebSocket : public QObject {
	Q_OBJECT
public:
	explicit WebSocket(QObject *parent = nullptr);
	~WebSocket() override;

	void open(const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &headers = {});
	void close();
	bool isOpen() const { return m_state == State::Open; }
	void sendText(const QString &text);

signals:
	void opened();
	void closed(const QString &why); // also after a failed connect
	void textMessage(const QString &text);

private:
	enum class State { Closed, Connecting, Handshake, Open };
	void onReadyRead();
	void parseFrames();
	void sendFrame(int opcode, const QByteArray &payload);
	void fail(const QString &why);

	QSslSocket *m_socket;
	QTimer *m_timeout;
	State m_state = State::Closed;
	QUrl m_url;
	QList<QPair<QByteArray, QByteArray>> m_headers;
	QByteArray m_key;
	QByteArray m_buffer;
	QByteArray m_fragments;
	int m_fragmentOpcode = 0;
};
