#pragma once
#include <QDialog>
#include <QWidget>

#include <obs.h>

class QFormLayout;
class QVBoxLayout;

// Every option a source or filter has, built from what libobs says it has: check boxes, numbers, sliders,
// lists, text, files, colours, fonts and buttons. Changes apply live, so the preview shows them at once.
class PropertiesView : public QWidget {
	Q_OBJECT
public:
	explicit PropertiesView(obs_source_t *source, QWidget *parent = nullptr);
	~PropertiesView() override;

	void setSource(obs_source_t *source);
	void revert(); // back to how it was when the view opened
	obs_source_t *source() const { return m_source; }

signals:
	void changed();

private:
	void rebuild();
	void addProperties(obs_properties_t *props, QFormLayout *form);
	void addProperty(obs_property_t *p, QFormLayout *form);
	void commit(obs_property_t *p);
	void scheduleRebuild();

	obs_source_t *m_source = nullptr;
	obs_data_t *m_settings = nullptr; // the source's own settings object
	obs_data_t *m_original = nullptr;
	obs_properties_t *m_props = nullptr;
	QWidget *m_body = nullptr;
	QVBoxLayout *m_layout = nullptr;
	bool m_rebuildPending = false;
};

// The view in a dialog with OK and Cancel. Cancel undoes every change made while it was open.
class PropertiesDialog : public QDialog {
	Q_OBJECT
public:
	PropertiesDialog(obs_source_t *source, const QString &title, QWidget *parent);

private:
	PropertiesView *m_view;
};
