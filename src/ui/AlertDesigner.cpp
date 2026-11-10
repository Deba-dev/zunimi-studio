#include "ui/AlertDesigner.h"

#include "core/Settings.h"
#include "events/Events.h"
#include "overlays/Overlays.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

using namespace overlays;

namespace {
QLabel *hint(const QString &text)
{
	auto *label = new QLabel(text);
	label->setObjectName("hint");
	label->setWordWrap(true);
	return label;
}

StreamEvent sample(const QString &type)
{
	StreamEvent e;
	e.type = type;
	e.user = "PixelNinja";
	e.platform = "test";
	if (type == "tip" || type == "superchat") {
		e.amount = 500;
		e.currency = Settings::get().value("events/currency", "INR").toString();
		e.message = "Bhai kya clutch tha! 🔥";
	} else if (type == "bits") {
		e.amount = 500;
		e.message = "Cheer500 GG";
	} else if (type == "raid") {
		e.count = 42;
	} else if (type == "gift") {
		e.count = 5;
	} else if (type == "resub") {
		e.count = 7;
		e.message = "7 months already!";
	}
	return e;
}
} // namespace

AlertDesigner::AlertDesigner(obs_source_t *source, QWidget *parent) : QDialog(parent), m_source(source)
{
	setWindowTitle("Alert designer");
	resize(1100, 720);
	obs_data_t *settings = obs_source_get_settings(source);
	const QString text = QString::fromUtf8(obs_data_get_string(settings, "design"));
	obs_data_release(settings);
	m_design = text.isEmpty() ? defaultDesign() : QJsonDocument::fromJson(text.toUtf8()).object();

	// left: the alert types
	m_types = new QListWidget;
	m_types->setFixedWidth(190);
	for (const QString &t : alertTypes()) {
		auto *item = new QListWidgetItem(alertTypeLabel(t));
		item->setData(Qt::UserRole, t);
		m_types->addItem(item);
	}
	m_theme = new QComboBox;
	m_theme->addItem("Pick a look for all...", "");
	for (const QString &t : themes())
		m_theme->addItem(themeLabel(t), t);
	connect(m_theme, &QComboBox::activated, this, [this] {
		const QString t = m_theme->currentData().toString();
		if (!t.isEmpty())
			applyTheme(t);
		m_theme->setCurrentIndex(0);
	});
	auto *left = new QVBoxLayout;
	left->addWidget(m_types, 1);
	left->addWidget(m_theme);

	// middle: the form
	m_on = new QCheckBox("Show this alert");
	m_title = new QLineEdit;
	m_message = new QCheckBox("Show their message");
	m_image = new QLineEdit;
	m_image->setPlaceholderText("Empty: a glowing emoji badge");
	m_sound = new QLineEdit;
	m_sound->setPlaceholderText("Empty: no sound");
	m_volume = new QSpinBox;
	m_volume->setRange(0, 100);
	m_volume->setSuffix(" %");
	m_seconds = new QSpinBox;
	m_seconds->setRange(2, 60);
	m_seconds->setSuffix(" s");
	m_animation = new QComboBox;
	m_animation->addItem("Pop", "pop");
	m_animation->addItem("Slide up", "slide");
	m_animation->addItem("Bounce in", "bounce");
	m_animation->addItem("Fade", "fade");
	m_layout = new QComboBox;
	m_layout->addItem("Picture above the text", "above");
	m_layout->addItem("Picture beside the text", "side");
	m_layout->addItem("Text only", "text");
	m_font = new QFontComboBox;
	m_size = new QSpinBox;
	m_size->setRange(12, 160);
	m_size->setSuffix(" px");
	m_color = new QLineEdit;
	m_accent = new QLineEdit;
	auto colorRow = [this](QLineEdit *edit) {
		auto *pick = new QPushButton("Pick...");
		connect(pick, &QPushButton::clicked, this, [this, edit] {
			const QColor c = QColorDialog::getColor(QColor(edit->text()), this);
			if (c.isValid())
				edit->setText(c.name());
		});
		auto *w = new QWidget;
		auto *row = new QHBoxLayout(w);
		row->setContentsMargins(0, 0, 0, 0);
		row->addWidget(edit, 1);
		row->addWidget(pick);
		return w;
	};
	m_min = new QDoubleSpinBox;
	m_min->setRange(0, 1e9);
	m_min->setDecimals(0);
	m_tts = new QCheckBox("Read their message aloud (text to speech)");
	m_ttsMin = new QDoubleSpinBox;
	m_ttsMin->setRange(0, 1e9);
	m_ttsMin->setDecimals(0);
	m_variants = new QTableWidget(0, 4);
	m_variants->setHorizontalHeaderLabels({"From amount", "Title", "Picture", "Sound"});
	m_variants->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	m_variants->setMinimumHeight(110);
	auto *addVariant = new QPushButton("Add a bigger-amount version");
	auto *removeVariant = new QPushButton("Remove");
	connect(addVariant, &QPushButton::clicked, this, [this] {
		const int r = m_variants->rowCount();
		m_variants->insertRow(r);
		m_variants->setItem(r, 0, new QTableWidgetItem("1000"));
		m_variants->setItem(r, 1, new QTableWidgetItem("{name} went HUGE: {amount}!"));
		m_variants->setItem(r, 2, new QTableWidgetItem(""));
		m_variants->setItem(r, 3, new QTableWidgetItem(""));
		store();
	});
	connect(removeVariant, &QPushButton::clicked, this, [this] {
		m_variants->removeRow(m_variants->currentRow());
		store();
	});
	auto *variantButtons = new QHBoxLayout;
	variantButtons->addWidget(addVariant);
	variantButtons->addWidget(removeVariant);
	variantButtons->addStretch();

	auto *form = new QFormLayout;
	form->setVerticalSpacing(8);
	form->addRow("", m_on);
	form->addRow("Title", m_title);
	form->addRow("", hint("Words in braces fill in: {name}, {amount}, {count}, {months}, {message}."));
	form->addRow("", m_message);
	form->addRow("Picture or GIF", fileRow(m_image, "Pictures (*.png *.jpg *.jpeg *.gif *.webp)"));
	form->addRow("Sound", fileRow(m_sound, "Sounds (*.mp3 *.wav *.ogg *.m4a)"));
	form->addRow("Sound volume", m_volume);
	form->addRow("On screen for", m_seconds);
	form->addRow("Comes in with", m_animation);
	form->addRow("Layout", m_layout);
	form->addRow("Font", m_font);
	form->addRow("Text size", m_size);
	form->addRow("Text colour", colorRow(m_color));
	form->addRow("Highlight colour", colorRow(m_accent));
	form->addRow("Only from (amount)", m_min);
	form->addRow("", m_tts);
	form->addRow("Read aloud from (amount)", m_ttsMin);
	form->addRow("Bigger amounts", m_variants);
	form->addRow("", variantButtons);
	auto *formBox = new QWidget;
	formBox->setLayout(form);
	auto *scroll = new QScrollArea;
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll->setWidget(formBox);
	for (QLineEdit *e : {m_image, m_sound})
		e->setMinimumWidth(120);

	// right: live preview and test
	m_preview = new QLabel;
	m_preview->setMinimumSize(400, 225);
	m_preview->setAlignment(Qt::AlignCenter);
	m_preview->setStyleSheet("background: qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 #1d2b3a, stop:1 #3a2440); border-radius: 12px;");
	auto *replay = new QPushButton("Play the preview again");
	auto *test = new QPushButton("Test on stream");
	test->setObjectName("primary");
	test->setToolTip("Sends a pretend event: every alert box shows it (and plays its sound) exactly as viewers would see it");
	connect(replay, &QPushButton::clicked, this, [this] { m_previewStart = QDateTime::currentMSecsSinceEpoch(); });
	connect(test, &QPushButton::clicked, this, [this] {
		save();
		EventHub::get().test(m_type);
	});
	auto *voice = new QComboBox;
	voice->addItem("Windows default voice", "");
	for (const QString &v : voices())
		voice->addItem(v, v);
	voice->setCurrentIndex(std::max(0, voice->findData(Settings::get().value("alerts/voice").toString())));
	connect(voice, &QComboBox::currentIndexChanged, this,
		[voice] { Settings::get().set("alerts/voice", voice->currentData()); });
	auto *hear = new QCheckBox("I hear alert sounds in my headphones too");
	hear->setChecked(Settings::get().value("alerts/hear", true).toBool());
	connect(hear, &QCheckBox::toggled, this, [](bool on) { Settings::get().set("alerts/hear", on); });
	auto *exportBtn = new QPushButton("Export design...");
	auto *importBtn = new QPushButton("Import design...");
	connect(exportBtn, &QPushButton::clicked, this, [this] {
		store();
		const QString file = QFileDialog::getSaveFileName(this, "Export alert design",
								  QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/My alerts.zalerts",
								  "Zunimi alert design (*.zalerts)");
		QFile f(file);
		if (!file.isEmpty() && f.open(QIODevice::WriteOnly))
			f.write(QJsonDocument(m_design).toJson());
	});
	connect(importBtn, &QPushButton::clicked, this, [this] {
		const QString file = QFileDialog::getOpenFileName(this, "Import alert design", {}, "Zunimi alert design (*.zalerts *.json)");
		QFile f(file);
		if (file.isEmpty() || !f.open(QIODevice::ReadOnly))
			return;
		const QJsonObject d = QJsonDocument::fromJson(f.readAll()).object();
		if (!d.contains("follow")) {
			QMessageBox::warning(this, "Import", "That file isn't an alert design.");
			return;
		}
		m_design = d;
		load(m_type);
	});
	auto *right = new QVBoxLayout;
	right->addWidget(m_preview);
	right->addWidget(replay);
	right->addWidget(test);
	right->addSpacing(12);
	right->addWidget(new QLabel("Voice for reading messages"));
	right->addWidget(voice);
	right->addWidget(hint("Hindi voices appear here once Windows has the Hindi speech pack (Settings > Time & "
			      "language > Speech)."));
	right->addWidget(hear);
	right->addStretch();
	right->addWidget(exportBtn);
	right->addWidget(importBtn);

	auto *body = new QHBoxLayout;
	body->addLayout(left);
	body->addWidget(scroll, 1);
	body->addLayout(right);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
	buttons->button(QDialogButtonBox::Save)->setObjectName("primary");
	connect(buttons, &QDialogButtonBox::accepted, this, [this] {
		save();
		accept();
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(18, 18, 18, 18);
	layout->addLayout(body, 1);
	layout->addWidget(buttons);

	connect(m_types, &QListWidget::currentRowChanged, this, [this](int row) {
		if (row < 0)
			return;
		store();
		load(m_types->item(row)->data(Qt::UserRole).toString());
	});
	for (QLineEdit *e : {m_title, m_image, m_sound, m_color, m_accent})
		connect(e, &QLineEdit::textChanged, this, &AlertDesigner::store);
	for (QSpinBox *s : {m_volume, m_seconds, m_size})
		connect(s, &QSpinBox::valueChanged, this, &AlertDesigner::store);
	for (QDoubleSpinBox *s : {m_min, m_ttsMin})
		connect(s, &QDoubleSpinBox::valueChanged, this, &AlertDesigner::store);
	for (QCheckBox *c : {m_on, m_message, m_tts})
		connect(c, &QCheckBox::toggled, this, &AlertDesigner::store);
	for (QComboBox *c : {m_animation, m_layout})
		connect(c, &QComboBox::currentIndexChanged, this, &AlertDesigner::store);
	connect(m_font, &QFontComboBox::currentFontChanged, this, &AlertDesigner::store);
	connect(m_variants, &QTableWidget::cellChanged, this, &AlertDesigner::store);

	m_animate = new QTimer(this);
	connect(m_animate, &QTimer::timeout, this, &AlertDesigner::preview);
	m_animate->start(33);
	m_types->setCurrentRow(0);
}

QWidget *AlertDesigner::fileRow(QLineEdit *edit, const QString &filter)
{
	auto *pick = new QPushButton("Choose...");
	connect(pick, &QPushButton::clicked, this, [this, edit, filter] {
		const QString file = QFileDialog::getOpenFileName(this, "Choose a file", QFileInfo(edit->text()).absolutePath(), filter);
		if (!file.isEmpty())
			edit->setText(file);
	});
	auto *clear = new QPushButton("✕");
	clear->setObjectName("icon");
	connect(clear, &QPushButton::clicked, edit, &QLineEdit::clear);
	auto *w = new QWidget;
	auto *row = new QHBoxLayout(w);
	row->setContentsMargins(0, 0, 0, 0);
	row->addWidget(edit, 1);
	row->addWidget(pick);
	row->addWidget(clear);
	return w;
}

void AlertDesigner::load(const QString &type)
{
	m_type = type;
	const AlertStyle s = AlertStyle::fromJson(m_design.value(type).toObject(), type);
	m_loading = true;
	m_on->setChecked(s.on);
	m_title->setText(s.title);
	m_message->setChecked(s.showMessage);
	m_image->setText(s.image);
	m_sound->setText(s.sound);
	m_volume->setValue(s.volume);
	m_seconds->setValue(s.seconds);
	m_animation->setCurrentIndex(std::max(0, m_animation->findData(s.animation)));
	m_layout->setCurrentIndex(std::max(0, m_layout->findData(s.layout)));
	m_font->setCurrentFont(QFont(s.font));
	m_size->setValue(s.size);
	m_color->setText(s.color);
	m_accent->setText(s.accent);
	m_min->setValue(s.min);
	m_tts->setChecked(s.tts);
	m_ttsMin->setValue(s.ttsMin);
	const bool money = type == "tip" || type == "superchat" || type == "bits";
	m_min->setEnabled(money);
	m_variants->setRowCount(0);
	for (const AlertStyle::Variant &v : s.variants) {
		const int r = m_variants->rowCount();
		m_variants->insertRow(r);
		m_variants->setItem(r, 0, new QTableWidgetItem(QString::number(v.min)));
		m_variants->setItem(r, 1, new QTableWidgetItem(v.title));
		m_variants->setItem(r, 2, new QTableWidgetItem(v.image));
		m_variants->setItem(r, 3, new QTableWidgetItem(v.sound));
	}
	m_loading = false;
	m_previewStart = QDateTime::currentMSecsSinceEpoch();
}

void AlertDesigner::store()
{
	if (m_loading || m_type.isEmpty())
		return;
	AlertStyle s;
	s.on = m_on->isChecked();
	s.title = m_title->text();
	s.showMessage = m_message->isChecked();
	s.image = m_image->text().trimmed();
	s.sound = m_sound->text().trimmed();
	s.volume = m_volume->value();
	s.seconds = m_seconds->value();
	s.animation = m_animation->currentData().toString();
	s.layout = m_layout->currentData().toString();
	s.font = m_font->currentFont().family();
	s.size = m_size->value();
	s.color = QColor(m_color->text()).isValid() ? m_color->text() : "#FFFFFF";
	s.accent = QColor(m_accent->text()).isValid() ? m_accent->text() : "#22D3EE";
	s.min = m_min->value();
	s.tts = m_tts->isChecked();
	s.ttsMin = m_ttsMin->value();
	for (int r = 0; r < m_variants->rowCount(); r++) {
		auto cell = [this, r](int c) { return m_variants->item(r, c) ? m_variants->item(r, c)->text() : QString(); };
		s.variants.append({cell(0).toDouble(), cell(1), cell(2), cell(3)});
	}
	m_design.insert(m_type, s.toJson());
}

void AlertDesigner::preview()
{
	if (m_type.isEmpty())
		return;
	const AlertStyle s = AlertStyle::fromJson(m_design.value(m_type).toObject(), m_type);
	const qint64 ms = QDateTime::currentMSecsSinceEpoch() - m_previewStart;
	const double cycle = s.seconds * 1000.0 + 800.0;
	const double p = std::fmod(double(ms), cycle) / (s.seconds * 1000.0);
	const QSize size(800, 450);
	static QString lastImage;
	static QImage picture;
	if (s.image != lastImage) {
		lastImage = s.image;
		picture = QImage(s.image);
	}
	const QImage frame = paintAlert(s, sample(m_type), size, std::clamp(p, 0.0, 1.0), picture);
	m_preview->setPixmap(QPixmap::fromImage(frame).scaled(m_preview->width() - 8, m_preview->height() - 8,
							       Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void AlertDesigner::applyTheme(const QString &theme)
{
	store();
	const QJsonObject fresh = defaultDesign(theme);
	for (const QString &t : alertTypes()) {
		QJsonObject o = m_design.value(t).toObject();
		const QJsonObject f = fresh.value(t).toObject();
		for (const char *k : {"color", "accent", "font", "animation"})
			o.insert(k, f.value(k));
		m_design.insert(t, o);
	}
	m_design.insert("theme", theme);
	load(m_type);
}

void AlertDesigner::save()
{
	store();
	obs_data_t *settings = obs_source_get_settings(m_source);
	obs_data_set_string(settings, "design", QJsonDocument(m_design).toJson(QJsonDocument::Compact).constData());
	obs_source_update(m_source, settings);
	obs_data_release(settings);
}
