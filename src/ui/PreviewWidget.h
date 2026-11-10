#pragma once
#include <QPointF>
#include <QRectF>
#include <QWidget>

#include <mutex>

#include <obs.h>

struct obs_display;
class ObsCore;

// The live picture. libobs draws straight into this native window on the GPU; Qt never paints it.
// Pausing it (hidden, minimized, or "pause preview while live" on Potato) saves GPU time on weak PCs.
// Sources can be edited right here: click to select, drag to move (snaps to the edges and the middle; hold Ctrl to place freely),
// drag a corner to resize (Shift: stretch freely), arrow keys to nudge, right-click for more.
class PreviewWidget : public QWidget {
	Q_OBJECT
public:
	explicit PreviewWidget(ObsCore *core, QWidget *parent = nullptr);
	~PreviewWidget() override;

	void setRunning(bool on);
	bool running() const { return m_running; }
	void select(const QString &name); // empty clears
	QString selectedName() const;
	QPaintEngine *paintEngine() const override { return nullptr; }

signals:
	void selected(const QString &name);
	void menuRequested(const QString &name, const QPoint &global);
	void edited(); // a source was moved or resized

protected:
	void showEvent(QShowEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void paintEvent(QPaintEvent *) override {}
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;

private:
	enum class Drag { None, Move, Resize };
	void create();
	static void draw(void *param, uint32_t cx, uint32_t cy);
	void drawSelection(float scale);
	double scale() const;                  // screen pixels per canvas pixel
	QPointF toCanvas(const QPointF &widgetPos) const;
	QRectF box(obs_sceneitem_t *item) const; // canvas rectangle the item covers
	obs_sceneitem_t *itemAt(const QPointF &canvasPos) const;
	int cornerAt(const QPointF &canvasPos) const; // 0 TL, 1 TR, 2 BR, 3 BL, -1 none
	void setSelected(obs_sceneitem_t *item, bool announce);
	void normalize(obs_sceneitem_t *item); // top-left aligned with a bounds box, so resizing is simple
	void moveBy(const QPointF &delta, bool snapping);

	ObsCore *m_core;
	obs_display *m_display = nullptr;
	bool m_running = true;
	mutable std::mutex m_lock;          // m_selected is read on the graphics thread
	obs_sceneitem_t *m_selected = nullptr; // holds a reference
	Drag m_drag = Drag::None;
	int m_corner = -1;
	QPointF m_pressCanvas;
	QPointF m_startPos;
	QRectF m_startBox;
};
