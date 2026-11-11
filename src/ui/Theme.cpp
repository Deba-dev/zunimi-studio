#include "ui/Theme.h"

#include "core/I18n.h"
#include "core/Settings.h"

#include <QApplication>
#include <QDir>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>

namespace {
struct AccentColors {
	const char *id, *a1, *a2, *on;
};
// the same pairs as zunimilabs.online (style.css, data-accent)
const AccentColors ACCENTS[] = {{"neon", "#9333EA", "#3B82F6", "#FFFFFF"}, // Zunimi Stream+'s own: purple into blue
				{"violet", "#7C5CFF", "#C084FC", "#FFFFFF"},
				{"sunset", "#FF5A3D", "#FF9F1C", "#FFFFFF"},
				{"ocean", "#2563EB", "#06B6D4", "#FFFFFF"},
				{"mint", "#10B981", "#34D399", "#04120C"}};

QString rgba(const QColor &c, double alpha)
{
	return QString("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(int(alpha * 255));
}
// The show/hide eye for the Sources list, drawn here because style sheets need an image file and this Qt has no
// SVG reader. Written once per colour into the app's data folder (it is replaced, never piles up).
// kind: "eye-on", "eye-off", "close" (a dock's X), "float" (a dock's pop-out)
QString iconFile(const QString &kind, const QColor &color)
{
	const QString dir = Settings::dataDir() + "/ui";
	QDir().mkpath(dir);
	const QString file = QString("%1/%2.png").arg(dir, kind);
	QPixmap pix(40, 40);
	pix.fill(Qt::transparent);
	QPainter p(&pix);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(QPen(color, 2.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	p.setBrush(Qt::NoBrush);
	if (kind.startsWith("eye")) {
		QPainterPath eye;
		eye.moveTo(5, 20);
		eye.quadTo(20, 4, 35, 20);
		eye.quadTo(20, 36, 5, 20);
		p.drawPath(eye);
		p.drawEllipse(QPointF(20, 20), 5, 5);
		if (kind == "eye-off")
			p.drawLine(QPointF(8, 33), QPointF(32, 7));
	} else if (kind == "close") {
		p.drawLine(QPointF(12, 12), QPointF(28, 28));
		p.drawLine(QPointF(28, 12), QPointF(12, 28));
	} else { // float: a window with an arrow out of its corner
		p.drawRoundedRect(QRectF(8, 13, 19, 19), 3, 3);
		p.drawLine(QPointF(19, 21), QPointF(32, 8));
		p.drawLine(QPointF(24, 8), QPointF(32, 8));
		p.drawLine(QPointF(32, 8), QPointF(32, 16));
	}
	p.end();
	pix.save(file, "PNG");
	return QDir::fromNativeSeparators(file);
}
} // namespace

QStringList Theme::accents()
{
	QStringList out;
	for (const AccentColors &a : ACCENTS)
		out << a.id;
	return out;
}

QString Theme::accentName(const QString &accent)
{
	if (accent == "sunset")
		return L("Sunset");
	if (accent == "ocean")
		return L("Ocean");
	if (accent == "mint")
		return L("Mint");
	if (accent == "violet")
		return L("Violet");
	return L("Neon");
}

void Theme::apply()
{
	const QString wanted = Settings::get().value("ui/accent", "neon").toString();
	const AccentColors *chosen = &ACCENTS[0];
	for (const AccentColors &a : ACCENTS)
		if (wanted == a.id)
			chosen = &a;
	Accent = QColor(chosen->a1);
	Accent2 = QColor(chosen->a2);
	OnAccent = QColor(chosen->on);
	if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance())) {
		QPalette p = app->palette();
		p.setColor(QPalette::Link, Accent2.lighter(115)); // links in rich text follow the accent
		p.setColor(QPalette::LinkVisited, Accent2.lighter(115));
		p.setColor(QPalette::Highlight, Accent);
		p.setColor(QPalette::HighlightedText, OnAccent);
		app->setPalette(p);
		app->setStyleSheet(styleSheet());
	}
}

QString Theme::styleSheet()
{
	QString qss = R"(
* { font-family: "Segoe UI Variable Text", "Segoe UI", "Nirmala UI"; font-size: 10pt; color: @text; }
QMainWindow, QDialog { background: @bg; }
QWidget#panel { background: @panel; border: 1px solid @line; border-radius: 16px; }
QWidget#topbar { background: transparent; }
QLabel#brand { font-size: 12.5pt; font-weight: 700; }
QLabel#pill { font-family: "Cascadia Mono", Consolas; font-size: 8pt; color: @acc2; border: 1px solid @accent;
	border-radius: 9px; padding: 1px 8px; background: @accsoft; }
QMenuBar { background: @panel; border-bottom: 1px solid @line; padding: 2px 4px; }
QMenuBar::item { background: transparent; padding: 7px 11px; border-radius: 8px; color: @mute; }
QMenuBar::item:selected, QMenuBar::item:pressed { background: @accsoft; color: @text; }

QMainWindow::separator { background: transparent; width: 8px; height: 8px; }
QMainWindow::separator:hover { background: @accsoft; }
QDockWidget { color: @text; font-weight: 600; titlebar-close-icon: url(@iconclose); titlebar-normal-icon: url(@iconfloat); }
QDockWidget::title { background: @panel; border: 1px solid @line; border-bottom: none; border-top-left-radius: 14px;
	border-top-right-radius: 14px; padding: 9px 12px 4px 14px; text-align: left; }
QDockWidget::close-button, QDockWidget::float-button { background: transparent; border: none; border-radius: 6px;
	width: 16px; height: 16px; }
QDockWidget::close-button:hover, QDockWidget::float-button:hover { background: @raised; }
QWidget#dockBody { background: @panel; border: 1px solid @line; border-top: none; border-bottom-left-radius: 14px;
	border-bottom-right-radius: 14px; }
QWidget#hero { border: 1px solid @accent; border-radius: 14px;
	background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 @accmid, stop:1 @acc2mid); }
QPushButton#hero { text-align: left; padding: 0; }
QPushButton#hero:hover { border: 1px solid @acc2; }
QPushButton#hero[on="true"] { border: 1px solid @live; background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 rgba(220, 38, 38, 90), stop:1 @acc2mid); }
QPushButton#hero:disabled { border: 1px solid @line; }
QLabel#slogan { font-size: 16pt; font-weight: 800; color: white; }
QLabel#heroNote { color: #E2DCFB; }
QWidget#tile { background: @bg; border: 1px solid @line; border-radius: 11px; }
QLabel#tileLabel { color: @mute; font-size: 8pt; letter-spacing: 1px; }
QLabel#tileValue { font-family: "Cascadia Mono", Consolas; font-size: 12.5pt; }
QLabel#tileValue[on="true"] { color: #FCA5A5; }
QListWidget::indicator { width: 18px; height: 18px; }
QListWidget::indicator:checked { image: url(@eyeon); }
QListWidget::indicator:unchecked { image: url(@eyeoff); }
QLabel#h2 { font-size: 11pt; font-weight: 600; }
QLabel#h1 { font-size: 16pt; font-weight: 700; }
QPushButton#choice { text-align: left; padding: 14px 16px; border-radius: 12px; }
QPushButton#choice:checked { border: 2px solid @accent; background: @accsoft; }
QLabel#mute, QLabel#hint { color: @mute; }
QLabel#chip { background: @raised; border: 1px solid @line; border-radius: 11px; padding: 3px 10px; color: @text; }
QLabel#preview_note { color: @mute; font-size: 11pt; }

QPushButton { background: @raised; border: 1px solid @line; border-radius: 10px; padding: 8px 14px; }
QPushButton:hover { border-color: @acc2; }
QPushButton:pressed { background: @line; }
QPushButton:disabled { color: @mute; }
QPushButton[on="true"] { color: @acc2; border: 2px solid @accent; }
QPushButton#primary { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 @accent, stop:1 @acc2); color: @onacc;
	border: none; font-weight: 700; }
QPushButton#primary:hover { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 @acchi, stop:1 @acc2hi); }
QPushButton#primary:disabled { background: @line; color: @mute; }
QPushButton#live { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 @accent, stop:1 @acc2); color: @onacc;
	border: none; font-weight: 700; font-size: 11pt; padding: 12px 22px; }
QPushButton#live:hover { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 @acchi, stop:1 @acc2hi); }
QPushButton#live[on="true"] { background: @raised; color: @live; border: 2px solid @live; }
QPushButton#rec { font-weight: 700; font-size: 11pt; padding: 12px 22px; }
QPushButton#rec[on="true"] { color: @rec; border: 2px solid @rec; }
QPushButton#icon { padding: 5px 8px; min-width: 16px; }

QListWidget { background: transparent; border: none; outline: none; }
QScrollArea#clear, QScrollArea#clear > QWidget#qt_scrollarea_viewport, QWidget#clearInner { background: transparent; border: none; }
QListWidget::item { padding: 8px 6px; border-radius: 8px; margin: 1px 0; }
QListWidget::item:selected { background: @accsoft; color: @text; }
QListWidget::item:hover { background: @raised; }

QLineEdit, QComboBox, QSpinBox { background: @raised; border: 1px solid @line; border-radius: 8px; padding: 6px 8px; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border-color: @accent; }
QComboBox QAbstractItemView { background: @raised; border: 1px solid @line; selection-background-color: @accsoft; }
QCheckBox { spacing: 8px; }
QCheckBox::indicator { width: 34px; height: 18px; border-radius: 9px; background: @line; }
QCheckBox::indicator:checked { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 @accent, stop:1 @acc2); }

QSlider::groove:horizontal { height: 4px; background: @line; border-radius: 2px; }
QSlider::sub-page:horizontal { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 @accent, stop:1 @acc2); border-radius: 2px; }
QSlider::handle:horizontal { background: @text; width: 14px; height: 14px; margin: -5px 0; border-radius: 7px; }

QTabWidget::pane { border: 1px solid @line; border-radius: 14px; top: -1px; background: @panel; }
QTabBar::tab { background: transparent; padding: 8px 16px; color: @mute; border: none; }
QTabBar::tab:selected { color: @text; border-bottom: 2px solid @accent; }
QMenu { background: @raised; border: 1px solid @line; border-radius: 10px; padding: 6px; }
QMenu::item { padding: 7px 16px; border-radius: 6px; }
QMenu::item:selected { background: @accsoft; }
QToolTip { background: @raised; color: @text; border: 1px solid @line; padding: 4px 8px; }
QScrollBar:vertical { background: transparent; width: 8px; }
QScrollBar::handle:vertical { background: @line; border-radius: 4px; min-height: 30px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
)";
	// longer tokens first, so @acc2hi is not eaten by @acc2
	const std::pair<const char *, QString> tokens[] = {
		{"@acc2hi", Accent2.lighter(112).name()}, {"@acchi", Accent.lighter(112).name()},
		{"@accsoft", rgba(Accent, 0.16)},        {"@accmid", rgba(Accent, 0.42)},
		{"@acc2mid", rgba(Accent2, 0.32)},       {"@acc2", Accent2.name()},
		{"@eyeon", iconFile("eye-on", Accent2.lighter(130))}, {"@eyeoff", iconFile("eye-off", Mute.darker(130))},
		{"@iconclose", iconFile("close", Mute)}, {"@iconfloat", iconFile("float", Mute)},
		{"@onacc", OnAccent.name()},             {"@accent", Accent.name()},
		{"@bg", Bg.name()},                      {"@panel", Panel.name()},
		{"@raised", Raised.name()},              {"@line", Line.name()},
		{"@text", Text.name()},                  {"@mute", Mute.name()},
		{"@live", Live.name()},                  {"@rec", Rec.name()}};
	for (const auto &[token, value] : tokens)
		qss.replace(token, value);
	return qss;
}
