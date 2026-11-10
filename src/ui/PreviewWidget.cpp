#include "ui/PreviewWidget.h"

#include "core/ObsCore.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QWindow>

#include <graphics/matrix4.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <algorithm>
#include <cmath>

namespace {
const double HANDLE_PX = 9.0; // how close to a corner (in screen pixels) grabs it
const double SNAP_PX = 10.0;
} // namespace

PreviewWidget::PreviewWidget(ObsCore *core, QWidget *parent) : QWidget(parent), m_core(core)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_NativeWindow);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setMinimumSize(320, 180);
	setMouseTracking(true);
	setFocusPolicy(Qt::ClickFocus);
}

PreviewWidget::~PreviewWidget()
{
	if (m_display) {
		obs_display_remove_draw_callback(m_display, draw, this);
		obs_display_destroy(m_display);
	}
	setSelected(nullptr, false);
}

void PreviewWidget::create()
{
	if (m_display || !windowHandle())
		return;
	const qreal dpr = devicePixelRatioF();
	gs_init_data info{};
	info.window.hwnd = reinterpret_cast<void *>(winId());
	info.cx = uint32_t(std::max(1.0, width() * dpr));
	info.cy = uint32_t(std::max(1.0, height() * dpr));
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
	info.num_backbuffers = 1;
	m_display = obs_display_create(&info, 0x120D0B); // 0xBBGGRR: the app background, #0B0D12
	if (m_display) {
		obs_display_add_draw_callback(m_display, draw, this);
		obs_display_set_enabled(m_display, m_running);
	}
}

void PreviewWidget::setRunning(bool on)
{
	m_running = on;
	if (m_display)
		obs_display_set_enabled(m_display, on);
}

void PreviewWidget::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	create();
}

void PreviewWidget::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	if (m_display) {
		const qreal dpr = devicePixelRatioF();
		obs_display_resize(m_display, uint32_t(std::max(1.0, width() * dpr)),
				   uint32_t(std::max(1.0, height() * dpr)));
	}
}

// ---- drawing (libobs graphics thread)
void PreviewWidget::draw(void *param, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<PreviewWidget *>(param);
	obs_video_info ovi{};
	if (!obs_get_video_info(&ovi) || !ovi.base_width || !ovi.base_height)
		return;
	const double scale = std::min(double(cx) / ovi.base_width, double(cy) / ovi.base_height);
	const int w = int(ovi.base_width * scale), h = int(ovi.base_height * scale);
	const int x = (int(cx) - w) / 2, y = (int(cy) - h) / 2;
	gs_viewport_push();
	gs_projection_push();
	gs_ortho(0.0f, float(ovi.base_width), 0.0f, float(ovi.base_height), -100.0f, 100.0f);
	gs_set_viewport(x, y, w, h);
	obs_render_main_texture_src_color_only();
	self->drawSelection(float(scale));
	gs_projection_pop();
	gs_viewport_pop();
}

void PreviewWidget::drawSelection(float scale)
{
	std::lock_guard<std::mutex> lock(m_lock);
	if (!m_selected || !obs_sceneitem_visible(m_selected))
		return;
	matrix4 m;
	obs_sceneitem_get_box_transform(m_selected, &m);
	vec3 c[4];
	const float unit[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
	for (int i = 0; i < 4; i++) {
		vec3_set(&c[i], unit[i][0], unit[i][1], 0.0f);
		vec3_transform(&c[i], &c[i], &m);
	}
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	vec4 accent;
	const bool locked = obs_sceneitem_locked(m_selected);
	vec4_set(&accent, locked ? 0.54f : 0.13f, locked ? 0.58f : 0.83f, locked ? 0.65f : 0.93f, 1.0f);
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
	gs_effect_set_vec4(color, &accent);
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	const float px = 1.0f / std::max(scale, 0.01f); // one screen pixel in canvas units
	for (float off : {0.0f, px}) {                  // two pixels thick
		gs_render_start(true);
		for (int i = 0; i <= 4; i++) {
			const vec3 &p = c[i % 4];
			const float dx = (i % 4 == 0 || i % 4 == 3) ? off : -off;
			const float dy = (i % 4 < 2) ? off : -off;
			gs_vertex2f(p.x + dx, p.y + dy);
		}
		gs_render_stop(GS_LINESTRIP);
	}
	if (!locked) {
		const float hs = 4.5f * px;
		for (const vec3 &p : c) {
			gs_render_start(true);
			gs_vertex2f(p.x - hs, p.y - hs);
			gs_vertex2f(p.x + hs, p.y - hs);
			gs_vertex2f(p.x - hs, p.y + hs);
			gs_vertex2f(p.x + hs, p.y + hs);
			gs_render_stop(GS_TRISTRIP);
		}
	}
	gs_technique_end_pass(tech);
	gs_technique_end(tech);
}

// ---- geometry (UI thread)
double PreviewWidget::scale() const
{
	const QSize canvas = m_core->canvas();
	if (canvas.isEmpty())
		return 1.0;
	return std::min(double(width()) / canvas.width(), double(height()) / canvas.height());
}

QPointF PreviewWidget::toCanvas(const QPointF &pos) const
{
	const QSize canvas = m_core->canvas();
	const double s = scale();
	const double x = (width() - canvas.width() * s) / 2, y = (height() - canvas.height() * s) / 2;
	return {(pos.x() - x) / s, (pos.y() - y) / s};
}

QRectF PreviewWidget::box(obs_sceneitem_t *item) const
{
	matrix4 m;
	obs_sceneitem_get_box_transform(item, &m);
	double minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
	const float unit[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
	for (const auto &u : unit) {
		vec3 p;
		vec3_set(&p, u[0], u[1], 0.0f);
		vec3_transform(&p, &p, &m);
		minX = std::min(minX, double(p.x));
		maxX = std::max(maxX, double(p.x));
		minY = std::min(minY, double(p.y));
		maxY = std::max(maxY, double(p.y));
	}
	return QRectF(QPointF(minX, minY), QPointF(maxX, maxY));
}

obs_sceneitem_t *PreviewWidget::itemAt(const QPointF &pos) const
{
	struct Hit {
		const PreviewWidget *self;
		QPointF pos;
		obs_sceneitem_t *found;
	} hit{this, pos, nullptr};
	if (!m_core->scene())
		return nullptr;
	obs_scene_enum_items( // bottom to top, so the last hit is the one in front
		m_core->scene(),
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *h = static_cast<Hit *>(param);
			if (obs_sceneitem_visible(item) && h->self->box(item).contains(h->pos))
				h->found = item;
			return true;
		},
		&hit);
	return hit.found;
}

int PreviewWidget::cornerAt(const QPointF &pos) const
{
	std::lock_guard<std::mutex> lock(m_lock);
	if (!m_selected || obs_sceneitem_locked(m_selected))
		return -1;
	const QRectF b = box(m_selected);
	const QPointF corners[4] = {b.topLeft(), b.topRight(), b.bottomRight(), b.bottomLeft()};
	const double reach = HANDLE_PX / scale();
	for (int i = 0; i < 4; i++)
		if (std::abs(pos.x() - corners[i].x()) <= reach && std::abs(pos.y() - corners[i].y()) <= reach)
			return i;
	return -1;
}

void PreviewWidget::setSelected(obs_sceneitem_t *item, bool announce)
{
	{
		std::lock_guard<std::mutex> lock(m_lock);
		if (item == m_selected)
			return;
		if (item)
			obs_sceneitem_addref(item);
		if (m_selected)
			obs_sceneitem_release(m_selected);
		m_selected = item;
	}
	if (announce)
		emit selected(selectedName());
}

void PreviewWidget::select(const QString &name)
{
	setSelected(name.isEmpty() ? nullptr : m_core->item(name), false);
}

QString PreviewWidget::selectedName() const
{
	std::lock_guard<std::mutex> lock(m_lock);
	return m_selected ? QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(m_selected))) : QString();
}

void PreviewWidget::normalize(obs_sceneitem_t *item)
{
	const QRectF b = box(item);
	vec2 v{};
	obs_sceneitem_set_rot(item, 0.0f);
	obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
	if (obs_sceneitem_get_bounds_type(item) == OBS_BOUNDS_NONE) {
		obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
		obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
		vec2_set(&v, 1.0f, 1.0f);
		obs_sceneitem_set_scale(item, &v);
	}
	vec2_set(&v, float(b.width()), float(b.height()));
	obs_sceneitem_set_bounds(item, &v);
	vec2_set(&v, float(b.x()), float(b.y()));
	obs_sceneitem_set_pos(item, &v);
}

// ---- mouse and keys
void PreviewWidget::mousePressEvent(QMouseEvent *event)
{
	const QPointF pos = toCanvas(event->position());
	m_pressCanvas = pos;
	m_drag = Drag::None;
	const int corner = event->button() == Qt::LeftButton ? cornerAt(pos) : -1;
	if (corner >= 0) {
		normalize(m_selected);
		m_drag = Drag::Resize;
		m_corner = corner;
		m_startBox = box(m_selected);
		return;
	}
	obs_sceneitem_t *hit = itemAt(pos);
	setSelected(hit, true);
	if (hit && event->button() == Qt::LeftButton && !obs_sceneitem_locked(hit)) {
		vec2 p{};
		obs_sceneitem_get_pos(hit, &p);
		m_startPos = QPointF(p.x, p.y);
		m_startBox = box(hit);
		m_drag = Drag::Move;
	}
	if (event->button() == Qt::RightButton)
		emit menuRequested(selectedName(), event->globalPosition().toPoint());
}

void PreviewWidget::moveBy(const QPointF &delta, bool snapping)
{
	if (!m_selected)
		return;
	const QSize canvas = m_core->canvas();
	QPointF d = delta;
	const QRectF moved = m_startBox.translated(d);
	const double reach = snapping ? SNAP_PX / scale() : -1.0; // Ctrl: no snapping
	auto snap = [reach](double a, double b, double c, double size, double half) {
		double best = reach + 1, shift = 0;
		for (double edge : {a, b, c})
			for (double target : {0.0, half, size}) {
				if (std::abs(target - edge) < best) {
					best = std::abs(target - edge);
					shift = target - edge;
				}
			}
		return best <= reach ? shift : 0.0;
	};
	d.rx() += snap(moved.left(), moved.center().x(), moved.right(), canvas.width(), canvas.width() / 2.0);
	d.ry() += snap(moved.top(), moved.center().y(), moved.bottom(), canvas.height(), canvas.height() / 2.0);
	vec2 p{};
	vec2_set(&p, float(m_startPos.x() + d.x()), float(m_startPos.y() + d.y()));
	obs_sceneitem_set_pos(m_selected, &p);
}

void PreviewWidget::mouseMoveEvent(QMouseEvent *event)
{
	const QPointF pos = toCanvas(event->position());
	if (m_drag == Drag::Move) {
		moveBy(pos - m_pressCanvas, !(event->modifiers() & Qt::ControlModifier));
		return;
	}
	if (m_drag == Drag::Resize && m_selected) {
		const QRectF b = m_startBox;
		const QPointF anchor = m_corner == 0 ? b.bottomRight() : m_corner == 1 ? b.bottomLeft()
					: m_corner == 2 ? b.topLeft() : b.topRight();
		double w = std::max(8.0, std::abs(pos.x() - anchor.x()));
		double h = std::max(8.0, std::abs(pos.y() - anchor.y()));
		const bool free = event->modifiers() & Qt::ShiftModifier;
		if (!free && b.width() > 0 && b.height() > 0) {
			const double f = std::max(w / b.width(), h / b.height());
			w = b.width() * f;
			h = b.height() * f;
		}
		const bool left = m_corner == 0 || m_corner == 3, top = m_corner == 0 || m_corner == 1;
		vec2 v{};
		vec2_set(&v, float(w), float(h));
		obs_sceneitem_set_bounds(m_selected, &v);
		if (free)
			obs_sceneitem_set_bounds_type(m_selected, OBS_BOUNDS_STRETCH);
		vec2_set(&v, float(left ? anchor.x() - w : anchor.x()), float(top ? anchor.y() - h : anchor.y()));
		obs_sceneitem_set_pos(m_selected, &v);
		return;
	}
	// hover: show what a drag would do
	const int corner = cornerAt(pos);
	if (corner == 0 || corner == 2)
		setCursor(Qt::SizeFDiagCursor);
	else if (corner == 1 || corner == 3)
		setCursor(Qt::SizeBDiagCursor);
	else if (obs_sceneitem_t *hit = itemAt(pos); hit && !obs_sceneitem_locked(hit))
		setCursor(Qt::SizeAllCursor);
	else
		unsetCursor();
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent *)
{
	if (m_drag != Drag::None) {
		m_drag = Drag::None;
		m_core->save();
		emit edited();
	}
}

void PreviewWidget::keyPressEvent(QKeyEvent *event)
{
	if (!m_selected || obs_sceneitem_locked(m_selected)) {
		QWidget::keyPressEvent(event);
		return;
	}
	const double step = event->modifiers() & Qt::ShiftModifier ? 10.0 : 1.0;
	QPointF d;
	switch (event->key()) {
	case Qt::Key_Left:
		d = {-step, 0};
		break;
	case Qt::Key_Right:
		d = {step, 0};
		break;
	case Qt::Key_Up:
		d = {0, -step};
		break;
	case Qt::Key_Down:
		d = {0, step};
		break;
	default:
		QWidget::keyPressEvent(event);
		return;
	}
	vec2 p{};
	obs_sceneitem_get_pos(m_selected, &p);
	p.x += float(d.x());
	p.y += float(d.y());
	obs_sceneitem_set_pos(m_selected, &p);
	m_core->save();
}
