#include "net/YouTubeAccount.h"

#include "core/I18n.h"
#include "core/Settings.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <memory>

// A Google Cloud project made for Zunimi Stream+ (OAuth client type "Desktop app"). Set both at build time
// (-DZUNIMI_GOOGLE_CLIENT_ID=... -DZUNIMI_GOOGLE_CLIENT_SECRET=...) or in Settings (youtube/clientId, clientSecret).
#ifndef ZUNIMI_GOOGLE_CLIENT_ID
#define ZUNIMI_GOOGLE_CLIENT_ID ""
#endif
#ifndef ZUNIMI_GOOGLE_CLIENT_SECRET
#define ZUNIMI_GOOGLE_CLIENT_SECRET ""
#endif

namespace {
const char *SCOPE = "https://www.googleapis.com/auth/youtube.readonly";
const char *API = "https://www.googleapis.com/youtube/v3/";

QString randomText(int length)
{
	static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
	QString out;
	for (int i = 0; i < length; i++)
		out += QChar(chars[QRandomGenerator::system()->bounded(int(sizeof(chars) - 1))]);
	return out;
}

QString googleError(const QJsonObject &o, const QString &fallback)
{
	if (o.value("error").isObject())
		return o.value("error").toObject().value("message").toString(fallback);
	const QString e = o.value("error_description").toString(o.value("error").toString());
	return e.isEmpty() ? fallback : e;
}
} // namespace

YouTubeAccount &YouTubeAccount::get()
{
	static YouTubeAccount *account = new YouTubeAccount;
	return *account;
}

YouTubeAccount::YouTubeAccount(QObject *parent) : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

QString YouTubeAccount::clientId()
{
	const QString custom = Settings::get().value("youtube/clientId").toString().trimmed();
	return custom.isEmpty() ? QString(ZUNIMI_GOOGLE_CLIENT_ID) : custom;
}

QString YouTubeAccount::clientSecret()
{
	const QString custom = Settings::get().secret("youtube/clientSecret").trimmed();
	return custom.isEmpty() ? QString(ZUNIMI_GOOGLE_CLIENT_SECRET) : custom;
}

bool YouTubeAccount::signedIn() const
{
	return !Settings::get().secret("youtube/refresh").isEmpty();
}

QString YouTubeAccount::channel() const
{
	return Settings::get().value("youtube/channel").toString();
}

void YouTubeAccount::signIn()
{
	if (!configured()) {
		emit failed(L("This copy of Zunimi Stream+ has no Google sign-in set up yet."));
		return;
	}
	delete m_server;
	m_server = new QTcpServer(this);
	if (!m_server->listen(QHostAddress::LocalHost, 0)) {
		emit failed(L("Could not open the sign-in return address on this PC."));
		return;
	}
	m_redirect = QString("http://127.0.0.1:%1").arg(m_server->serverPort());
	m_verifier = randomText(64);
	m_state = randomText(24);
	const QByteArray challenge =
		QCryptographicHash::hash(m_verifier.toLatin1(), QCryptographicHash::Sha256)
			.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
	QUrlQuery q;
	q.addQueryItem("client_id", clientId());
	q.addQueryItem("redirect_uri", m_redirect);
	q.addQueryItem("response_type", "code");
	q.addQueryItem("scope", SCOPE);
	q.addQueryItem("code_challenge", QString::fromLatin1(challenge));
	q.addQueryItem("code_challenge_method", "S256");
	q.addQueryItem("state", m_state);
	q.addQueryItem("access_type", "offline"); // a refresh token, so one sign-in lasts
	q.addQueryItem("prompt", "consent");
	QUrl url("https://accounts.google.com/o/oauth2/v2/auth");
	url.setQuery(q);

	QTcpServer *server = m_server;
	connect(server, &QTcpServer::newConnection, this, [this, server] {
		QTcpSocket *socket = server->nextPendingConnection();
		if (!socket)
			return;
		connect(socket, &QTcpSocket::readyRead, this, [this, socket, server] {
			const QString line = QString::fromLatin1(socket->readLine()); // GET /?code=...&state=... HTTP/1.1
			const QUrlQuery answer(QUrl("http://x" + line.section(' ', 1, 1)).query());
			const bool ok = answer.hasQueryItem("code") && answer.queryItemValue("state") == m_state;
			const QString page = ok ? L("Signed in. You can close this tab and go back to Zunimi Stream+.")
						: L("Sign-in was cancelled or failed. You can close this tab.");
			const QByteArray body = "<!doctype html><meta charset=utf-8><title>Zunimi Stream+</title>"
						"<body style='font-family:sans-serif;background:#0a0716;color:#ece9fb;padding:40px'>" +
						page.toHtmlEscaped().toUtf8() + "</body>";
			socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: " +
				      QByteArray::number(body.size()) + "\r\n\r\n" + body);
			socket->disconnectFromHost();
			if (!line.startsWith("GET /?"))
				return; // the browser asking for a favicon and the like
			server->close();
			if (ok)
				finishSignIn(answer.queryItemValue("code"));
			else
				emit failed(answer.queryItemValue("error").isEmpty() ? L("Sign-in was cancelled.")
										     : answer.queryItemValue("error"));
		});
		connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
	});
	QTimer::singleShot(5 * 60 * 1000, server, [server] { server->close(); }); // nobody finished: stop listening
	QDesktopServices::openUrl(url);
}

void YouTubeAccount::finishSignIn(const QString &code)
{
	QUrlQuery form;
	form.addQueryItem("code", code);
	form.addQueryItem("client_id", clientId());
	form.addQueryItem("client_secret", clientSecret());
	form.addQueryItem("redirect_uri", m_redirect);
	form.addQueryItem("grant_type", "authorization_code");
	form.addQueryItem("code_verifier", m_verifier);
	QNetworkRequest req(QUrl("https://oauth2.googleapis.com/token"));
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
	QNetworkReply *reply = m_net->post(req, form.toString(QUrl::FullyEncoded).toUtf8());
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		reply->deleteLater();
		const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
		const QString refresh = o.value("refresh_token").toString();
		if (refresh.isEmpty()) {
			emit failed(googleError(o, L("Google did not finish the sign-in.")));
			return;
		}
		Settings::get().setSecret("youtube/refresh", refresh);
		m_access = o.value("access_token").toString();
		m_accessUntil = QDateTime::currentDateTimeUtc().addSecs(o.value("expires_in").toInt(3600) - 60);
		fetchChannel();
	});
}

void YouTubeAccount::fetchChannel()
{
	withToken([this](const QString &token) {
		QNetworkRequest req(QUrl(QString(API) + "channels?part=snippet&mine=true"));
		req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
		QNetworkReply *reply = m_net->get(req);
		connect(reply, &QNetworkReply::finished, this, [this, reply] {
			reply->deleteLater();
			const QJsonArray items = QJsonDocument::fromJson(reply->readAll()).object().value("items").toArray();
			Settings::get().set("youtube/channel", items.isEmpty() ? QString()
									       : items[0].toObject().value("snippet").toObject().value("title").toString());
			emit signedInChanged();
		});
	});
}

void YouTubeAccount::signOut()
{
	const QString refresh = Settings::get().secret("youtube/refresh");
	if (!refresh.isEmpty()) {
		QNetworkRequest req(QUrl("https://oauth2.googleapis.com/revoke"));
		req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
		QNetworkReply *reply = m_net->post(req, "token=" + QUrl::toPercentEncoding(refresh));
		connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
	}
	Settings::get().setSecret("youtube/refresh", QString());
	Settings::get().set("youtube/channel", QString());
	m_access.clear();
	emit signedInChanged();
}

void YouTubeAccount::withToken(std::function<void(const QString &)> then)
{
	if (!m_access.isEmpty() && QDateTime::currentDateTimeUtc() < m_accessUntil) {
		then(m_access);
		return;
	}
	const QString refresh = Settings::get().secret("youtube/refresh");
	if (refresh.isEmpty()) {
		emit failed(L("Sign in to YouTube first."));
		return;
	}
	QUrlQuery form;
	form.addQueryItem("client_id", clientId());
	form.addQueryItem("client_secret", clientSecret());
	form.addQueryItem("refresh_token", refresh);
	form.addQueryItem("grant_type", "refresh_token");
	QNetworkRequest req(QUrl("https://oauth2.googleapis.com/token"));
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
	QNetworkReply *reply = m_net->post(req, form.toString(QUrl::FullyEncoded).toUtf8());
	connect(reply, &QNetworkReply::finished, this, [this, reply, then] {
		reply->deleteLater();
		const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
		if (o.value("access_token").toString().isEmpty()) {
			if (o.value("error").toString() == "invalid_grant") { // revoked, or a test-mode token that expired
				Settings::get().setSecret("youtube/refresh", QString());
				emit signedInChanged();
				emit failed(L("YouTube signed you out. Sign in again."));
			} else {
				emit failed(googleError(o, L("Could not reach YouTube.")));
			}
			return;
		}
		m_access = o.value("access_token").toString();
		m_accessUntil = QDateTime::currentDateTimeUtc().addSecs(o.value("expires_in").toInt(3600) - 60);
		then(m_access);
	});
}

void YouTubeAccount::listBroadcasts()
{
	withToken([this](const QString &token) {
		// "upcoming" and "active" need one call each; then one more for the stream keys they are bound to
		auto list = std::make_shared<QVector<Broadcast>>();
		auto streams = std::make_shared<QHash<QString, QStringList>>(); // broadcast id -> bound stream id
		auto pending = std::make_shared<int>(2);
		auto error = std::make_shared<QString>();
		auto finish = [this, token, list, streams, error] {
			if (!error->isEmpty()) {
				emit failed(*error);
				return;
			}
			QStringList ids;
			for (const QStringList &s : *streams)
				ids << s;
			ids.removeDuplicates();
			auto sorted = [list] {
				std::sort(list->begin(), list->end(), [](const Broadcast &a, const Broadcast &b) {
					if (a.status != b.status)
						return a.status == "live";
					return a.start < b.start;
				});
			};
			if (ids.isEmpty()) {
				sorted();
				emit broadcastsReady(*list);
				return;
			}
			QNetworkRequest req(QUrl(QString(API) + "liveStreams?part=cdn&maxResults=50&id=" + ids.join(',')));
			req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
			QNetworkReply *reply = m_net->get(req);
			connect(reply, &QNetworkReply::finished, this, [this, reply, list, streams, sorted] {
				reply->deleteLater();
				QHash<QString, QPair<QString, QString>> keys;
				for (const QJsonValue &v : QJsonDocument::fromJson(reply->readAll()).object().value("items").toArray()) {
					const QJsonObject info = v.toObject().value("cdn").toObject().value("ingestionInfo").toObject();
					const QString server = info.value("rtmpsIngestionAddress").toString(info.value("ingestionAddress").toString());
					keys.insert(v.toObject().value("id").toString(), {server, info.value("streamName").toString()});
				}
				for (Broadcast &b : *list) {
					const QString sid = streams->value(b.id).value(0);
					if (keys.contains(sid)) {
						b.server = keys[sid].first;
						b.key = keys[sid].second;
					}
				}
				sorted();
				emit broadcastsReady(*list);
			});
		};
		for (const char *status : {"upcoming", "active"}) {
			QNetworkRequest req(QUrl(QString(API) + "liveBroadcasts?part=snippet,contentDetails,status&maxResults=25"
							       "&broadcastType=all&broadcastStatus=" + status));
			req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
			QNetworkReply *reply = m_net->get(req);
			connect(reply, &QNetworkReply::finished, this, [reply, list, streams, pending, error, finish, status] {
				reply->deleteLater();
				const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
				if (o.contains("error"))
					*error = googleError(o, L("YouTube did not list your streams."));
				for (const QJsonValue &v : o.value("items").toArray()) {
					const QJsonObject item = v.toObject(), snippet = item.value("snippet").toObject();
					Broadcast b;
					b.id = item.value("id").toString();
					b.title = snippet.value("title").toString();
					b.status = QString(status) == "active" ? "live" : "upcoming";
					b.start = QDateTime::fromString(snippet.value(b.status == "live" ? "actualStartTime" : "scheduledStartTime").toString(),
									Qt::ISODate);
					const QString bound = item.value("contentDetails").toObject().value("boundStreamId").toString();
					if (!bound.isEmpty())
						(*streams)[b.id] << bound;
					list->append(b);
				}
				if (--*pending == 0)
					finish();
			});
		}
	});
}
