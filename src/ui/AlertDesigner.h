#pragma once
#include <QDialog>
#include <QJsonObject>

#include <obs.h>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QTableWidget;
class QTimer;

// The notification builder: how each alert looks, moves and sounds, with a live preview. Saved into the alert
// box's settings; designs can be exported and shared (.zalerts).
class AlertDesigner : public QDialog {
	Q_OBJECT
public:
	AlertDesigner(obs_source_t *source, QWidget *parent);

private:
	void load(const QString &type);
	void store();          // the form into m_design for the current type
	void preview();
	void applyTheme(const QString &theme);
	void save();
	QWidget *fileRow(QLineEdit *edit, const QString &filter);

	obs_source_t *m_source;
	QJsonObject m_design;
	QString m_type;
	bool m_loading = false;
	QListWidget *m_types;
	QCheckBox *m_on, *m_message, *m_tts;
	QLineEdit *m_title, *m_image, *m_sound;
	QSpinBox *m_volume, *m_seconds, *m_size;
	QComboBox *m_animation, *m_layout, *m_theme;
	QFontComboBox *m_font;
	QLineEdit *m_color, *m_accent;
	QDoubleSpinBox *m_min, *m_ttsMin;
	QTableWidget *m_variants;
	QLabel *m_preview;
	QTimer *m_animate;
	qint64 m_previewStart = 0;
};
