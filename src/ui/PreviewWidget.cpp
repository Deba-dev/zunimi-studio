#include "ui/PreviewWidget.h"

#include <QResizeEvent>
#include <QWindow>

#include <obs.h>

#include <algorithm>

PreviewWidget::PreviewWidget(QWidget *parent) : QWidget(parent)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_NativeWindow);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setMinimumSize(320, 180);
}

PreviewWidget::~PreviewWidget()
{
	if (m_display) {
		obs_display_remove_draw_callback(m_display, draw, this);
		obs_display_destroy(m_display);
	}
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
	m_display = obs_display_create(&info, 0x0B0D12);
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

// Runs on the libobs graphics thread: fits the canvas inside the widget, letterboxed.
void PreviewWidget::draw(void *, uint32_t cx, uint32_t cy)
{
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
	gs_projection_pop();
	gs_viewport_pop();
}
