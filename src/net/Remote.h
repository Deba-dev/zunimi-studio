#pragma once
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QStringList>

#include <functional>

class ObsCore;
class QTcpServer;
class QTcpSocket;
class QTimer;

// Lets other devices drive Zunimi. It speaks obs-websocket 5 (the protocol OBS Studio uses), so the Stream Deck
// "OBS Studio" plugin, Touch Portal, Loupedeck, Streamer.bot and friends work as they are; the same port also serves
// a phone remote page. Off until the streamer turns it on; a password is always needed.
class Remote : public QObject {
	Q_OBJECT
public:
	static constexpr int DefaultPort = 4455;

	Remote(ObsCore *core, QObject *parent = nullptr);
	~Remote() override;

	// Starts (or restarts, when the settings changed) from the remote/* settings; stops when they say off.
	bool apply(QString *error);
	void stop(bool exiting = false);
	bool running() const;
	int port() const { return m_port; }
	int clients() const;

	// Runs one of the main window's actions ("live", "record", "replay_save", "mic_mute", "mark", ...).
	std::function<void(const QString &)> action;
	static QStringList actions(); // the ones a remote may call
	// Tells connected tools (Clip Studio, Stream Deck...) about a marked moment, as obs-websocket CustomEvent
	// {"zunimi": "mark", "label", "recordingSeconds"}.
	void announceMark(const QString &label, double recordingSeconds);

	static QString password();    // made the first time it is asked for
	static QString newPassword(); // makes and stores a new one
	static QStringList addresses(); // this PC's addresses on the home network, best first
	static QString phoneLink(const QString &address, int port, const QString &password);

signals:
	void clientsChanged(int count);
	void audioChanged(); // a remote muted or moved a volume

private:
	struct Client;
	void onConnection();
	void onReadyRead(Client *c);
	bool handleHttp(Client *c);
	void parseFrames(Client *c);
	void onMessage(Client *c, const QByteArray &text);
	void sendFrame(Client *c, int opcode, const QByteArray &payload);
	void send(Client *c, int op, const QJsonObject &d);
	void closeClient(Client *c, int code, const QString &reason);
	void drop(Client *c);
	QJsonObject request(const QString &type, const QJsonObject &data, int *code, QString *comment);
	void emitEvent(const QString &type, int intent, const QJsonObject &data);
	void poll();
	QJsonArray sceneList() const;
	bool blocked(const QString &ip);
	void failedLogin(const QString &ip);

	ObsCore *m_core;
	QTcpServer *m_server = nullptr;
	QTimer *m_poll;
	QHash<QTcpSocket *, Client *> m_clients;
	int m_port = 0;
	QString m_config; // what the running server was started with
	QByteArray m_salt;
	QString m_lastReplay;
	struct Fails {
		int count = 0;
		qint64 until = 0;
	};
	QHash<QString, Fails> m_fails;

	// what the last poll saw, to send events for what changed
	QString m_lastProgram, m_lastPreview;
	QStringList m_lastScenes;
	QHash<QString, QPair<bool, float>> m_lastAudio; // uuid: muted, volume
	QHash<QString, bool> m_lastItems;               // scene uuid + item id: enabled
	bool m_polled = false;
};
