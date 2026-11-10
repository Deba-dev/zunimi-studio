#include "ui/Dialogs.h"

#include "core/ObsCore.h"
#include "ui/PreviewWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
QLabel *hint(const QString &text)
{
	auto *label = new QLabel(text);
	label->setObjectName("hint");
	label->setWordWrap(true);
	return label;
}

QDoubleSpinBox *number(double min, double max, const QString &suffix, int decimals = 0)
{
	auto *box = new QDoubleSpinBox;
	box->setRange(min, max);
	box->setDecimals(decimals);
	box->setSuffix(suffix);
	box->setKeyboardTracking(false);
	return box;
}
} // namespace

// ---- size and place
TransformDialog::TransformDialog(ObsCore *core, const QString &name, QWidget *parent)
	: QDialog(parent), m_core(core), m_name(name)
{
	setWindowTitle("Size and place: " + name);
	m_x = number(-20000, 20000, " px");
	m_y = number(-20000, 20000, " px");
	m_w = number(1, 20000, " px");
	m_h = number(1, 20000, " px");
	m_rot = number(-360, 360, "°", 1);
	m_fit = new QComboBox;
	m_fit->addItem("Fit inside, keep its shape", OBS_BOUNDS_SCALE_INNER);
	m_fit->addItem("Fill the box (edges cut off)", OBS_BOUNDS_SCALE_OUTER);
	m_fit->addItem("Stretch to the box", OBS_BOUNDS_STRETCH);
	m_fit->addItem("Original size (box ignored)", OBS_BOUNDS_NONE);
	const char *sides[] = {"Left", "Top", "Right", "Bottom"};
	auto *cropRow = new QHBoxLayout;
	for (int i = 0; i < 4; i++) {
		m_crop[i] = new QSpinBox;
		m_crop[i]->setRange(0, 16384);
		m_crop[i]->setPrefix(QString(sides[i]) + " ");
		m_crop[i]->setKeyboardTracking(false);
		cropRow->addWidget(m_crop[i]);
	}
	auto *pos = new QHBoxLayout;
	pos->addWidget(m_x);
	pos->addWidget(m_y);
	auto *size = new QHBoxLayout;
	size->addWidget(m_w);
	size->addWidget(m_h);
	auto *form = new QFormLayout;
	form->setVerticalSpacing(10);
	form->addRow("Position (x, y)", pos);
	form->addRow("Size (w, h)", size);
	form->addRow("", hint("Size is before rotation. The position is the top-left corner of what you see."));
	form->addRow("Rotation", m_rot);
	form->addRow("How it fills", m_fit);
	form->addRow("Crop (source pixels)", cropRow);
	form->addRow("", hint("Tip: hold Alt and drag a corner in the preview to crop by eye."));
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	buttons->button(QDialogButtonBox::Ok)->setObjectName("primary");
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(18, 18, 18, 18);
	layout->addLayout(form);
	layout->addWidget(buttons);

	if (obs_sceneitem_t *it = m_core->item(m_name))
		m_before = m_core->itemState(it);
	m_core->beginEdit("Edit size and place");
	load();
	for (QDoubleSpinBox *b : {m_x, m_y, m_w, m_h, m_rot})
		connect(b, &QDoubleSpinBox::valueChanged, this, &TransformDialog::apply);
	for (QSpinBox *b : m_crop)
		connect(b, &QSpinBox::valueChanged, this, &TransformDialog::apply);
	connect(m_fit, &QComboBox::currentIndexChanged, this, &TransformDialog::apply);
}

void TransformDialog::load()
{
	obs_sceneitem_t *it = m_core->item(m_name);
	if (!it)
		return;
	m_loading = true;
	const QRectF box = m_core->itemBox(it);
	m_x->setValue(box.x());
	m_y->setValue(box.y());
	vec2 v{};
	if (obs_sceneitem_get_bounds_type(it) != OBS_BOUNDS_NONE) {
		obs_sceneitem_get_bounds(it, &v);
	} else {
		obs_source_t *src = obs_sceneitem_get_source(it);
		obs_sceneitem_crop c{};
		obs_sceneitem_get_crop(it, &c);
		vec2 s{};
		obs_sceneitem_get_scale(it, &s);
		v.x = std::fabs(s.x) * float(int(obs_source_get_width(src)) - c.left - c.right);
		v.y = std::fabs(s.y) * float(int(obs_source_get_height(src)) - c.top - c.bottom);
	}
	m_w->setValue(v.x);
	m_h->setValue(v.y);
	m_rot->setValue(obs_sceneitem_get_rot(it));
	obs_sceneitem_crop c{};
	obs_sceneitem_get_crop(it, &c);
	m_crop[0]->setValue(c.left);
	m_crop[1]->setValue(c.top);
	m_crop[2]->setValue(c.right);
	m_crop[3]->setValue(c.bottom);
	m_fit->setCurrentIndex(std::max(0, m_fit->findData(obs_sceneitem_get_bounds_type(it))));
	const bool original = obs_sceneitem_get_bounds_type(it) == OBS_BOUNDS_NONE;
	m_w->setEnabled(!original);
	m_h->setEnabled(!original);
	m_loading = false;
}

void TransformDialog::apply()
{
	obs_sceneitem_t *it = m_core->item(m_name);
	if (!it || m_loading)
		return;
	obs_sceneitem_crop c{m_crop[0]->value(), m_crop[1]->value(), m_crop[2]->value(), m_crop[3]->value()};
	obs_sceneitem_set_crop(it, &c);
	obs_sceneitem_set_rot(it, float(m_rot->value()));
	obs_sceneitem_set_alignment(it, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
	const auto type = obs_bounds_type(m_fit->currentData().toInt());
	vec2 v{};
	obs_sceneitem_get_scale(it, &v);
	const float sx = v.x < 0 ? -1.0f : 1.0f, sy = v.y < 0 ? -1.0f : 1.0f;
	if (type == OBS_BOUNDS_NONE) {
		obs_sceneitem_set_bounds_type(it, OBS_BOUNDS_NONE);
		if (obs_sceneitem_get_bounds_type(it) != OBS_BOUNDS_NONE || m_w->isEnabled()) {
			vec2_set(&v, sx, sy);
			obs_sceneitem_set_scale(it, &v);
		}
	} else {
		if (obs_sceneitem_get_bounds_type(it) == OBS_BOUNDS_NONE) {
			vec2_set(&v, sx, sy);
			obs_sceneitem_set_scale(it, &v);
		}
		obs_sceneitem_set_bounds_type(it, type);
		obs_sceneitem_set_bounds_alignment(it, OBS_ALIGN_CENTER);
		vec2_set(&v, float(m_w->value()), float(m_h->value()));
		obs_sceneitem_set_bounds(it, &v);
	}
	m_w->setEnabled(type != OBS_BOUNDS_NONE);
	m_h->setEnabled(type != OBS_BOUNDS_NONE);
	// place the visible box's top-left exactly where asked, whatever the rotation or flip
	vec2_set(&v, float(m_x->value()), float(m_y->value()));
	obs_sceneitem_set_pos(it, &v);
	const QRectF box = m_core->itemBox(it);
	vec2_set(&v, float(2 * m_x->value() - box.x()), float(2 * m_y->value() - box.y()));
	obs_sceneitem_set_pos(it, &v);
}

void TransformDialog::done(int result)
{
	if (result == QDialog::Rejected)
		if (obs_sceneitem_t *it = m_core->item(m_name))
			m_core->setItemState(it, m_before); // back as it was, so endEdit records nothing
	m_core->endEdit();
	QDialog::done(result);
}

// ---- advanced audio
AudioDialog::AudioDialog(ObsCore *core, obs_source_t *source, QWidget *parent)
	: QDialog(parent), m_core(core), m_source(source)
{
	const QString name = source == core->desktopAudio() ? "Desktop audio"
			     : source == core->microphone()  ? "Mic"
							     : QString::fromUtf8(obs_source_get_name(source));
	setWindowTitle("Sound settings: " + name);
	auto *monitor = new QComboBox;
	monitor->addItem("Off", 0);
	monitor->addItem("Hear it too (also in the stream)", 1);
	monitor->addItem("Only I hear it (not in stream or recording)", 2);
	const bool on = obs_source_get_monitoring_enabled(source);
	const uint32_t mixers = obs_source_get_audio_mixers(source);
	monitor->setCurrentIndex(!on ? 0 : mixers == 0 ? 2 : 1);

	auto *sync = new QSpinBox;
	sync->setRange(-950, 20000);
	sync->setSingleStep(10);
	sync->setSuffix(" ms");
	sync->setValue(int(obs_source_get_sync_offset(source) / 1000000LL));

	auto *balance = new QSlider(Qt::Horizontal);
	balance->setRange(0, 100);
	balance->setValue(int(std::lround(obs_source_get_balance_value(source) * 100.0f)));
	auto *balanceText = new QLabel;
	balanceText->setObjectName("mute");
	auto showBalance = [balanceText](int v) {
		balanceText->setText(v == 50 ? "Centre" : v < 50 ? QString("%1% left").arg((50 - v) * 2)
							      : QString("%1% right").arg((v - 50) * 2));
	};
	showBalance(balance->value());
	auto *balanceRow = new QHBoxLayout;
	balanceRow->addWidget(balance, 1);
	balanceRow->addWidget(balanceText);

	auto *tracks = new QHBoxLayout;
	QCheckBox *track[6];
	for (int i = 0; i < 6; i++) {
		track[i] = new QCheckBox(QString::number(i + 1));
		track[i]->setChecked(mixers & (1u << i));
		tracks->addWidget(track[i]);
	}
	tracks->addStretch();

	auto *form = new QFormLayout;
	form->setVerticalSpacing(10);
	form->addRow("Listen in headphones", monitor);
	form->addRow("", hint("Pick your headphones in Settings > Audio. \"Only I hear it\" is good for a music cue "
			      "or a call you don't want on stream."));
	form->addRow("Sync offset", sync);
	form->addRow("", hint("Your voice arrives before the webcam picture? Delay the mic by about 100-300 ms."));
	form->addRow("Balance", balanceRow);
	form->addRow("Recording tracks", tracks);
	form->addRow("", hint("Recordings can keep each track as its own sound, so an editor can lower the game "
			      "without touching your voice. The stream uses the track set in Settings > Stream."));

	auto apply = [this, monitor, sync, balance, track] {
		uint32_t m = 0;
		for (int i = 0; i < 6; i++)
			if (track[i]->isChecked())
				m |= 1u << i;
		const int mode = monitor->currentIndex();
		obs_source_set_monitoring_enabled(m_source, mode != 0);
		obs_source_set_audio_mixers(m_source, mode == 2 ? 0 : (m ? m : 1));
		for (QCheckBox *c : track)
			c->setEnabled(mode != 2);
		obs_source_set_sync_offset(m_source, int64_t(sync->value()) * 1000000LL);
		obs_source_set_balance_value(m_source, float(balance->value()) / 100.0f);
		m_core->saveAudioSettings(m_source);
	};
	if (monitor->currentIndex() == 2)
		for (QCheckBox *c : track)
			c->setEnabled(false);
	connect(monitor, &QComboBox::currentIndexChanged, this, apply);
	connect(sync, &QSpinBox::valueChanged, this, apply);
	connect(balance, &QSlider::valueChanged, this, [apply, showBalance](int v) {
		showBalance(v);
		apply();
	});
	for (QCheckBox *c : track)
		connect(c, &QCheckBox::toggled, this, apply);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(18, 18, 18, 18);
	layout->addLayout(form);
	layout->addWidget(buttons);
	resize(520, 0);
}

// ---- stats
StatsDialog::StatsDialog(ObsCore *core, QWidget *parent) : QDialog(parent), m_core(core)
{
	setWindowTitle("Stats");
	m_text = new QLabel;
	m_text->setTextFormat(Qt::RichText);
	m_text->setTextInteractionFlags(Qt::TextSelectableByMouse);
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(18, 18, 18, 18);
	layout->addWidget(m_text);
	m_timer = new QTimer(this);
	connect(m_timer, &QTimer::timeout, this, &StatsDialog::refresh);
	m_timer->start(1000);
	refresh();
	resize(460, 0);
}

void StatsDialog::refresh()
{
	const Health h = m_core->health();
	auto row = [](const QString &k, const QString &v) {
		return QString("<tr><td style='color:#8A93A6;padding-right:18px'>%1</td><td>%2</td></tr>").arg(k, v);
	};
	auto clock = [](qint64 s) {
		return QString("%1:%2:%3").arg(s / 3600, 2, 10, QChar('0')).arg((s / 60) % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
	};
	QString html = "<table cellspacing=4>";
	html += row("Frame rate", QString("%1 fps").arg(h.fps, 0, 'f', 1));
	html += row("App CPU", QString("%1%").arg(h.cpu, 0, 'f', 1));
	html += row("App memory", QString("%1 MB").arg(h.ramMb, 0, 'f', 0));
	html += row("Missed by the GPU", QString("%1%").arg(h.renderLag, 0, 'f', 1));
	html += row("Skipped by the encoder", QString("%1%").arg(h.encodeLag, 0, 'f', 1));
	const OutputStats stream = m_core->streamStats();
	if (stream.active) {
		html += "<tr><td colspan=2 style='padding-top:10px'><b>Stream</b></td></tr>";
		html += row("Live for", clock(stream.seconds));
		html += row("Bitrate", QString("%1 kbps (target %2)").arg(stream.kbps, 0, 'f', 0).arg(h.bitrate));
		html += row("Dropped by the network", QString("%1 of %2 (%3%)")
							   .arg(stream.droppedFrames)
							   .arg(stream.totalFrames)
							   .arg(stream.totalFrames ? 100.0 * stream.droppedFrames / stream.totalFrames : 0.0, 0, 'f', 2));
		html += row("Congestion", QString("%1%").arg(stream.congestion));
		html += row("Sent", QString("%1 MB").arg(stream.bytes / 1048576.0, 0, 'f', 1));
	}
	const OutputStats rec = m_core->recordStats();
	if (rec.active) {
		html += "<tr><td colspan=2 style='padding-top:10px'><b>Recording</b></td></tr>";
		html += row("Length", clock(rec.seconds) + (m_core->recordingPaused() ? "  (paused)" : ""));
		html += row("Bitrate", QString("%1 kbps").arg(rec.kbps, 0, 'f', 0));
		html += row("Size", QString("%1 MB").arg(rec.bytes / 1048576.0, 0, 'f', 1));
	}
	if (!stream.active && !rec.active)
		html += "<tr><td colspan=2 style='padding-top:10px;color:#8A93A6'>Stream and recording numbers appear "
			"here once you go live or record.</td></tr>";
	html += "</table>";
	m_text->setText(html);
}

// ---- projector
ProjectorWindow::ProjectorWindow(ObsCore *core, obs_source_t *source, int screen, QWidget *parent)
	: QWidget(parent, Qt::Window)
{
	setAttribute(Qt::WA_DeleteOnClose);
	const QString what = source ? QString::fromUtf8(obs_source_get_name(source)) : "Program";
	setWindowTitle("Projector: " + what);
	m_view = new PreviewWidget(core, source ? PreviewWidget::Mode::Source : PreviewWidget::Mode::Program);
	if (source)
		m_view->setSource(source);
	m_view->setAttribute(Qt::WA_TransparentForMouseEvents);
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(m_view);
	setStyleSheet("background: black;");
	const QList<QScreen *> screens = QGuiApplication::screens();
	if (screen >= 0 && screen < screens.size()) {
		setGeometry(screens[screen]->geometry());
		showFullScreen();
	} else {
		resize(960, 540);
		show();
	}
}

ProjectorWindow::~ProjectorWindow() = default;

void ProjectorWindow::keyPressEvent(QKeyEvent *event)
{
	if (event->key() == Qt::Key_Escape)
		close();
	else
		QWidget::keyPressEvent(event);
}

void ProjectorWindow::mouseDoubleClickEvent(QMouseEvent *)
{
	if (isFullScreen())
		close();
	else
		showFullScreen();
}
