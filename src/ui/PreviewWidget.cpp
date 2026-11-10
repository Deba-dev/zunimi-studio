#include "ui/PreviewWidget.h"
#include "core/I18n.h"

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

bool upright(obs_sceneitem_t *item)
{
	const float r = std::fmod(std::fabs(obs_sceneitem_get_rot(item)), 360.0f);
	return r < 0.01f || r > 359.99f;
}
} // namespace

PreviewWidget::PreviewWidget(ObsCore *core, Mode mode, QWidget *parent) : QWidget(parent), m_core(core), m_mode(mode)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_NativeWindow);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setMinimumSize(mode == Mode::Editor ? 320 : 160, mode == Mode::Editor ? 180 : 90);
	setMouseTracking(mode == Mode::Editor);
	setFocusPolicy(mode == Mode::Editor ? Qt::ClickFocus : Qt::NoFocus);
}

PreviewWidget::~PreviewWidget()
{
	if (m_display) {
		obs_display_remove_draw_callback(m_display, draw, this);
		obs_display_destroy(m_display);
	}
	setSelected(nullptr, false);
	setSource(nullptr);
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
	m_display = obs_display_create(&info, m_mode == Mode::Editor ? 0x120D0B : 0x000000); // 0xBBGGRR
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

void PreviewWidget::setSource(obs_source_t *source)
{
	std::lock_guard<std::mutex> lock(m_lock);
	if (source == m_source)
		return;
	if (source)
		obs_source_get_ref(source);
	if (m_source)
		obs_source_release(m_source);
	m_source = source;
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
	std::lock_guard<std::mutex> lock(self->m_lock);
	uint32_t bw = 0, bh = 0;
	if (self->m_source && self->m_mode != Mode::Program) { // one source, studio mode's scene, or the vertical scene
		bw = obs_source_get_width(self->m_source);
		bh = obs_source_get_height(self->m_source);
	} else {
		obs_video_info ovi{};
		if (obs_get_video_info(&ovi)) {
			bw = ovi.base_width;
			bh = ovi.base_height;
		}
	}
	if (!bw || !bh)
		return;
	const double scale = std::min(double(cx) / bw, double(cy) / bh);
	const int w = int(bw * scale), h = int(bh * scale);
	const int x = (int(cx) - w) / 2, y = (int(cy) - h) / 2;
	gs_viewport_push();
	gs_projection_push();
	gs_ortho(0.0f, float(bw), 0.0f, float(bh), -100.0f, 100.0f);
	gs_set_viewport(x, y, w, h);
	if (self->m_source && self->m_mode != Mode::Program)
		obs_source_video_render(self->m_source); // studio mode's edited scene, or one source
	else
		obs_render_main_texture_src_color_only();
	if (self->m_mode == Mode::Editor)
		self->drawSelection(float(scale));
	gs_projection_pop();
	gs_viewport_pop();
}

void PreviewWidget::drawSelection(float scale)
{
	// m_lock is held by draw()
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
	const bool crop = m_drag == Drag::Crop;
	if (locked)
		vec4_set(&accent, 0.54f, 0.58f, 0.65f, 1.0f);
	else if (crop)
		vec4_set(&accent, 1.0f, 0.72f, 0.2f, 1.0f); // amber while cropping
	else
		vec4_set(&accent, 0.13f, 0.83f, 0.93f, 1.0f);
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
	gs_effect_set_vec4(color, &accent);
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	const float px = 1.0f / std::max(scale, 0.01f); // one screen pixel in canvas units
	for (float off : {0.0f, px}) {                  // two pixels thick
		gs_render_start(true);
		for (int i = 0; i <= 4; i++) {
			const vec3 &p = c[i % 4];
			gs_vertex2f(p.x + (i % 4 == 0 || i % 4 == 3 ? off : -off), p.y + (i % 4 < 2 ? off : -off));
		}
		gs_render_stop(GS_LINESTRIP);
	}
	if (!locked && upright(m_selected)) {
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
QSize PreviewWidget::sourceSize() const
{
	return m_core->canvas();
}

double PreviewWidget::scale() const
{
	const QSize canvas = sourceSize();
	if (canvas.isEmpty())
		return 1.0;
	return std::min(double(width()) / canvas.width(), double(height()) / canvas.height());
}

QPointF PreviewWidget::toCanvas(const QPointF &pos) const
{
	const QSize canvas = sourceSize();
	const double s = scale();
	const double x = (width() - canvas.width() * s) / 2, y = (height() - canvas.height() * s) / 2;
	return {(pos.x() - x) / s, (pos.y() - y) / s};
}

obs_sceneitem_t *PreviewWidget::itemAt(const QPointF &pos) const
{
	struct Hit {
		const ObsCore *core;
		QPointF pos;
		obs_sceneitem_t *found;
	} hit{m_core, pos, nullptr};
	if (!m_core->scene())
		return nullptr;
	obs_scene_enum_items( // bottom to top, so the last hit is the one in front
		m_core->scene(),
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *h = static_cast<Hit *>(param);
			if (obs_sceneitem_visible(item) && h->core->itemBox(item).contains(h->pos))
				h->found = item;
			return true;
		},
		&hit);
	return hit.found;
}

int PreviewWidget::cornerAt(const QPointF &pos) const
{
	std::lock_guard<std::mutex> lock(m_lock);
	if (!m_selected || obs_sceneitem_locked(m_selected) || !upright(m_selected))
		return -1; // a rotated source is sized in Edit size and place
	const QRectF b = m_core->itemBox(m_selected);
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

void PreviewWidget::placeBox(obs_sceneitem_t *item, const QRectF &box, bool stretch)
{
	vec2 v{};
	obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
	if (obs_sceneitem_get_bounds_type(item) == OBS_BOUNDS_NONE) {
		obs_sceneitem_get_scale(item, &v);
		vec2_set(&v, v.x < 0 ? -1.0f : 1.0f, v.y < 0 ? -1.0f : 1.0f); // keep a flip
		obs_sceneitem_set_scale(item, &v);
		obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
		obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	}
	if (stretch)
		obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_STRETCH);
	vec2_set(&v, float(box.width()), float(box.height()));
	obs_sceneitem_set_bounds(item, &v);
	vec2_set(&v, float(box.x()), float(box.y()));
	obs_sceneitem_set_pos(item, &v);
	// a flipped source extends the other way from its position: shift it so the box lands exactly
	const QRectF now = m_core->itemBox(item);
	vec2_set(&v, float(box.x() + box.x() - now.x()), float(box.y() + box.y() - now.y()));
	obs_sceneitem_set_pos(item, &v);
}

// ---- mouse and keys
void PreviewWidget::mousePressEvent(QMouseEvent *event)
{
	if (m_mode != Mode::Editor)
		return;
	const QPointF pos = toCanvas(event->position());
	m_pressCanvas = pos;
	m_drag = Drag::None;
	const int corner = event->button() == Qt::LeftButton ? cornerAt(pos) : -1;
	if (corner >= 0) {
		const bool crop = event->modifiers() & Qt::AltModifier;
		m_core->beginEdit(crop ? L("Crop") : L("Resize"));
		m_drag = crop ? Drag::Crop : Drag::Resize;
		m_corner = corner;
		m_startBox = m_core->itemBox(m_selected);
		obs_sceneitem_get_crop(m_selected, &m_startCrop);
		return;
	}
	obs_sceneitem_t *hit = itemAt(pos);
	setSelected(hit, true);
	if (hit && event->button() == Qt::LeftButton && !obs_sceneitem_locked(hit)) {
		m_core->beginEdit(L("Move"));
		vec2 p{};
		obs_sceneitem_get_pos(hit, &p);
		m_startPos = QPointF(p.x, p.y);
		m_startBox = m_core->itemBox(hit);
		m_drag = Drag::Move;
	}
	if (event->button() == Qt::RightButton)
		emit menuRequested(selectedName(), event->globalPosition().toPoint());
}

void PreviewWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
	if (m_mode == Mode::Editor && event->button() == Qt::LeftButton && !selectedName().isEmpty())
		emit menuRequested("options:" + selectedName(), event->globalPosition().toPoint());
}

void PreviewWidget::moveBy(const QPointF &delta, bool snapping)
{
	if (!m_selected)
		return;
	const QSize canvas = sourceSize();
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
	if (m_mode != Mode::Editor)
		return;
	const QPointF pos = toCanvas(event->position());
	if (m_drag == Drag::Move) {
		moveBy(pos - m_pressCanvas, !(event->modifiers() & Qt::ControlModifier));
		return;
	}
	const bool left = m_corner == 0 || m_corner == 3, top = m_corner == 0 || m_corner == 1;
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
		placeBox(m_selected, QRectF(left ? anchor.x() - w : anchor.x(), top ? anchor.y() - h : anchor.y(), w, h), free);
		return;
	}
	if (m_drag == Drag::Crop && m_selected) {
		// Alt+drag: cut away the edge of the picture instead of shrinking it.
		obs_source_t *src = obs_sceneitem_get_source(m_selected);
		const double sw = obs_source_get_width(src), sh = obs_source_get_height(src);
		const obs_sceneitem_crop c0 = m_startCrop;
		const double visW = sw - c0.left - c0.right, visH = sh - c0.top - c0.bottom;
		if (visW <= 0 || visH <= 0)
			return;
		const double fx = m_startBox.width() / visW, fy = m_startBox.height() / visH;
		obs_sceneitem_crop c = c0;
		QRectF box = m_startBox;
		if (left) {
			c.left = int(std::clamp(c0.left + (pos.x() - m_startBox.left()) / fx, 0.0, sw - c0.right - 4));
			box.setLeft(m_startBox.left() + (c.left - c0.left) * fx);
		} else {
			c.right = int(std::clamp(c0.right + (m_startBox.right() - pos.x()) / fx, 0.0, sw - c0.left - 4));
			box.setRight(m_startBox.right() - (c.right - c0.right) * fx);
		}
		if (top) {
			c.top = int(std::clamp(c0.top + (pos.y() - m_startBox.top()) / fy, 0.0, sh - c0.bottom - 4));
			box.setTop(m_startBox.top() + (c.top - c0.top) * fy);
		} else {
			c.bottom = int(std::clamp(c0.bottom + (m_startBox.bottom() - pos.y()) / fy, 0.0, sh - c0.top - 4));
			box.setBottom(m_startBox.bottom() - (c.bottom - c0.bottom) * fy);
		}
		obs_sceneitem_set_crop(m_selected, &c);
		placeBox(m_selected, box, obs_sceneitem_get_bounds_type(m_selected) == OBS_BOUNDS_STRETCH);
		return;
	}
	// hover: show what a drag would do
	const int corner = cornerAt(pos);
	if (corner >= 0 && (event->modifiers() & Qt::AltModifier))
		setCursor(Qt::CrossCursor);
	else if (corner == 0 || corner == 2)
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
		m_core->endEdit();
		emit edited();
	}
}

void PreviewWidget::keyPressEvent(QKeyEvent *event)
{
	if (m_mode != Mode::Editor || !m_selected || obs_sceneitem_locked(m_selected)) {
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
	if (!m_nudging) { // one undo step for a whole run of nudges
		m_core->beginEdit(L("Nudge"));
		m_nudging = true;
	}
	vec2 p{};
	obs_sceneitem_get_pos(m_selected, &p);
	p.x += float(d.x());
	p.y += float(d.y());
	obs_sceneitem_set_pos(m_selected, &p);
}

void PreviewWidget::keyReleaseEvent(QKeyEvent *event)
{
	if (m_nudging && !event->isAutoRepeat()) {
		m_nudging = false;
		m_core->endEdit();
	}
	QWidget::keyReleaseEvent(event);
}
