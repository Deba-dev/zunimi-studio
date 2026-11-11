#include "ui/Mixer.h"
#include "core/I18n.h"

#include "core/ObsCore.h"
#include "core/Settings.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <obs-audio-controls.h>

#include <algorithm>
#include <cmath>

namespace {
const float FLOOR_DB = -60.0f;

// The slider feels even: it moves in perceptual steps, libobs wants a multiplier.
float toMul(int slider)
{
	return slider <= 0 ? 0.0f : float(std::pow(slider / 100.0, 2.0));
}

int toSlider(float mul)
{
	return int(std::round(std::sqrt(std::max(0.0f, mul)) * 100.0));
}

QString settingsKey(obs_source_t *source, ObsCore *core)
{
	if (source == core->desktopAudio())
		return "desktop";
	if (source == core->microphone())
		return "mic";
	return {}; // scene sources keep their volume in the scene file
}
} // namespace

LevelMeter::LevelMeter(obs_source_t *source, QWidget *parent) : QWidget(parent)
{
	setFixedHeight(8);
	setMinimumWidth(80);
	m_meter = obs_volmeter_create(OBS_FADER_LOG);
	obs_volmeter_attach_source(m_meter, source);
	obs_volmeter_add_callback(m_meter, onLevels, this);
}

LevelMeter::~LevelMeter()
{
	obs_volmeter_remove_callback(m_meter, onLevels, this);
	obs_volmeter_destroy(m_meter);
}

void LevelMeter::onLevels(void *param, const float[MAX_AUDIO_CHANNELS], const float peak[MAX_AUDIO_CHANNELS],
			  const float[MAX_AUDIO_CHANNELS])
{
	float loudest = -100.0f;
	for (int i = 0; i < MAX_AUDIO_CHANNELS; i++)
		if (std::isfinite(peak[i]))
			loudest = std::max(loudest, peak[i]);
	static_cast<LevelMeter *>(param)->m_level.store(loudest, std::memory_order_relaxed);
}

void LevelMeter::tick()
{
	const float level = m_level.exchange(-100.0f, std::memory_order_relaxed);
	// rise at once, fall smoothly, and hold the peak mark for a second
	m_shown = level > m_shown ? level : std::max(level, m_shown - 1.5f);
	if (level >= m_hold) {
		m_hold = level;
		m_holdTicks = 30;
	} else if (--m_holdTicks <= 0) {
		m_hold = std::max(-100.0f, m_hold - 1.0f);
	}
	update();
}

void LevelMeter::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	const QRectF r = rect();
	p.setPen(Qt::NoPen);
	p.setBrush(Theme::Line);
	p.drawRoundedRect(r, 4, 4);
	auto x = [&r](float db) {
		return r.width() * std::clamp((db - FLOOR_DB) / -FLOOR_DB, 0.0f, 1.0f);
	};
	const double w = x(m_shown);
	if (w > 0) {
		QLinearGradient g(r.topLeft(), r.topRight());
		g.setColorAt(0.0, Theme::Good);
		g.setColorAt(0.75, Theme::Good);   // -15 dB
		g.setColorAt(0.85, Theme::Warn);   // -9 dB
		g.setColorAt(1.0, Theme::Live);
		p.setBrush(g);
		p.setClipRect(QRectF(r.x(), r.y(), w, r.height()));
		p.drawRoundedRect(r, 4, 4);
		p.setClipping(false);
	}
	if (m_hold > FLOOR_DB) {
		p.setBrush(m_hold > -3.0f ? Theme::Live : Theme::Text);
		p.drawRect(QRectF(std::min(x(m_hold), r.width() - 2), r.y(), 2, r.height()));
	}
}

Mixer::Mixer(ObsCore *core, QWidget *parent) : QWidget(parent), m_core(core)
{
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0); // it lives in the Audio Mixer dock, whose title bar names it
	outer->setSpacing(8);
	auto *area = new QScrollArea;
	area->setWidgetResizable(true);
	area->setFrameShape(QFrame::NoFrame);
	area->setObjectName("clear");
	auto *inner = new QWidget;
	inner->setObjectName("clearInner");
	m_list = new QVBoxLayout(inner);
	m_list->setContentsMargins(0, 0, 4, 0);
	m_list->setSpacing(12);
	m_list->addStretch();
	area->setWidget(inner);
	auto *cleanup = new QCheckBox(L("Clean up mic noise"));
	cleanup->setToolTip(L("RNNoise: removes keyboard, fan and room noise. Light on the CPU."));
	cleanup->setChecked(core->micCleanup());
	cleanup->setEnabled(core->microphone() != nullptr);
	connect(cleanup, &QCheckBox::toggled, core, &ObsCore::setMicCleanup);
	outer->addWidget(area, 1);
	outer->addWidget(cleanup);

	m_timer = new QTimer(this);
	connect(m_timer, &QTimer::timeout, this, [this] {
		for (LevelMeter *m : m_meters)
			m->tick();
	});
	m_timer->start(33);
	// a game capture starts sending sound only once the game is hooked, so the list is checked now and then
	auto *recheck = new QTimer(this);
	connect(recheck, &QTimer::timeout, this, [this] { refresh(); });
	recheck->start(2000);
	connect(core, &ObsCore::sceneChanged, this, [this] { refresh(); });
	refresh();
}

void Mixer::refresh(bool force)
{
	const QVector<obs_source_t *> sources = m_core->audioSources();
	if (!force && std::equal(sources.begin(), sources.end(), m_shown.begin(), m_shown.end()))
		return;
	m_meters.clear();
	while (m_list->count() > 1) { // keep the stretch at the end
		QLayoutItem *item = m_list->takeAt(0);
		delete item->widget();
		delete item;
	}
	m_shown.assign(sources.begin(), sources.end());
	for (int i = 0; i < sources.size(); i++)
		m_list->insertWidget(i, channel(sources[i]));
}

QWidget *Mixer::channel(obs_source_t *source)
{
	auto *box = new QWidget;
	auto *col = new QVBoxLayout(box);
	col->setContentsMargins(0, 0, 0, 0);
	col->setSpacing(4);
	const QString key = settingsKey(source, m_core);
	const QString name = key == "desktop" ? L("Desktop") : key == "mic" ? L("Mic") : QString::fromUtf8(obs_source_get_name(source));

	auto *head = new QHBoxLayout;
	head->setSpacing(4);
	auto *label = new QLabel(name);
	label->setToolTip(name);
	auto *mute = new QPushButton;
	mute->setObjectName("icon");
	mute->setCheckable(true);
	mute->setCursor(Qt::PointingHandCursor);
	auto *filters = new QPushButton(L("Filters"));
	filters->setObjectName("icon");
	filters->setCursor(Qt::PointingHandCursor);
	filters->setToolTip(L("Noise gate, compressor, gain, EQ..."));
	auto *more = new QPushButton("⋯");
	more->setObjectName("icon");
	more->setCursor(Qt::PointingHandCursor);
	more->setToolTip(L("Listen in headphones, sync offset, balance, recording tracks"));
	head->addWidget(label, 1);
	head->addWidget(mute);
	head->addWidget(filters);
	const bool easy = Settings::get().value("ui/simple", false).toBool(); // Simple mode: just level and mute
	filters->setVisible(!easy);
	more->setVisible(!easy);
	head->addWidget(more);

	auto *meter = new LevelMeter(source);
	m_meters.push_back(meter);
	auto *slider = new QSlider(Qt::Horizontal);
	slider->setRange(0, 100);
	slider->setValue(toSlider(obs_source_get_volume(source)));
	auto *db = new QLabel;
	db->setObjectName("mute");
	db->setFixedWidth(52);
	db->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	auto showDb = [db](float mul) {
		db->setText(mul <= 0.0f ? L("-inf dB") : QString(L("%1 dB")).arg(20.0 * std::log10(mul), 0, 'f', 1));
	};
	showDb(obs_source_get_volume(source));
	auto *row = new QHBoxLayout;
	row->addWidget(slider, 1);
	row->addWidget(db);

	auto setMuteText = [mute](bool on) { mute->setText(on ? L("Muted") : L("Mute")); };
	mute->setChecked(obs_source_muted(source));
	setMuteText(mute->isChecked());

	obs_source_t *ref = source; // rows are rebuilt whenever the source list changes, so this stays valid
	connect(slider, &QSlider::valueChanged, this, [this, ref, key, showDb](int v) {
		obs_source_set_volume(ref, toMul(v));
		showDb(toMul(v));
		if (!key.isEmpty())
			Settings::get().set("audio/" + key + "Volume", double(toMul(v)));
		else
			m_core->save();
	});
	connect(mute, &QPushButton::toggled, this, [this, ref, key, setMuteText](bool on) {
		obs_source_set_muted(ref, on);
		setMuteText(on);
		if (!key.isEmpty())
			Settings::get().set("audio/" + key + "Muted", on);
		else
			m_core->save();
	});
	connect(filters, &QPushButton::clicked, this, [this, ref] { emit filtersRequested(ref); });
	connect(more, &QPushButton::clicked, this, [this, ref] { emit audioRequested(ref); });

	col->addLayout(head);
	col->addWidget(meter);
	col->addLayout(row);
	return box;
}
