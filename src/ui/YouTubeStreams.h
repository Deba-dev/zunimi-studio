#pragma once
#include <QDialog>

class QLabel;
class QListWidget;
class QPushButton;

// Sign in to YouTube and pick one of your scheduled or live streams: its server and stream key become the main
// stream's, so Start Streaming goes to that broadcast.
class YouTubeStreamsDialog : public QDialog {
	Q_OBJECT
public:
	explicit YouTubeStreamsDialog(QWidget *parent);
	QString picked() const { return m_picked; } // the chosen broadcast's title, after Accepted

private:
	void refresh();
	void use();

	QLabel *m_who;
	QLabel *m_note;
	QPushButton *m_sign;
	QPushButton *m_reload;
	QPushButton *m_use;
	QListWidget *m_list;
	QString m_picked;
};
