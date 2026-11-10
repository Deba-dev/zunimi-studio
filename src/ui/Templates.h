#pragma once
#include <QDialog>
#include <QImage>
#include <QJsonObject>
#include <QString>
#include <QVector>

class ObsCore;
class QButtonGroup;
class QLabel;
class QListWidget;

// Ready-made scenes (Starting soon, Be right back, Stream ending, Gaming, Just chatting) in a few styles. The
// backgrounds and frames are drawn here at the canvas size and the widgets are Zunimi's own, so nothing is downloaded
// and a template costs no more than the sources it holds.
namespace templates {
struct Style {
	QString id, name;
};
struct Kind {
	QString id, name, description;
};
QVector<Style> styles();
QVector<Kind> kinds();
// What the template looks like, with grey boxes where the game, webcam and chat go.
QImage preview(const QString &kind, const QString &style, const QSize &size);
// Adds the scene to the current collection and switches to it. Returns its name, or empty with error set.
QString apply(ObsCore *core, const QString &kind, const QString &style, QString *error);
// A downloaded pack (zip of a scene collection and its pictures): checked against sha256, unpacked into the user's
// data and imported as its own collection. Returns the collection's name, or empty with message set.
QString installPack(ObsCore *core, const QString &id, const QByteArray &zip, const QString &sha256, QString *message);
} // namespace templates

class TemplatesDialog : public QDialog {
	Q_OBJECT
public:
	TemplatesDialog(ObsCore *core, QWidget *parent);
	QString added() const { return m_added; }

private:
	void refresh();
	void loadCatalog();
	void getPack(const QJsonObject &pack);
	ObsCore *m_core;
	QListWidget *m_online = nullptr;
	QLabel *m_onlineNote = nullptr;
	QButtonGroup *m_styles;
	QListWidget *m_list;
	QString m_added;
};
