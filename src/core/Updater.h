#pragma once
#include <QCryptographicHash>
#include <QFile>
#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

struct UpdateInfo {
	QString version;  // "0.2.0"
	QString page;     // release page on GitHub
	QString asset;    // https://github.com/<repo>/releases/download/... (the Setup .exe)
	QString apiAsset; // the same file through GitHub's API, used if the normal link fails
	QString digest;   // "sha256:..." from GitHub
	QString notes;    // first real line of the release notes
};

// Update check and download through GitHub Releases, ported from Clip Studio's updater.
// The list of releases is read instead of "latest", because GitHub's "latest" skips pre-releases.
// Only a Setup .exe from this repo's own releases is ever downloaded, and its SHA-256 must match.
class Updater : public QObject {
	Q_OBJECT
public:
	explicit Updater(QObject *parent = nullptr);

	void check(bool manual);
	void download(const UpdateInfo &info);
	void cancel();
	bool launch(const QString &installer);
	bool busy() const { return m_busy; }

	// Only an installed copy can replace itself; the zip build gets the release page instead.
	static bool installed();
	// Says if the last update did not finish and removes downloaded installers (they are never needed twice).
	QString afterStart();

	// Exposed for the self-test.
	static QList<int> parseVersion(const QString &text);
	static bool isReleaseAsset(const QString &url);

signals:
	void found(const UpdateInfo &info);
	void nothing(); // a manual check found no newer version
	void failed(const QString &reason);
	void progress(double fraction);
	void ready(const QString &installer);

private:
	void fetch();
	void fetchDone();
	void giveUp(const QString &reason);

	QNetworkAccessManager *m_net;
	QNetworkReply *m_reply = nullptr;
	bool m_busy = false;
	UpdateInfo m_info;
	QStringList m_links; // links still to try, normal one first
	int m_attempt = 0;
	QFile m_file;
	QCryptographicHash m_sha{QCryptographicHash::Sha256};
	qint64 m_done = 0;
};
