#include "ui/WindowModes.h"

#include "core/Settings.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QScreen>
#include <QSystemTrayIcon>
#include <QTextEdit>
#include <QTimer>
#include <QToolTip>

namespace {
const int BUTTON = 46;
const char *TIPS[] = {"Minimize", "Back to a normal window", "Close"};
} // namespace

// ---- corner buttons
CornerControls::CornerControls(WindowModes *modes, QWidget *window) : QWidget(window), m_modes(modes)
{
	setFixedSize(BUTTON * 3, 32);
	setMouseTracking(true);
	setCursor(Qt::PointingHandCursor);
	hide();
}

void CornerControls::place()
{
	move(parentWidget()->width() - width() - 8, 6);
	raise();
}

int CornerControls::indexAt(int x) const
{
	const int i = x / BUTTON;
	return i >= 0 && i < 3 ? i : -1;
}

void CornerControls::mouseMoveEvent(QMouseEvent *event)
{
	const int i = indexAt(int(event->position().x()));
	if (i != m_hot) {
		m_hot = i;
		setToolTip(i >= 0 ? TIPS[i] : "");
		update();
	}
}

void CornerControls::leaveEvent(QEvent *)
{
	m_hot = -1;
	update();
}

void CornerControls::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton)
		return;
	const int i = indexAt(int(event->position().x()));
	hide();
	if (i == 0)
		parentWidget()->showMinimized();
	else if (i == 1)
		m_modes->setMode("windowed");
	else if (i == 2)
		parentWidget()->close();
}

void CornerControls::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(12, 14, 20, 225));
	p.drawRoundedRect(rect(), 10, 10);
	for (int i = 0; i < 3; i++) {
		const QRect r(i * BUTTON, 0, BUTTON, height());
		if (i == m_hot) {
			p.setPen(Qt::NoPen);
			p.setBrush(i == 2 ? QColor(232, 17, 35) : QColor(255, 255, 255, 40));
			p.drawRoundedRect(r.adjusted(3, 3, -3, -3), 8, 8);
		}
		p.setPen(QPen(QColor(236, 239, 246), 1.4));
		p.setBrush(Qt::NoBrush);
		const QPoint c = r.center();
		if (i == 0)
			p.drawLine(c.x() - 5, c.y(), c.x() + 5, c.y());
		else if (i == 1)
			p.drawRect(c.x() - 5, c.y() - 4, 10, 8);
		else {
			p.drawLine(c.x() - 5, c.y() - 5, c.x() + 5, c.y() + 5);
			p.drawLine(c.x() - 5, c.y() + 5, c.x() + 5, c.y() - 5);
		}
	}
}

// ---- window modes
WindowModes::WindowModes(QWidget *window, const QString &name)
	: QObject(window),
	  m_window(window),
	  m_name(name),
	  m_controls(new CornerControls(this, window)),
	  m_watch(new QTimer(this))
{
	m_watch->setInterval(200);
	connect(m_watch, &QTimer::timeout, this, &WindowModes::hover);
	window->installEventFilter(this);

	m_menu = new QMenu(window);
	m_menu->addAction("Open " + name, this, &WindowModes::showAgain);
	syncTray();
}

QString WindowModes::label(const QString &mode)
{
	if (mode == "borderless")
		return "Borderless";
	if (mode == "fullscreen")
		return "Fullscreen";
	return "Windowed";
}

QString WindowModes::mode() const
{
	const QString m = Settings::get().value("window/mode", "windowed").toString();
	return modes().contains(m) ? m : "windowed";
}

void WindowModes::show()
{
	restore();
	apply(mode());
}

void WindowModes::showAgain()
{
	if (m_current.isEmpty())
		show();
	else if (m_window->isMinimized())
		m_current == "windowed" ? m_window->showNormal() : apply(m_current);
	else
		m_window->show();
	m_window->raise();
	m_window->activateWindow();
}

void WindowModes::setMode(const QString &mode)
{
	const QString m = modes().contains(mode) ? mode : "windowed";
	Settings::get().set("window/mode", m);
	apply(m);
}

void WindowModes::toggleFullscreen()
{
	// F11 is for now: the saved mode stays as it is.
	if (m_current == "fullscreen")
		apply(m_beforeFull == "fullscreen" ? "windowed" : m_beforeFull);
	else {
		m_beforeFull = m_current.isEmpty() ? "windowed" : m_current;
		apply("fullscreen");
	}
}

void WindowModes::apply(const QString &mode)
{
	if (m_current == "windowed" && mode != "windowed")
		remember();
	const bool frameless = mode != "windowed";
	if (m_window->windowFlags().testFlag(Qt::FramelessWindowHint) != frameless)
		m_window->setWindowFlag(Qt::FramelessWindowHint, frameless); // hides the window; shown below
	if (mode == "fullscreen") {
		m_window->showFullScreen();
	} else if (mode == "borderless") {
		if (m_window->isFullScreen() || m_window->isMaximized())
			m_window->showNormal();
		QScreen *screen = m_window->screen() ? m_window->screen() : QApplication::primaryScreen();
		m_window->setGeometry(screen->availableGeometry()); // fills the screen above the taskbar
		m_window->show();
	} else {
		if (m_current == "borderless" || m_current == "fullscreen") {
			m_window->showNormal();
			restore();
		}
		m_window->show();
	}
	if (mode == "fullscreen" && !m_current.isEmpty() && m_current != "fullscreen")
		m_beforeFull = m_current;
	m_current = mode;
	if (frameless)
		m_watch->start();
	else {
		m_watch->stop();
		m_controls->hide();
	}
	emit modeChanged(mode);
}

void WindowModes::remember()
{
	if (m_current == "windowed" && m_window->isVisible() && !m_window->isMinimized())
		Settings::get().set("window/geometry", m_window->saveGeometry());
}

void WindowModes::restore()
{
	const QByteArray geometry = Settings::get().value("window/geometry").toByteArray();
	if (!geometry.isEmpty())
		m_window->restoreGeometry(geometry);
}

void WindowModes::hover()
{
	if (!m_window->isVisible() || m_window->isMinimized() || !m_window->isActiveWindow()) {
		m_controls->hide();
		return;
	}
	const QPoint pos = m_window->mapFromGlobal(QCursor::pos());
	const bool nearTop = pos.x() >= 0 && pos.x() <= m_window->width() && pos.y() >= 0 && pos.y() <= 4;
	const bool over = m_controls->isVisible() && m_controls->geometry().adjusted(-24, -8, 8, 24).contains(pos);
	if (nearTop || over) {
		m_away = 0;
		if (!m_controls->isVisible()) {
			m_controls->place();
			m_controls->show();
		}
	} else if (m_controls->isVisible() && ++m_away >= 4) {
		m_controls->hide();
	}
}

bool WindowModes::eventFilter(QObject *obj, QEvent *event)
{
	if (obj == m_window) {
		if (event->type() == QEvent::Resize && m_controls->isVisible())
			m_controls->place();
		else if (event->type() == QEvent::Show)
			emit visibilityChanged(true);
		else if (event->type() == QEvent::Hide)
			emit visibilityChanged(false);
		else if (event->type() == QEvent::WindowStateChange)
			emit visibilityChanged(!m_window->isMinimized() && m_window->isVisible());
	}
	return false;
}

bool WindowModes::handleKey(QKeyEvent *event)
{
	if (event->key() == Qt::Key_F11 && event->modifiers() == Qt::NoModifier) {
		toggleFullscreen();
		return true;
	}
	if (event->key() != Qt::Key_Escape || event->modifiers() != Qt::NoModifier)
		return false;
	if (QApplication::activePopupWidget() || QApplication::activeModalWidget())
		return false;
	QWidget *focus = QApplication::focusWidget();
	if (focus && m_window->isAncestorOf(focus) &&
	    (qobject_cast<QLineEdit *>(focus) || qobject_cast<QTextEdit *>(focus) ||
	     qobject_cast<QPlainTextEdit *>(focus) || qobject_cast<QAbstractSpinBox *>(focus) ||
	     qobject_cast<QKeySequenceEdit *>(focus))) {
		focus->clearFocus(); // Esc in a text box only leaves the box
		return true;
	}
	if (m_current == "fullscreen") {
		toggleFullscreen();
		return true;
	}
	return Settings::get().value("tray/esc", true).toBool() && hideToTray();
}

void WindowModes::syncTray()
{
	Settings &s = Settings::get();
	const bool wanted = (s.value("tray/close", true).toBool() || s.value("tray/esc", true).toBool()) &&
			    QSystemTrayIcon::isSystemTrayAvailable();
	if (wanted && !m_tray) {
		m_tray = new QSystemTrayIcon(m_window->windowIcon().isNull() ? QApplication::windowIcon()
									     : m_window->windowIcon(),
					     this);
		m_tray->setToolTip(m_name);
		m_tray->setContextMenu(m_menu);
		connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason why) {
			if (why == QSystemTrayIcon::Trigger)
				showAgain();
		});
	}
	if (m_tray)
		m_tray->setVisible(wanted);
}

bool WindowModes::hideToTray()
{
	if (!m_tray || !m_tray->isVisible())
		return false;
	remember();
	m_controls->hide();
	m_window->hide();
	Settings &s = Settings::get();
	if (!s.value("tray/hintShown", false).toBool()) {
		m_tray->showMessage(m_name,
				    m_name + " is still running here in the tray. Click this icon to open it, or change "
					     "this in Settings > Window.",
				    QSystemTrayIcon::Information, 6000);
		s.set("tray/hintShown", true);
	}
	return true;
}

bool WindowModes::confirm()
{
	const QString why = busy ? busy() : QString();
	if (why.isEmpty())
		return true;
	showAgain();
	return QMessageBox::question(m_window, m_name, why + "\n\nQuit " + m_name + " anyway?") == QMessageBox::Yes;
}

bool WindowModes::closeEvent(QCloseEvent *event)
{
	if (!m_quitting && Settings::get().value("tray/close", true).toBool() && hideToTray()) {
		event->ignore();
		return true;
	}
	if (!m_quitting && !confirm()) {
		event->ignore();
		return true;
	}
	remember();
	m_quitting = true;
	if (m_tray)
		m_tray->hide();
	return false;
}

void WindowModes::quit()
{
	if (!confirm())
		return;
	m_quitting = true;
	m_window->close();
}
