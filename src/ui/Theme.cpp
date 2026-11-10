#include "ui/Theme.h"

QString Theme::styleSheet()
{
	QString qss = R"(
* { font-family: "Segoe UI Variable Text", "Segoe UI"; font-size: 10pt; color: @text; }
QMainWindow, QDialog { background: @bg; }
QWidget#panel { background: @panel; border: 1px solid @line; border-radius: 14px; }
QWidget#topbar { background: transparent; }
QLabel#brand { font-size: 13pt; font-weight: 700; letter-spacing: 2px; }
QLabel#h2 { font-size: 11pt; font-weight: 600; }
QLabel#mute, QLabel#hint { color: @mute; }
QLabel#chip { background: @raised; border: 1px solid @line; border-radius: 11px; padding: 3px 10px; color: @text; }
QLabel#preview_note { color: @mute; font-size: 11pt; }

QPushButton { background: @raised; border: 1px solid @line; border-radius: 10px; padding: 8px 14px; }
QPushButton:hover { border-color: @accent; }
QPushButton:pressed { background: @line; }
QPushButton:disabled { color: @mute; }
QPushButton[on="true"] { color: @accent; border: 2px solid @accent; }
QPushButton#primary { background: @accent; color: #031016; border: none; font-weight: 700; }
QPushButton#primary:hover { background: #5EE3F4; }
QPushButton#live { background: @live; color: white; border: none; font-weight: 700; font-size: 11pt; padding: 12px 22px; }
QPushButton#live:hover { background: #FF5C78; }
QPushButton#live[on="true"] { background: @raised; color: @live; border: 2px solid @live; }
QPushButton#rec { font-weight: 700; font-size: 11pt; padding: 12px 22px; }
QPushButton#rec[on="true"] { color: @rec; border: 2px solid @rec; }
QPushButton#icon { padding: 5px 8px; min-width: 16px; }

QListWidget { background: transparent; border: none; outline: none; }
QScrollArea#clear, QScrollArea#clear > QWidget#qt_scrollarea_viewport, QWidget#clearInner { background: transparent; border: none; }
QListWidget::item { padding: 8px 6px; border-radius: 8px; margin: 1px 0; }
QListWidget::item:selected { background: @raised; color: @text; }
QListWidget::item:hover { background: #161A23; }

QLineEdit, QComboBox, QSpinBox { background: @raised; border: 1px solid @line; border-radius: 8px; padding: 6px 8px; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border-color: @accent; }
QComboBox QAbstractItemView { background: @raised; border: 1px solid @line; selection-background-color: @line; }
QCheckBox { spacing: 8px; }
QCheckBox::indicator { width: 34px; height: 18px; border-radius: 9px; background: @line; }
QCheckBox::indicator:checked { background: @accent; }

QSlider::groove:horizontal { height: 4px; background: @line; border-radius: 2px; }
QSlider::sub-page:horizontal { background: @accent; border-radius: 2px; }
QSlider::handle:horizontal { background: @text; width: 14px; height: 14px; margin: -5px 0; border-radius: 7px; }

QTabWidget::pane { border: 1px solid @line; border-radius: 12px; top: -1px; background: @panel; }
QTabBar::tab { background: transparent; padding: 8px 16px; color: @mute; border: none; }
QTabBar::tab:selected { color: @text; border-bottom: 2px solid @accent; }
QMenu { background: @raised; border: 1px solid @line; border-radius: 10px; padding: 6px; }
QMenu::item { padding: 7px 16px; border-radius: 6px; }
QMenu::item:selected { background: @line; }
QToolTip { background: @raised; color: @text; border: 1px solid @line; padding: 4px 8px; }
QScrollBar:vertical { background: transparent; width: 8px; }
QScrollBar::handle:vertical { background: @line; border-radius: 4px; min-height: 30px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
)";
	const std::pair<const char *, QColor> tokens[] = {{"@bg", Bg},     {"@panel", Panel}, {"@raised", Raised},
							   {"@line", Line}, {"@text", Text},   {"@mute", Mute},
							   {"@accent", Accent}, {"@live", Live}, {"@rec", Rec}};
	for (const auto &[token, color] : tokens)
		qss.replace(token, color.name());
	return qss;
}
