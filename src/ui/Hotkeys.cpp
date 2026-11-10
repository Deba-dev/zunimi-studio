#include "ui/Hotkeys.h"
#include "core/I18n.h"

#include "core/Settings.h"

#include <QCoreApplication>
#include <QTimer>

#include <windows.h>

namespace {
const int BASE_ID = 0x5A00; // our ids, away from anything Qt uses

UINT virtualKey(int key, bool keypad)
{
	if (key >= Qt::Key_A && key <= Qt::Key_Z)
		return UINT('A' + (key - Qt::Key_A));
	if (key >= Qt::Key_0 && key <= Qt::Key_9)
		return keypad ? UINT(VK_NUMPAD0 + (key - Qt::Key_0)) : UINT('0' + (key - Qt::Key_0));
	if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
		return UINT(VK_F1 + (key - Qt::Key_F1));
	switch (key) {
	case Qt::Key_Home: return VK_HOME;
	case Qt::Key_End: return VK_END;
	case Qt::Key_PageUp: return VK_PRIOR;
	case Qt::Key_PageDown: return VK_NEXT;
	case Qt::Key_Insert: return VK_INSERT;
	case Qt::Key_Delete: return VK_DELETE;
	case Qt::Key_Pause: return VK_PAUSE;
	case Qt::Key_Print: return VK_SNAPSHOT;
	case Qt::Key_ScrollLock: return VK_SCROLL;
	case Qt::Key_Space: return VK_SPACE;
	case Qt::Key_Left: return VK_LEFT;
	case Qt::Key_Right: return VK_RIGHT;
	case Qt::Key_Up: return VK_UP;
	case Qt::Key_Down: return VK_DOWN;
	case Qt::Key_Minus: return keypad ? VK_SUBTRACT : VK_OEM_MINUS;
	case Qt::Key_Plus: return keypad ? VK_ADD : VK_OEM_PLUS;
	case Qt::Key_Equal: return VK_OEM_PLUS;
	case Qt::Key_Asterisk: return VK_MULTIPLY;
	case Qt::Key_Slash: return keypad ? VK_DIVIDE : VK_OEM_2;
	case Qt::Key_Comma: return VK_OEM_COMMA;
	case Qt::Key_Period: return keypad ? VK_DECIMAL : VK_OEM_PERIOD;
	case Qt::Key_Semicolon: return VK_OEM_1;
	case Qt::Key_Apostrophe: return VK_OEM_7;
	case Qt::Key_BracketLeft: return VK_OEM_4;
	case Qt::Key_BracketRight: return VK_OEM_6;
	case Qt::Key_Backslash: return VK_OEM_5;
	case Qt::Key_QuoteLeft: return VK_OEM_3;
	default: return 0;
	}
}
} // namespace

QVector<Hotkeys::Action> Hotkeys::actions()
{
	QVector<Action> list = {
		{"live", L("Go Live / End stream")},
		{"record", L("Start / stop recording")},
		{"replay_save", L("Save replay")},
		{"replay_toggle", L("Replay buffer on / off")},
		{"mic_mute", L("Mute / unmute mic")},
		{"desktop_mute", L("Mute / unmute desktop audio")},
		{"ptt", L("Push to talk (mic on while held)")},
		{"ptm", L("Push to mute (mic off while held)")},
		{"record_pause", L("Pause / resume recording")},
		{"screenshot", L("Screenshot")},
		{"studio_go", L("Studio mode: send preview live")},
		{"vcam", L("Virtual camera on / off")},
		{"undo", L("Undo last change")},
		{"mark", L("Mark moment (chapter + saved replay)")},
	};
	for (int i = 1; i <= 9; i++)
		list.append({QString("scene_%1").arg(i), QString(L("Switch to scene %1")).arg(i)});
	return list;
}

bool Hotkeys::supported(const QKeySequence &keys)
{
	if (keys.isEmpty())
		return false;
	const QKeyCombination combo = keys[0];
	return virtualKey(int(combo.key()), combo.keyboardModifiers() & Qt::KeypadModifier) != 0;
}

Hotkeys::Hotkeys(QObject *parent) : QObject(parent)
{
	QCoreApplication::instance()->installNativeEventFilter(this);
	m_hold = new QTimer(this);
	m_hold->setInterval(30);
	connect(m_hold, &QTimer::timeout, this, [this] {
		if (GetAsyncKeyState(int(m_heldKey)) & 0x8000)
			return;
		m_hold->stop();
		emit triggered(m_held + ":up");
		m_held.clear();
	});
}

Hotkeys::~Hotkeys()
{
	clear();
	QCoreApplication::instance()->removeNativeEventFilter(this);
}

void Hotkeys::clear()
{
	for (int i = 0; i < m_registered.size(); i++)
		if (!m_registered[i].isEmpty())
			UnregisterHotKey(nullptr, BASE_ID + i);
	m_registered.clear();
	m_keys.clear();
}

QStringList Hotkeys::apply()
{
	clear();
	QStringList failed;
	const QVector<Action> list = actions();
	m_registered.resize(list.size());
	m_keys.resize(list.size());
	for (int i = 0; i < list.size(); i++) {
		const QKeySequence keys(Settings::get().value("hotkeys/" + list[i].id).toString());
		if (!supported(keys))
			continue;
		const QKeyCombination combo = keys[0];
		const Qt::KeyboardModifiers mods = combo.keyboardModifiers();
		UINT flags = MOD_NOREPEAT;
		if (mods & Qt::ControlModifier)
			flags |= MOD_CONTROL;
		if (mods & Qt::AltModifier)
			flags |= MOD_ALT;
		if (mods & Qt::ShiftModifier)
			flags |= MOD_SHIFT;
		if (mods & Qt::MetaModifier)
			flags |= MOD_WIN;
		const UINT vk = virtualKey(int(combo.key()), mods & Qt::KeypadModifier);
		const bool hold = list[i].id == "ptt" || list[i].id == "ptm";
		if (RegisterHotKey(nullptr, BASE_ID + i, hold ? flags & ~UINT(MOD_NOREPEAT) : flags, vk)) {
			m_registered[i] = list[i].id;
			m_keys[i] = vk;
		}
		else
			failed << QString("%1 (%2)").arg(list[i].label, keys.toString(QKeySequence::NativeText));
	}
	return failed;
}

bool Hotkeys::nativeEventFilter(const QByteArray &, void *message, qintptr *)
{
	const MSG *msg = static_cast<const MSG *>(message);
	if (!msg || msg->message != WM_HOTKEY || msg->hwnd)
		return false;
	const int index = int(msg->wParam) - BASE_ID;
	if (index < 0 || index >= m_registered.size() || m_registered[index].isEmpty())
		return false;
	const QString id = m_registered[index];
	if (id == "ptt" || id == "ptm") {
		if (m_held == id)
			return true; // key repeat while held
		m_held = id;
		m_heldKey = m_keys[index];
		emit triggered(id + ":down");
		m_hold->start();
		return true;
	}
	emit triggered(id);
	return true;
}
