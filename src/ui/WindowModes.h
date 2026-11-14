#pragma once
#include <QObject>
#include <QPointer>
#include <QWidget>
#include <functional>

class QKeyEvent;
class QCloseEvent;
class QSystemTrayIcon;
class QMenu;
class QTimer;
class WindowModes;

// Minimize, back to a normal window, and close, for windows without a title bar.
class CornerControls : public QWidget {
	Q_OBJECT
public:
	explicit CornerControls(WindowModes *modes, QWidget *window);
	void place();

protected:
	void paintEvent(QPaintEvent *) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void leaveEvent(QEvent *) override;

private:
	int indexAt(int x) const;
	WindowModes *m_modes;
	int m_hot = -1;
};

// The ZunimiLabs window rules, the same as in Clip Studio and ChatDoom (ui/window_modes.py there):
// Windowed / Borderless / Fullscreen, F11, Esc and close to the tray (each switchable), a one-time tray
// hint, a remembered size, and a confirmation before quitting while live or recording.
class WindowModes : public QObject {
	Q_OBJECT
public:
	WindowModes(QWidget *window, const QString &name);

	static QStringList modes() { return {"windowed", "borderless", "fullscreen"}; }
	static QString label(const QString &mode);
	QString mode() const;          // the saved mode
	QString current() const { return m_current; }

	void show();                   // first show: saved size, saved mode
	void showAgain();              // from the tray
	void setMode(const QString &mode);
	void toggleFullscreen();
	bool handleKey(QKeyEvent *event);
	bool closeEvent(QCloseEvent *event); // true: the window only hid, or the person kept it open
	bool hideToTray();
	void quit(bool ask = true);
	void notify(const QString &text); // a Windows notification from the tray icon (when there is one) // ask: confirm first (self-tests and "Restart and update" skip it)
	void syncTray();
	void remember();
	void dragOut(); // the move grip in borderless and fullscreen

	QMenu *trayMenu() const { return m_menu; }
	std::function<QString()> busy;       // a sentence when quitting would stop something

signals:
	void modeChanged(const QString &mode);
	void visibilityChanged(bool shown);

protected:
	bool eventFilter(QObject *obj, QEvent *event) override;

private:
	void apply(const QString &mode);
	void restore();
	void hover();
	bool confirm();

	QWidget *m_window;
	QString m_name;
	QString m_current;
	QString m_beforeFull = "windowed";
	bool m_quitting = false;
	QSystemTrayIcon *m_tray = nullptr;
	QMenu *m_menu = nullptr;
	CornerControls *m_controls;
	QTimer *m_watch;
	int m_away = 0;
};
