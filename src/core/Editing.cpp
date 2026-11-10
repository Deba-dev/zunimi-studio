// Editing items in a scene, and undo/redo for every change made to scenes and their items.
#include "core/ObsCore.h"
#include "core/I18n.h"

#include <QPointF>

#include <graphics/matrix4.h>
#include <graphics/vec3.h>

#include <algorithm>
#include <cmath>

namespace {
const int UNDO_LIMIT = 100;

QString nameOf(obs_source_t *source)
{
	return QString::fromUtf8(obs_source_get_name(source));
}
} // namespace

QRectF ObsCore::itemBox(obs_sceneitem_t *item) const
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

ItemState ObsCore::itemState(obs_sceneitem_t *item) const
{
	ItemState s;
	s.name = nameOf(obs_sceneitem_get_source(item));
	obs_sceneitem_get_info2(item, &s.info);
	obs_sceneitem_get_crop(item, &s.crop);
	s.visible = obs_sceneitem_visible(item);
	s.locked = obs_sceneitem_locked(item);
	s.blend = obs_sceneitem_get_blending_mode(item);
	s.filter = obs_sceneitem_get_scale_filter(item);
	return s;
}

void ObsCore::setItemState(obs_sceneitem_t *item, const ItemState &s)
{
	obs_sceneitem_defer_update_begin(item);
	obs_sceneitem_set_info2(item, &s.info);
	obs_sceneitem_set_crop(item, &s.crop);
	obs_sceneitem_set_visible(item, s.visible);
	obs_sceneitem_set_locked(item, s.locked);
	obs_sceneitem_set_blending_mode(item, s.blend);
	obs_sceneitem_set_scale_filter(item, s.filter);
	obs_sceneitem_defer_update_end(item);
}

QVector<ItemState> ObsCore::snapshot(obs_scene_t *scene) const
{
	struct Ctx {
		const ObsCore *self;
		QVector<ItemState> list;
	} ctx{this, {}};
	if (scene)
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *it, void *param) {
				auto *c = static_cast<Ctx *>(param);
				c->list << c->self->itemState(it);
				return true;
			},
			&ctx);
	return ctx.list;
}

void ObsCore::restore(const QString &sceneName, const QVector<ItemState> &states)
{
	obs_source_t *source = sceneSource(sceneName);
	if (!source)
		return;
	if (source != m_sceneSource)
		setCurrent(source, !m_studio);
	obs_scene_t *scene = obs_scene_from_source(source);
	QVector<obs_sceneitem_t *> order;
	for (const ItemState &s : states) {
		obs_sceneitem_t *it = obs_scene_find_source(scene, s.name.toUtf8().constData());
		if (!it)
			continue;
		setItemState(it, s);
		order << it;
	}
	// put them back in the same stacking order (items added since keep their place on top)
	if (order.size() == int(snapshot(scene).size()))
		obs_scene_reorder_items(scene, order.data(), size_t(order.size()));
	save();
	emit sceneChanged();
}

void ObsCore::push(const QString &label, std::function<void()> undo, std::function<void()> redo)
{
	if (m_replaying || !undo)
		return;
	m_undo.append({label, std::move(undo), std::move(redo)});
	if (m_undo.size() > UNDO_LIMIT)
		m_undo.removeFirst();
	m_redo.clear();
	emit undoChanged();
}

void ObsCore::beginEdit(const QString &label)
{
	if (m_editDepth++ > 0)
		return;
	m_editLabel = label;
	m_editScene = currentScene();
	m_editBefore = snapshot(m_scene);
}

void ObsCore::endEdit()
{
	if (m_editDepth == 0 || --m_editDepth > 0)
		return;
	if (m_editScene != currentScene())
		return;
	const QVector<ItemState> after = snapshot(m_scene);
	save();
	if (after == m_editBefore)
		return;
	const QString scene = m_editScene;
	const QVector<ItemState> before = m_editBefore;
	push(m_editLabel, [this, scene, before] { restore(scene, before); },
	     [this, scene, after] { restore(scene, after); });
}

void ObsCore::editItem(const QString &name, const QString &label, const std::function<void(obs_sceneitem_t *)> &change)
{
	obs_sceneitem_t *it = item(name);
	if (!it)
		return;
	beginEdit(label);
	change(it);
	endEdit();
}

QString ObsCore::undoLabel() const
{
	return m_undo.isEmpty() ? QString() : m_undo.last().label;
}

QString ObsCore::redoLabel() const
{
	return m_redo.isEmpty() ? QString() : m_redo.last().label;
}

void ObsCore::undo()
{
	if (m_undo.isEmpty())
		return;
	Action a = m_undo.takeLast();
	m_replaying = true;
	a.undo();
	m_replaying = false;
	if (a.redo)
		m_redo.append(a);
	emit undoChanged();
	emit notice(L("Undid: ") + a.label);
}

void ObsCore::redo()
{
	if (m_redo.isEmpty())
		return;
	Action a = m_redo.takeLast();
	m_replaying = true;
	a.redo();
	m_replaying = false;
	m_undo.append(a);
	emit undoChanged();
	emit notice(L("Redid: ") + a.label);
}

// ---- transforms
void ObsCore::keepCentre(obs_sceneitem_t *item, const QPointF &centre)
{
	const QPointF now = itemBox(item).center();
	vec2 p{};
	obs_sceneitem_get_pos(item, &p);
	p.x += float(centre.x() - now.x());
	p.y += float(centre.y() - now.y());
	obs_sceneitem_set_pos(item, &p);
}

void ObsCore::rotate(const QString &name, float degrees)
{
	editItem(name, L("Rotate"), [&](obs_sceneitem_t *it) {
		const QPointF centre = itemBox(it).center();
		float rot = std::fmod(obs_sceneitem_get_rot(it) + degrees, 360.0f);
		if (rot < 0)
			rot += 360.0f;
		obs_sceneitem_set_rot(it, rot);
		keepCentre(it, centre);
	});
}

void ObsCore::flip(const QString &name, bool horizontal)
{
	editItem(name, horizontal ? L("Flip") : L("Flip upside down"), [&](obs_sceneitem_t *it) {
		const QPointF centre = itemBox(it).center();
		vec2 s{};
		obs_sceneitem_get_scale(it, &s);
		if (horizontal)
			s.x = -s.x;
		else
			s.y = -s.y;
		obs_sceneitem_set_scale(it, &s);
		keepCentre(it, centre);
	});
}

void ObsCore::setCrop(const QString &name, const obs_sceneitem_crop &crop)
{
	editItem(name, L("Crop"), [&](obs_sceneitem_t *it) { obs_sceneitem_set_crop(it, &crop); });
}

void ObsCore::setBlend(const QString &name, obs_blending_type type)
{
	editItem(name, L("Blend mode"), [type](obs_sceneitem_t *it) { obs_sceneitem_set_blending_mode(it, type); });
}

void ObsCore::setScaleFilter(const QString &name, obs_scale_type type)
{
	editItem(name, L("Scaling"), [type](obs_sceneitem_t *it) { obs_sceneitem_set_scale_filter(it, type); });
}

void ObsCore::setShowHide(const QString &name, const QString &transitionId, int ms)
{
	obs_sceneitem_t *it = item(name);
	if (!it)
		return;
	for (bool show : {true, false}) {
		obs_source_t *t = nullptr;
		if (!transitionId.isEmpty()) {
			obs_data_t *s = obs_data_create();
			if (transitionId == "slide_transition" || transitionId == "swipe_transition")
				obs_data_set_string(s, "direction", show ? "left" : "right");
			t = obs_source_create_private(transitionId.toUtf8().constData(),
						      show ? "Show effect" : "Hide effect", s);
			obs_data_release(s);
		}
		obs_sceneitem_set_transition(it, show, t);
		obs_sceneitem_set_transition_duration(it, show, uint32_t(std::clamp(ms, 50, 5000)));
		obs_source_release(t);
	}
	save();
}

QString ObsCore::showHide(const QString &name) const
{
	obs_sceneitem_t *it = item(name);
	obs_source_t *t = it ? obs_sceneitem_get_transition(it, true) : nullptr;
	return t ? QString::fromUtf8(obs_source_get_id(t)) : QString();
}

void ObsCore::resetTransform(const QString &name)
{
	editItem(name, L("Reset size"), [this](obs_sceneitem_t *it) {
		obs_sceneitem_crop none{};
		obs_sceneitem_set_crop(it, &none);
		place(it, {0, 0, double(m_plan.width), double(m_plan.height)});
		obs_sceneitem_set_bounds_type(it, OBS_BOUNDS_NONE);
		obs_sceneitem_set_alignment(it, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
		vec2 v{};
		obs_sceneitem_set_pos(it, &v);
	});
}

void ObsCore::copyTransform(const QString &name)
{
	if (obs_sceneitem_t *it = item(name))
		m_copied = itemState(it);
}

bool ObsCore::pasteTransform(const QString &name)
{
	obs_sceneitem_t *it = item(name);
	if (!it || !m_copied)
		return false;
	editItem(name, L("Paste size and place"), [this](obs_sceneitem_t *target) {
		ItemState s = itemState(target);
		s.info = m_copied->info;
		s.crop = m_copied->crop;
		setItemState(target, s);
	});
	return true;
}
