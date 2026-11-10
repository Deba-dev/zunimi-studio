#pragma once
#include <QDialog>

#include <obs.h>

class PropertiesView;
class QListWidget;

// The filters on one source (or the mic / desktop audio): add, switch on and off, reorder, remove,
// and every option of the selected filter on the right. Changes apply live.
class FiltersDialog : public QDialog {
	Q_OBJECT
public:
	FiltersDialog(obs_source_t *source, QWidget *parent);
	~FiltersDialog() override;

private:
	void refresh(const QString &select = {});
	void add(const QString &id);
	obs_source_t *selected() const; // a new reference, or null

	obs_source_t *m_source;
	QListWidget *m_list;
	PropertiesView *m_view;
};
