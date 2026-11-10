#pragma once
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTextBrowser;

// Every platform's chat in one place (Twitch, YouTube, Kick), with sending to Twitch when logged in.
class ChatPanel : public QWidget {
	Q_OBJECT
public:
	explicit ChatPanel(QWidget *parent = nullptr);

signals:
	void setupRequested();

private:
	void send();
	QTextBrowser *m_view;
	QLineEdit *m_input;
	QLabel *m_status;
	int m_lines = 0;
};

// What happened on stream (follows, subs, tips...), with replay, tests and the connection status.
class EventsPanel : public QWidget {
	Q_OBJECT
public:
	explicit EventsPanel(QWidget *parent = nullptr);

signals:
	void setupRequested();

private:
	void refresh();
	QListWidget *m_list;
	QLabel *m_status;
};
