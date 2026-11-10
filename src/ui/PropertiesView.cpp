#include "ui/PropertiesView.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <cmath>

namespace {
QString text(const char *s)
{
	return QString::fromUtf8(s ? s : "");
}

QColor fromObs(long long v, bool alpha)
{
	return QColor(int(v & 0xff), int((v >> 8) & 0xff), int((v >> 16) & 0xff), alpha ? int((v >> 24) & 0xff) : 255);
}

long long toObs(const QColor &c, bool alpha)
{
	return (long long)(uint32_t(alpha ? c.alpha() : 255) << 24 | uint32_t(c.blue()) << 16 |
			   uint32_t(c.green()) << 8 | uint32_t(c.red()));
}

void paintSwatch(QPushButton *b, const QColor &c)
{
	b->setText(c.name(c.alpha() < 255 ? QColor::HexArgb : QColor::HexRgb).toUpper());
	const QColor ink = c.lightnessF() > 0.55 ? Qt::black : Qt::white;
	b->setStyleSheet(QString("QPushButton { background:%1; color:%2; }").arg(c.name(QColor::HexArgb), ink.name()));
}

QLabel *note(const QString &s)
{
	auto *l = new QLabel(s);
	l->setObjectName("hint");
	l->setWordWrap(true);
	return l;
}
} // namespace

PropertiesView::PropertiesView(obs_source_t *source, QWidget *parent) : QWidget(parent)
{
	m_layout = new QVBoxLayout(this);
	m_layout->setContentsMargins(0, 0, 0, 0);
	setSource(source);
}

PropertiesView::~PropertiesView()
{
	setSource(nullptr);
}

void PropertiesView::setSource(obs_source_t *source)
{
	if (m_props)
		obs_properties_destroy(m_props);
	m_props = nullptr;
	obs_data_release(m_settings);
	obs_data_release(m_original);
	obs_source_release(m_source);
	m_settings = m_original = nullptr;
	m_source = source ? obs_source_get_ref(source) : nullptr;
	if (m_source) {
		m_settings = obs_source_get_settings(m_source);
		m_original = obs_data_create();
		obs_data_apply(m_original, m_settings);
	}
	rebuild();
}

void PropertiesView::revert()
{
	if (m_source && m_original)
		obs_source_reset_settings(m_source, m_original);
}

void PropertiesView::scheduleRebuild()
{
	if (m_rebuildPending)
		return;
	m_rebuildPending = true;
	QTimer::singleShot(0, this, [this] {
		m_rebuildPending = false;
		rebuild();
	});
}

void PropertiesView::rebuild()
{
	int scroll = 0;
	if (m_body) {
		if (auto *area = qobject_cast<QScrollArea *>(m_body))
			scroll = area->verticalScrollBar() ? area->verticalScrollBar()->value() : 0;
		delete m_body; // the widgets go first: they refer to properties by name only
		m_body = nullptr;
	}
	if (m_props)
		obs_properties_destroy(m_props);
	m_props = nullptr;
	if (!m_source)
		return;
	m_props = obs_source_properties(m_source);
	auto *area = new QScrollArea;
	area->setWidgetResizable(true);
	area->setFrameShape(QFrame::NoFrame);
	area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	area->setObjectName("clear");
	auto *inner = new QWidget;
	inner->setObjectName("clearInner");
	auto *form = new QFormLayout(inner);
	form->setContentsMargins(4, 4, 12, 4);
	form->setVerticalSpacing(10);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	if (m_props)
		addProperties(m_props, form);
	if (form->rowCount() == 0)
		form->addRow(note("This has no options to change."));
	area->setWidget(inner);
	m_body = area;
	m_layout->addWidget(area);
	QTimer::singleShot(0, area, [area, scroll] { area->verticalScrollBar()->setValue(scroll); });
}

void PropertiesView::addProperties(obs_properties_t *props, QFormLayout *form)
{
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p))
		addProperty(p, form);
}

void PropertiesView::commit(obs_property_t *p)
{
	const bool refresh = p && obs_property_modified(p, m_settings);
	obs_source_update(m_source, m_settings);
	emit changed();
	if (refresh)
		scheduleRebuild();
}

void PropertiesView::addProperty(obs_property_t *p, QFormLayout *form)
{
	if (!obs_property_visible(p))
		return;
	const QByteArray name = obs_property_name(p);
	const QString label = text(obs_property_description(p));
	const QString tip = text(obs_property_long_description(p));
	// The property is looked up again by name when it changes: a rebuild replaces every obs_property_t.
	auto prop = [this, name] { return m_props ? obs_properties_get(m_props, name.constData()) : nullptr; };
	QWidget *field = nullptr;
	bool wide = false; // a check box carries its own label

	switch (obs_property_get_type(p)) {
	case OBS_PROPERTY_BOOL: {
		auto *box = new QCheckBox(label);
		box->setChecked(obs_data_get_bool(m_settings, name));
		connect(box, &QCheckBox::toggled, this, [this, name, prop](bool on) {
			obs_data_set_bool(m_settings, name, on);
			commit(prop());
		});
		field = box;
		wide = true;
		break;
	}
	case OBS_PROPERTY_INT: {
		const int lo = obs_property_int_min(p), hi = obs_property_int_max(p), step = obs_property_int_step(p);
		auto *spin = new QSpinBox;
		spin->setRange(lo, hi);
		spin->setSingleStep(std::max(1, step));
		spin->setSuffix(text(obs_property_int_suffix(p)));
		spin->setValue(int(obs_data_get_int(m_settings, name)));
		connect(spin, &QSpinBox::valueChanged, this, [this, name, prop](int v) {
			obs_data_set_int(m_settings, name, v);
			commit(prop());
		});
		if (obs_property_int_type(p) == OBS_NUMBER_SLIDER) {
			auto *row = new QWidget;
			auto *h = new QHBoxLayout(row);
			h->setContentsMargins(0, 0, 0, 0);
			auto *slider = new QSlider(Qt::Horizontal);
			slider->setRange(lo, hi);
			slider->setSingleStep(std::max(1, step));
			slider->setValue(spin->value());
			connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
			connect(spin, &QSpinBox::valueChanged, slider, &QSlider::setValue);
			h->addWidget(slider, 1);
			h->addWidget(spin);
			field = row;
		} else {
			field = spin;
		}
		break;
	}
	case OBS_PROPERTY_FLOAT: {
		const double lo = obs_property_float_min(p), hi = obs_property_float_max(p),
			     step = obs_property_float_step(p);
		auto *spin = new QDoubleSpinBox;
		spin->setDecimals(step > 0 && step < 0.01 ? 3 : 2);
		spin->setRange(lo, hi);
		spin->setSingleStep(step > 0 ? step : 0.1);
		spin->setSuffix(text(obs_property_float_suffix(p)));
		spin->setValue(obs_data_get_double(m_settings, name));
		connect(spin, &QDoubleSpinBox::valueChanged, this, [this, name, prop](double v) {
			obs_data_set_double(m_settings, name, v);
			commit(prop());
		});
		if (obs_property_float_type(p) == OBS_NUMBER_SLIDER && step > 0) {
			auto *row = new QWidget;
			auto *h = new QHBoxLayout(row);
			h->setContentsMargins(0, 0, 0, 0);
			auto *slider = new QSlider(Qt::Horizontal);
			const int steps = int(std::round((hi - lo) / step));
			slider->setRange(0, steps);
			slider->setValue(int(std::round((spin->value() - lo) / step)));
			connect(slider, &QSlider::valueChanged, spin, [spin, lo, step](int v) { spin->setValue(lo + v * step); });
			connect(spin, &QDoubleSpinBox::valueChanged, slider, [slider, lo, step](double v) {
				QSignalBlocker block(slider);
				slider->setValue(int(std::round((v - lo) / step)));
			});
			h->addWidget(slider, 1);
			h->addWidget(spin);
			field = row;
		} else {
			field = spin;
		}
		break;
	}
	case OBS_PROPERTY_TEXT: {
		const obs_text_type type = obs_property_text_type(p);
		if (type == OBS_TEXT_INFO) {
			auto *info = note(text(obs_data_get_string(m_settings, name)).isEmpty()
						  ? label
						  : text(obs_data_get_string(m_settings, name)));
			if (obs_property_text_info_type(p) == OBS_TEXT_INFO_WARNING ||
			    obs_property_text_info_type(p) == OBS_TEXT_INFO_ERROR)
				info->setStyleSheet("color:#FBBF24;");
			form->addRow(info);
			return;
		}
		if (type == OBS_TEXT_MULTILINE) {
			auto *edit = new QPlainTextEdit(text(obs_data_get_string(m_settings, name)));
			edit->setMinimumHeight(90);
			connect(edit, &QPlainTextEdit::textChanged, this, [this, edit, name, prop] {
				obs_data_set_string(m_settings, name, edit->toPlainText().toUtf8().constData());
				commit(prop());
			});
			field = edit;
		} else {
			auto *edit = new QLineEdit(text(obs_data_get_string(m_settings, name)));
			if (type == OBS_TEXT_PASSWORD)
				edit->setEchoMode(QLineEdit::Password);
			connect(edit, &QLineEdit::textEdited, this, [this, name, prop](const QString &v) {
				obs_data_set_string(m_settings, name, v.toUtf8().constData());
				commit(prop());
			});
			field = edit;
		}
		break;
	}
	case OBS_PROPERTY_PATH: {
		auto *row = new QWidget;
		auto *h = new QHBoxLayout(row);
		h->setContentsMargins(0, 0, 0, 0);
		auto *edit = new QLineEdit(text(obs_data_get_string(m_settings, name)));
		auto *browse = new QPushButton("Browse...");
		const obs_path_type type = obs_property_path_type(p);
		const QString filter = text(obs_property_path_filter(p)), start = text(obs_property_path_default_path(p));
		connect(edit, &QLineEdit::editingFinished, this, [this, edit, name, prop] {
			obs_data_set_string(m_settings, name, edit->text().toUtf8().constData());
			commit(prop());
		});
		connect(browse, &QPushButton::clicked, this, [this, edit, name, prop, type, filter, start, label] {
			const QString from = edit->text().isEmpty() ? start : edit->text();
			const QString chosen = type == OBS_PATH_DIRECTORY ? QFileDialog::getExistingDirectory(this, label, from)
					       : type == OBS_PATH_FILE_SAVE
						       ? QFileDialog::getSaveFileName(this, label, from, filter)
						       : QFileDialog::getOpenFileName(this, label, from, filter);
			if (chosen.isEmpty())
				return;
			edit->setText(chosen);
			obs_data_set_string(m_settings, name, chosen.toUtf8().constData());
			commit(prop());
		});
		h->addWidget(edit, 1);
		h->addWidget(browse);
		field = row;
		break;
	}
	case OBS_PROPERTY_LIST: {
		auto *combo = new QComboBox;
		// long window titles must not push the dialog wider than it is
		combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		combo->setMinimumContentsLength(16);
		const obs_combo_format format = obs_property_list_format(p);
		const bool editable = obs_property_list_type(p) == OBS_COMBO_TYPE_EDITABLE;
		combo->setEditable(editable);
		QVariant current;
		switch (format) {
		case OBS_COMBO_FORMAT_INT:
			current = obs_data_get_int(m_settings, name);
			break;
		case OBS_COMBO_FORMAT_FLOAT:
			current = obs_data_get_double(m_settings, name);
			break;
		case OBS_COMBO_FORMAT_BOOL:
			current = obs_data_get_bool(m_settings, name);
			break;
		default:
			current = text(obs_data_get_string(m_settings, name));
		}
		int selected = -1;
		for (size_t i = 0; i < obs_property_list_item_count(p); i++) {
			QVariant value;
			switch (format) {
			case OBS_COMBO_FORMAT_INT:
				value = obs_property_list_item_int(p, i);
				break;
			case OBS_COMBO_FORMAT_FLOAT:
				value = obs_property_list_item_float(p, i);
				break;
			case OBS_COMBO_FORMAT_BOOL:
				value = obs_property_list_item_bool(p, i);
				break;
			default:
				value = text(obs_property_list_item_string(p, i));
			}
			QString itemName = text(obs_property_list_item_name(p, i));
			if (itemName.startsWith(':')) // a screen without a name (often the laptop's own panel)
				itemName = "Built-in display" + itemName;
			combo->addItem(itemName, value);
			if (obs_property_list_item_disabled(p, i)) {
				if (auto *model = qobject_cast<QStandardItemModel *>(combo->model()))
					model->item(combo->count() - 1)->setEnabled(false);
			}
			if (value == current)
				selected = combo->count() - 1;
		}
		if (selected >= 0)
			combo->setCurrentIndex(selected);
		else if (editable)
			combo->setEditText(current.toString());
		else if (!current.toString().isEmpty()) { // the saved choice is gone (a closed window, an unplugged camera)
			combo->insertItem(0, current.toString() + "  (not found)", current);
			combo->setCurrentIndex(0);
		}
		auto apply = [this, name, prop, format](const QVariant &v) {
			switch (format) {
			case OBS_COMBO_FORMAT_INT:
				obs_data_set_int(m_settings, name, v.toLongLong());
				break;
			case OBS_COMBO_FORMAT_FLOAT:
				obs_data_set_double(m_settings, name, v.toDouble());
				break;
			case OBS_COMBO_FORMAT_BOOL:
				obs_data_set_bool(m_settings, name, v.toBool());
				break;
			default:
				obs_data_set_string(m_settings, name, v.toString().toUtf8().constData());
			}
			commit(prop());
		};
		connect(combo, &QComboBox::activated, this, [combo, apply](int i) { apply(combo->itemData(i)); });
		if (editable)
			connect(combo->lineEdit(), &QLineEdit::editingFinished, this,
				[combo, apply] { apply(combo->currentText()); });
		field = combo;
		break;
	}
	case OBS_PROPERTY_COLOR:
	case OBS_PROPERTY_COLOR_ALPHA: {
		const bool alpha = obs_property_get_type(p) == OBS_PROPERTY_COLOR_ALPHA;
		auto *button = new QPushButton;
		paintSwatch(button, fromObs(obs_data_get_int(m_settings, name), alpha));
		connect(button, &QPushButton::clicked, this, [this, button, name, prop, alpha, label] {
			const QColor now = fromObs(obs_data_get_int(m_settings, name), alpha);
			const QColor c = QColorDialog::getColor(now, this, label,
								alpha ? QColorDialog::ShowAlphaChannel
								      : QColorDialog::ColorDialogOptions());
			if (!c.isValid())
				return;
			paintSwatch(button, c);
			obs_data_set_int(m_settings, name, toObs(c, alpha));
			commit(prop());
		});
		field = button;
		break;
	}
	case OBS_PROPERTY_FONT: {
		auto *button = new QPushButton;
		auto describe = [](obs_data_t *font) {
			return QString("%1 %2, %3").arg(text(obs_data_get_string(font, "face")),
							 text(obs_data_get_string(font, "style")))
				.arg(obs_data_get_int(font, "size"));
		};
		obs_data_t *font = obs_data_get_obj(m_settings, name);
		button->setText(font ? describe(font) : "Choose a font");
		obs_data_release(font);
		connect(button, &QPushButton::clicked, this, [this, button, name, prop, describe] {
			obs_data_t *font = obs_data_get_obj(m_settings, name);
			QFont initial(text(obs_data_get_string(font, "face")));
			const int flags = int(obs_data_get_int(font, "flags"));
			initial.setPointSize(std::max(1, int(obs_data_get_int(font, "size"))));
			initial.setBold(flags & 1);
			initial.setItalic(flags & 2);
			initial.setUnderline(flags & 4);
			initial.setStrikeOut(flags & 8);
			obs_data_release(font);
			bool ok = false;
			const QFont f = QFontDialog::getFont(&ok, initial, this, "Font");
			if (!ok)
				return;
			obs_data_t *out = obs_data_create();
			obs_data_set_string(out, "face", f.family().toUtf8().constData());
			obs_data_set_string(out, "style", f.styleName().toUtf8().constData());
			obs_data_set_int(out, "size", f.pointSize());
			obs_data_set_int(out, "flags", (f.bold() ? 1 : 0) | (f.italic() ? 2 : 0) | (f.underline() ? 4 : 0) |
							       (f.strikeOut() ? 8 : 0));
			obs_data_set_obj(m_settings, name, out);
			button->setText(describe(out));
			obs_data_release(out);
			commit(prop());
		});
		field = button;
		break;
	}
	case OBS_PROPERTY_BUTTON: {
		auto *button = new QPushButton(label);
		connect(button, &QPushButton::clicked, this, [this, prop] {
			obs_property_t *p = prop();
			if (p && obs_property_button_clicked(p, m_source))
				scheduleRebuild();
		});
		field = button;
		wide = true;
		break;
	}
	case OBS_PROPERTY_EDITABLE_LIST: {
		auto *box = new QWidget;
		auto *v = new QVBoxLayout(box);
		v->setContentsMargins(0, 0, 0, 0);
		auto *list = new QListWidget;
		list->setMinimumHeight(110);
		obs_data_array_t *array = obs_data_get_array(m_settings, name);
		for (size_t i = 0; array && i < obs_data_array_count(array); i++) {
			obs_data_t *entry = obs_data_array_item(array, i);
			list->addItem(text(obs_data_get_string(entry, "value")));
			obs_data_release(entry);
		}
		obs_data_array_release(array);
		auto store = [this, list, name, prop] {
			obs_data_array_t *out = obs_data_array_create();
			for (int i = 0; i < list->count(); i++) {
				obs_data_t *entry = obs_data_create();
				obs_data_set_string(entry, "value", list->item(i)->text().toUtf8().constData());
				obs_data_set_bool(entry, "selected", false);
				obs_data_set_bool(entry, "hidden", false);
				obs_data_array_push_back(out, entry);
				obs_data_release(entry);
			}
			obs_data_set_array(m_settings, name, out);
			obs_data_array_release(out);
			commit(prop());
		};
		const obs_editable_list_type type = obs_property_editable_list_type(p);
		const QString filter = text(obs_property_editable_list_filter(p));
		auto *row = new QHBoxLayout;
		auto *add = new QPushButton(type == OBS_EDITABLE_LIST_TYPE_STRINGS ? "Add" : "Add files...");
		auto *remove = new QPushButton("Remove");
		connect(add, &QPushButton::clicked, this, [this, list, store, type, filter, label] {
			if (type == OBS_EDITABLE_LIST_TYPE_STRINGS) {
				bool ok = false;
				const QString s = QInputDialog::getText(this, label, label, QLineEdit::Normal, {}, &ok);
				if (ok && !s.isEmpty())
					list->addItem(s);
			} else {
				list->addItems(QFileDialog::getOpenFileNames(this, label, {}, filter));
			}
			store();
		});
		connect(remove, &QPushButton::clicked, this, [list, store] {
			delete list->currentItem();
			store();
		});
		row->addWidget(add);
		row->addWidget(remove);
		row->addStretch();
		v->addWidget(list);
		v->addLayout(row);
		field = box;
		break;
	}
	case OBS_PROPERTY_GROUP: {
		obs_properties_t *content = obs_property_group_content(p);
		auto *title = new QLabel(label);
		title->setObjectName("h2");
		if (obs_property_group_type(p) == OBS_GROUP_CHECKABLE) {
			auto *box = new QCheckBox(label);
			box->setObjectName("h2");
			const bool on = obs_data_get_bool(m_settings, name);
			box->setChecked(on);
			connect(box, &QCheckBox::toggled, this, [this, name, prop](bool v) {
				obs_data_set_bool(m_settings, name, v);
				commit(prop());
				scheduleRebuild();
			});
			form->addRow(box);
			if (!on)
				return;
		} else {
			form->addRow(title);
		}
		if (content)
			addProperties(content, form);
		return;
	}
	default:
		form->addRow(label, note("Not adjustable here yet."));
		return;
	}

	field->setEnabled(obs_property_enabled(p));
	if (!tip.isEmpty())
		field->setToolTip(tip);
	if (wide)
		form->addRow(field);
	else
		form->addRow(label, field);
}

PropertiesDialog::PropertiesDialog(obs_source_t *source, const QString &title, QWidget *parent) : QDialog(parent)
{
	setWindowTitle(title);
	resize(560, 560);
	m_view = new PropertiesView(source, this);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	buttons->button(QDialogButtonBox::Ok)->setObjectName("primary");
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, [this] {
		m_view->revert();
		reject();
	});
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(18, 18, 18, 18);
	layout->addWidget(m_view, 1);
	layout->addWidget(buttons);
}
