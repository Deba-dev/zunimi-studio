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
// Editor: click to select, drag to move (snaps to the edges and the middle; Ctrl places freely), drag a corner to
// resize (Shift stretches), Alt+drag a corner to crop, arrow keys nudge, right-click for more. Every edit can be undone.
// Program: what is on air, read-only (studio mode's right side, and projectors).
class PreviewWidget : public QWidget {
	Q_OBJECT
public:
	enum class Mode { Editor, Program, Source };
	explicit PreviewWidget(ObsCore *core, Mode mode = Mode::Editor, QWidget *parent = nullptr);
	~PreviewWidget() override;

	void setRunning(bool on);
	bool running() const { return m_running; }
	void setSource(obs_source_t *source); // Mode::Source: what to show (holds a reference)
	void select(const QString &name);     // empty clears
	QString selectedName() const;
	QPaintEngine *paintEngine() const override { return nullptr; }

signals:
	void selected(const QString &name);
	void menuRequested(const QString &name, const QPoint &global);
	void edited(); // a source was moved, resized or cropped

protected:
	void showEvent(QShowEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void paintEvent(QPaintEvent *) override {}
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;
	void keyReleaseEvent(QKeyEvent *event) override;

private:
	enum class Drag { None, Move, Resize, Crop };
	void create();
	static void draw(void *param, uint32_t cx, uint32_t cy);
	void drawSelection(float scale);
	QSize sourceSize() const;
	double scale() const;                  // screen pixels per canvas pixel
	QPointF toCanvas(const QPointF &widgetPos) const;
	obs_sceneitem_t *itemAt(const QPointF &canvasPos) const;
	int cornerAt(const QPointF &canvasPos) const; // 0 TL, 1 TR, 2 BR, 3 BL, -1 none
	void setSelected(obs_sceneitem_t *item, bool announce);
	void placeBox(obs_sceneitem_t *item, const QRectF &box, bool stretch); // exactly this box, keeping flips
	void moveBy(const QPointF &delta, bool snapping);

	ObsCore *m_core;
	Mode m_mode;
	obs_display *m_display = nullptr;
	bool m_running = true;
	mutable std::mutex m_lock;             // read on the graphics thread
	obs_sceneitem_t *m_selected = nullptr; // holds a reference
	obs_source_t *m_source = nullptr;      // Mode::Source, and the edited scene in studio mode (holds a reference)
	Drag m_drag = Drag::None;
	int m_corner = -1;
	bool m_nudging = false;
	QPointF m_pressCanvas;
	QPointF m_startPos;
	QRectF m_startBox;
	obs_sceneitem_crop m_startCrop{};
};
