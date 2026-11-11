#pragma once
#include <QDateTime>
#include <QObject>
#include <QVector>
#include <functional>

class QNetworkAccessManager;
class QTcpServer;

// Signing in to YouTube (Google OAuth, desktop flow: the browser opens, Google sends the answer back to a one-time
// address on 127.0.0.1) and reading the streamer's scheduled and live broadcasts with their stream keys.
// Read-only: the scope is youtube.readonly; Zunimi never changes anything on the channel.
class YouTubeAccount : public QObject {
	Q_OBJECT
public:
	static YouTubeAccount &get();
	static QString clientId();     // built in (ZUNIMI_GOOGLE_CLIENT_ID), or Settings youtube/clientId
	static QString clientSecret(); // Google gives desktop apps one too; it is not a real secret
	static bool configured() { return !clientId().isEmpty(); }

	bool signedIn() const;
	QString channel() const; // the channel's name, after sign-in
	void signIn();           // opens the browser; signedInChanged() or failed() follows
	void signOut();          // forgets the tokens here and asks Google to revoke them

	struct Broadcast {
		QString id, title, status; // status: "upcoming" or "live"
		QDateTime start;           // scheduled (upcoming) or actual (live) start
		QString server, key;       // where to send the stream; empty when YouTube made no stream key for it
	};
	void listBroadcasts(); // broadcastsReady() or failed()

signals:
	void signedInChanged();
	void broadcastsReady(const QVector<YouTubeAccount::Broadcast> &list);
	void failed(const QString &why);

private:
	explicit YouTubeAccount(QObject *parent = nullptr);
	void finishSignIn(const QString &code);
	void withToken(std::function<void(const QString &token)> then);
	void fetchChannel();

	QNetworkAccessManager *m_net = nullptr;
	QTcpServer *m_server = nullptr;
	QString m_verifier, m_state, m_redirect;
	QString m_access;
	QDateTime m_accessUntil;
};
