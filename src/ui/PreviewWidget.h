#pragma once
#include <QWidget>

struct obs_display;

// The live picture. libobs draws straight into this native window on the GPU; Qt never paints it.
// Pausing it (hidden, minimized, or "pause preview while live" on Potato) saves GPU time on weak PCs.
class PreviewWidget : public QWidget {
	Q_OBJECT
public:
	explicit PreviewWidget(QWidget *parent = nullptr);
	~PreviewWidget() override;

	void setRunning(bool on);
	bool running() const { return m_running; }
	QPaintEngine *paintEngine() const override { return nullptr; }

protected:
	void showEvent(QShowEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void paintEvent(QPaintEvent *) override {}

private:
	void create();
	static void draw(void *param, uint32_t cx, uint32_t cy);

	obs_display *m_display = nullptr;
	bool m_running = true;
};
