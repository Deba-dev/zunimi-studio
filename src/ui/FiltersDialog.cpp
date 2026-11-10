#include "ui/FiltersDialog.h"

#include "ui/PropertiesView.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>

namespace {
// The filters people reach for, best first. Ones a module did not load are left out by themselves.
const char *VIDEO_FILTERS[] = {"chroma_key_filter", "color_filter",     "crop_filter",     "sharpness_filter",
			       "color_key_filter",  "luma_key_filter",  "mask_filter",     "clut_filter",
			       "scroll_filter",     "scale_filter",     "gpu_delay",       "hdr_tonemap_filter"};
const char *AUDIO_FILTERS[] = {"noise_suppress_filter", "noise_gate_filter", "compressor_filter",
			       "gain_filter",           "limiter_filter",    "expander_filter",
			       "basic_eq_filter",       "upward_compressor_filter", "invert_polarity_filter"};

QString display(const char *id)
{
	const char *name = obs_source_get_display_name(id);
	return name ? QString::fromUtf8(name) : QString::fromUtf8(id);
}
} // namespace

FiltersDialog::FiltersDialog(obs_source_t *source, QWidget *parent)
	: QDialog(parent), m_source(obs_source_get_ref(source))
{
	setWindowTitle("Filters: " + QString::fromUtf8(obs_source_get_name(source)));
	resize(860, 560);

	m_list = new QListWidget;
	m_list->setFixedWidth(250);
	m_list->setToolTip("Tick to switch a filter on or off. Filters run from top to bottom.");
	auto *addButton = new QPushButton("+  Add filter");
	addButton->setObjectName("primary");
	auto *menu = new QMenu(addButton);
	const uint32_t flags = obs_source_get_output_flags(source);
	auto offer = [this, menu](const char *id) {
		if (obs_get_source_output_flags(id) == 0)
			return; // not loaded
		menu->addAction(display(id), this, [this, id] { add(QString::fromUtf8(id)); });
	};
	if (flags & OBS_SOURCE_VIDEO) {
		menu->addSection("Picture");
		for (const char *id : VIDEO_FILTERS)
			offer(id);
	}
	if (flags & OBS_SOURCE_AUDIO) {
		menu->addSection("Sound");
		for (const char *id : AUDIO_FILTERS)
			offer(id);
	}
	addButton->setMenu(menu);
	auto *up = new QPushButton("↑");
	auto *down = new QPushButton("↓");
	auto *remove = new QPushButton("Remove");
	for (QPushButton *b : {up, down})
		b->setObjectName("icon");
	auto *tools = new QHBoxLayout;
	tools->addWidget(up);
	tools->addWidget(down);
	tools->addStretch();
	tools->addWidget(remove);
	auto *left = new QVBoxLayout;
	left->addWidget(m_list, 1);
	left->addLayout(tools);
	left->addWidget(addButton);

	m_view = new PropertiesView(nullptr);
	auto *body = new QHBoxLayout;
	body->setSpacing(16);
	body->addLayout(left);
	body->addWidget(m_view, 1);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(18, 18, 18, 18);
	layout->addLayout(body, 1);
	layout->addWidget(buttons);

	connect(m_list, &QListWidget::currentRowChanged, this, [this] {
		obs_source_t *filter = selected();
		m_view->setSource(filter);
		obs_source_release(filter);
	});
	connect(m_list, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
		obs_source_t *filter =
			obs_source_get_filter_by_name(m_source, item->data(Qt::UserRole).toString().toUtf8().constData());
		if (filter)
			obs_source_set_enabled(filter, item->checkState() == Qt::Checked);
		obs_source_release(filter);
	});
	auto move = [this](obs_order_movement how) {
		obs_source_t *filter = selected();
		if (!filter)
			return;
		const QString name = QString::fromUtf8(obs_source_get_name(filter));
		obs_source_filter_set_order(m_source, filter, how);
		obs_source_release(filter);
		refresh(name);
	};
	connect(up, &QPushButton::clicked, this, [move] { move(OBS_ORDER_MOVE_UP); });
	connect(down, &QPushButton::clicked, this, [move] { move(OBS_ORDER_MOVE_DOWN); });
	connect(remove, &QPushButton::clicked, this, [this] {
		obs_source_t *filter = selected();
		if (!filter)
			return;
		m_view->setSource(nullptr);
		obs_source_filter_remove(m_source, filter);
		obs_source_release(filter);
		refresh();
	});
	refresh();
}

FiltersDialog::~FiltersDialog()
{
	m_view->setSource(nullptr);
	obs_source_release(m_source);
}

obs_source_t *FiltersDialog::selected() const
{
	QListWidgetItem *item = m_list->currentItem();
	return item ? obs_source_get_filter_by_name(m_source, item->data(Qt::UserRole).toString().toUtf8().constData())
		    : nullptr;
}

void FiltersDialog::refresh(const QString &select)
{
	QSignalBlocker block(m_list);
	m_list->clear();
	obs_source_enum_filters(
		m_source,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			auto *list = static_cast<QListWidget *>(param);
			const QString name = QString::fromUtf8(obs_source_get_name(filter));
			auto *item = new QListWidgetItem(name);
			item->setData(Qt::UserRole, name);
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			item->setCheckState(obs_source_enabled(filter) ? Qt::Checked : Qt::Unchecked);
			item->setToolTip(display(obs_source_get_id(filter)));
			list->addItem(item);
		},
		m_list);
	int row = m_list->count() ? 0 : -1;
	for (int i = 0; i < m_list->count(); i++)
		if (m_list->item(i)->data(Qt::UserRole).toString() == select)
			row = i;
	block.unblock();
	m_list->setCurrentRow(row);
	if (row < 0)
		m_view->setSource(nullptr);
}

void FiltersDialog::add(const QString &id)
{
	QString name = display(id.toUtf8().constData());
	for (int i = 2;; i++) { // names are unique per source
		obs_source_t *taken = obs_source_get_filter_by_name(m_source, name.toUtf8().constData());
		if (!taken)
			break;
		obs_source_release(taken);
		name = QString("%1 %2").arg(display(id.toUtf8().constData())).arg(i);
	}
	obs_source_t *filter = obs_source_create_private(id.toUtf8().constData(), name.toUtf8().constData(), nullptr);
	if (!filter)
		return;
	obs_source_filter_add(m_source, filter);
	obs_source_release(filter);
	refresh(name);
}
