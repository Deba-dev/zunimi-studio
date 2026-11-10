#include "core/Updater.h"

#include "core/Settings.h"
#include "version.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

#include <util/base.h>

namespace {
const QString REPO = "Deba-dev/zunimi-studio";
const QString API = "https://api.github.com/repos/" + REPO + "/releases?per_page=20";
const QString API_ASSET = "https://api.github.com/repos/" + REPO + "/releases/assets/";
const QByteArray AGENT = "ZunimiStudio-updater";
const int TRIES = 2;            // per link: GitHub's download links sometimes answer 502/503/504 for a while
const int RETRY_WAIT_MS = 3000;
const QString INSTALLER_GLOB = "ZunimiStudio-Setup-*.exe";

bool isApiAsset(const QString &url)
{
	static const QRegularExpression re("^" + QRegularExpression::escape(API_ASSET) + "\\d+$",
					   QRegularExpression::CaseInsensitiveOption);
	return re.match(url).hasMatch();
}

QString summary(const QString &notes)
{
	for (QString line : notes.split('\n')) {
		line = line.trimmed();
		if (!line.isEmpty() && !line.startsWith('#'))
			return line.remove(QRegularExpression("[*`_]")).left(140);
	}
	return {};
}

bool newer(const QList<int> &a, const QList<int> &b)
{
	return a > b; // QList compares element by element
}

QString installerPath(const QString &version)
{
	return QDir::temp().filePath(QString("ZunimiStudio-Setup-%1.exe").arg(version));
}

QString appRoot()
{
	return QDir(QCoreApplication::applicationDirPath() + "/../..").absolutePath(); // exe is in bin\64bit
}
} // namespace

QList<int> Updater::parseVersion(const QString &text)
{
	// v1.2.3 -> 1,2,3,0. Only the leading numbers count, so a suffix is never read as a version.
	static const QRegularExpression re("^\\s*[vV]?(\\d+(?:\\.\\d+){0,3})");
	QList<int> numbers;
	const auto m = re.match(text);
	if (m.hasMatch())
		for (const QString &part : m.captured(1).split('.'))
			numbers << part.toInt();
	while (numbers.size() < 4)
		numbers << 0;
	return numbers;
}

bool Updater::isReleaseAsset(const QString &text)
{
	const QUrl url(text, QUrl::StrictMode);
	const QString path = url.path(QUrl::FullyDecoded);
	if (!url.isValid() || url.scheme() != "https" || url.host() != "github.com" || !url.userInfo().isEmpty() ||
	    url.port(-1) != -1)
		return false;
	if (!path.startsWith("/" + REPO + "/releases/download/", Qt::CaseInsensitive))
		return false;
	for (const QString &part : path.split('/'))
		if (part == "." || part == "..")
			return false;
	return true;
}

bool Updater::installed()
{
	return QFile::exists(appRoot() + "/unins000.exe");
}

Updater::Updater(QObject *parent) : QObject(parent), m_net(new QNetworkAccessManager(this))
{
	m_net->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

void Updater::check(bool manual)
{
	if (m_busy)
		return;
	m_busy = true;
	QNetworkRequest request{QUrl(API)};
	request.setRawHeader("User-Agent", AGENT);
	request.setRawHeader("Accept", "application/vnd.github+json");
	request.setTransferTimeout(15000);
	QNetworkReply *reply = m_net->get(request);
	connect(reply, &QNetworkReply::finished, this, [this, reply, manual] {
		reply->deleteLater();
		m_busy = false;
		if (reply->error() != QNetworkReply::NoError) {
			blog(LOG_WARNING, "update check failed: %s", reply->errorString().toUtf8().constData());
			if (manual) // a failed background check must never bother anyone
				emit failed("Could not check for updates (" + reply->errorString().left(60) + ").");
			return;
		}
		// ZUNIMI_PRETEND_VERSION lets the self-test act as an older build and fetch a real installer
		const QList<int> current = parseVersion(qEnvironmentVariable("ZUNIMI_PRETEND_VERSION", ZUNIMI_VERSION));
		UpdateInfo best;
		QList<int> bestVersion = current;
		for (const QJsonValue &value : QJsonDocument::fromJson(reply->readAll()).array()) {
			const QJsonObject release = value.toObject();
			if (release["draft"].toBool())
				continue;
			const QList<int> version = parseVersion(release["tag_name"].toString());
			if (!newer(version, bestVersion))
				continue;
			for (const QJsonValue &a : release["assets"].toArray()) {
				const QJsonObject asset = a.toObject();
				const QString link = asset["browser_download_url"].toString();
				if (asset["name"].toString().compare("ZunimiStudio-Setup.exe", Qt::CaseInsensitive) != 0 || // not the browser add-on
				    !isReleaseAsset(link))
					continue;
				best.version = release["tag_name"].toString().remove(QRegularExpression("^[vV]"));
				best.page = release["html_url"].toString();
				best.asset = link;
				best.digest = asset["digest"].toString();
				best.apiAsset = isApiAsset(asset["url"].toString()) ? asset["url"].toString() : QString();
				best.notes = summary(release["body"].toString());
				bestVersion = version;
				break;
			}
		}
		if (!best.version.isEmpty()) {
			blog(LOG_INFO, "update available: %s", best.version.toUtf8().constData());
			emit found(best);
		} else if (manual) {
			emit nothing();
		}
	});
}

void Updater::download(const UpdateInfo &info)
{
	if (m_busy)
		return;
	if (!isReleaseAsset(info.asset)) {
		emit failed("That update did not come from Zunimi Studio's releases, so it was not downloaded.");
		return;
	}
	m_busy = true;
	m_info = info;
	m_links = {info.asset};
	if (isApiAsset(info.apiAsset))
		m_links << info.apiAsset;
	m_attempt = 0;
	fetch();
}

void Updater::fetch()
{
	m_file.setFileName(installerPath(m_info.version));
	if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		giveUp("The update could not be saved in your Temp folder (" + m_file.errorString() + ").");
		return;
	}
	m_sha.reset();
	m_done = 0;
	const QString link = m_links.first();
	QNetworkRequest request{QUrl(link)};
	request.setRawHeader("User-Agent", AGENT);
	if (link.startsWith(API_ASSET))
		request.setRawHeader("Accept", "application/octet-stream");
	request.setTransferTimeout(30000); // only fires when nothing arrives for 30 s
	m_reply = m_net->get(request);
	connect(m_reply, &QNetworkReply::readyRead, this, [this] {
		const QByteArray chunk = m_reply->readAll();
		m_file.write(chunk);
		m_sha.addData(chunk);
		m_done += chunk.size();
	});
	connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
		if (total > 0)
			emit progress(double(got) / double(total));
	});
	connect(m_reply, &QNetworkReply::finished, this, &Updater::fetchDone);
}

void Updater::fetchDone()
{
	QNetworkReply *reply = m_reply;
	m_reply = nullptr;
	reply->deleteLater();
	m_file.write(reply->readAll());
	m_file.close();

	const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (reply->error() == QNetworkReply::OperationCanceledError && !m_busy) {
		m_file.remove();
		return; // cancelled by the user
	}
	if (reply->error() != QNetworkReply::NoError) {
		m_file.remove();
		blog(LOG_WARNING, "update download failed (%d): %s", status, reply->errorString().toUtf8().constData());
		const bool serverTrouble = status == 0 || status >= 500 || status == 429;
		if (serverTrouble && ++m_attempt < TRIES) {
			QTimer::singleShot(RETRY_WAIT_MS, this, &Updater::fetch);
			return;
		}
		m_links.removeFirst();
		m_attempt = 0;
		if (!m_links.isEmpty()) {
			fetch();
			return;
		}
		giveUp(status >= 500 ? "GitHub's download server is not answering right now. Try the update again in a "
				       "little while."
				     : "The update download failed (" + reply->errorString().left(60) + ").");
		return;
	}
	const qint64 total = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
	const QString want = m_info.digest.toLower();
	if ((total > 0 && m_done != total) ||
	    (want.startsWith("sha256:") && want.mid(7) != QString::fromLatin1(m_sha.result().toHex()))) {
		m_file.remove();
		giveUp("The update did not download correctly, so it was thrown away. Please try again.");
		return;
	}
	m_busy = false;
	Settings::get().set("update/pending", m_info.version);
	emit ready(m_file.fileName());
}

void Updater::giveUp(const QString &reason)
{
	m_busy = false;
	emit failed(reason);
}

void Updater::cancel()
{
	if (!m_reply)
		return;
	m_busy = false;
	m_reply->abort();
}

bool Updater::launch(const QString &installer)
{
	const QString log = QDir::toNativeSeparators(Settings::dataDir() + "/update.log");
	return QProcess::startDetached(installer, {"/SILENT", "/SP-", "/SUPPRESSMSGBOXES", "/NORESTART",
						    "/CLOSEAPPLICATIONS", "/LOG=" + log});
}

QString Updater::afterStart()
{
	QString message;
	Settings &s = Settings::get();
	const QString pending = s.value("update/pending").toString();
	if (!pending.isEmpty()) {
		s.set("update/pending", QString());
		if (newer(parseVersion(pending), parseVersion(ZUNIMI_VERSION)))
			message = QString("The update to %1 did not finish, so you are still on %2.").arg(pending,
												 ZUNIMI_VERSION);
	}
	QDir temp = QDir::temp();
	for (const QString &name : temp.entryList({INSTALLER_GLOB}, QDir::Files))
		temp.remove(name);
	return message;
}
