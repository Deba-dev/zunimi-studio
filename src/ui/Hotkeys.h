#pragma once
#include <QAbstractNativeEventFilter>
#include <QKeySequence>
#include <QObject>
#include <QVector>

// System-wide hotkeys (they work while a game has focus, even fullscreen). Stored in settings as
// "hotkeys/<action>" = a key sequence like "Ctrl+F9". Registered on the app's thread, so they survive the
// main window being re-created when the window mode changes.
class Hotkeys : public QObject, public QAbstractNativeEventFilter {
	Q_OBJECT
public:
	struct Action {
		QString id;
		QString label;
	};
	static QVector<Action> actions();
	static bool supported(const QKeySequence &keys); // a key Windows can register

	explicit Hotkeys(QObject *parent = nullptr);
	~Hotkeys() override;

	// (Re)registers everything in settings. Returns the labels of hotkeys another program already uses.
	QStringList apply();

	bool nativeEventFilter(const QByteArray &type, void *message, qintptr *result) override;

signals:
	void triggered(const QString &action);

private:
	void clear();
	QVector<QString> m_registered; // index + 1 = the Windows hotkey id
};
