#pragma once
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>
#include <mutex>

#include <obs.h>

#include "events/Events.h"

class QMovie;
class QTimer;

// Zunimi's own overlays, drawn natively instead of in a hidden web browser: alerts, chat, goal bars, labels,
// timers and recent events. Each is a "zunimi_overlay" source; the picture is painted on the UI thread only when
// it changes and handed to the GPU as one small texture, so an idle overlay costs nothing.
namespace overlays {

struct Shared {
	std::mutex lock;
	QImage pending;       // a new picture for the GPU
	bool changed = false;
	int width = 800, height = 450;
	gs_texture_t *texture = nullptr; // graphics thread only
	obs_source_t *source = nullptr;  // not a reference
	bool alive = true;
};

// What each alert type looks like and sounds like. Stored as JSON in the source's settings ("design").
struct AlertStyle {
	bool on = true;
	QString title;            // "{name} just followed!"  {name} {amount} {count} {message} {months}
	bool showMessage = true;
	QString image;            // picture or GIF; empty: a big emoji badge
	QString sound;
	int volume = 80;
	int seconds = 6;
	QString animation = "pop"; // pop, slide, fade, bounce
	QString layout = "above";  // above, side, text
	QString font = "Segoe UI";
	int size = 44;
	QString color = "#FFFFFF";
	QString accent = "#22D3EE";
	double min = 0;            // smaller tips/bits than this don't alert
	bool tts = false;
	double ttsMin = 0;
	struct Variant {
		double min = 0;
		QString title, image, sound;
	};
	QVector<Variant> variants; // bigger amounts can look different
	static AlertStyle fromJson(const QJsonObject &o, const QString &type);
	QJsonObject toJson() const;
};

QStringList alertTypes();                   // follow, sub, resub, gift, bits, raid, tip, superchat, member
QString alertTypeLabel(const QString &type);
QJsonObject defaultDesign(const QString &theme = "neon");
QStringList themes();                       // neon, clean, gold, minimal
QString themeLabel(const QString &theme);
// The alert drawn at a moment: progress 0..1 through its time on screen (in, hold, out).
QImage paintAlert(const AlertStyle &style, const StreamEvent &e, QSize size, double progress, const QImage &picture);

// Speech (Windows voices) and alert sounds go out on their own output channels, so they're in the stream and,
// if you want, in your headphones.
QStringList voices();
void playSound(const QString &file, int volume);
void speak(const QString &text, const QString &voice, int volume);

class Engine : public QObject {
	Q_OBJECT
public:
	static Engine &get();
	void registerSource();        // once, after libobs has started
	void shutdown();              // before libobs shuts down
	void add(const std::shared_ptr<Shared> &shared, obs_data_t *settings);
	void update(Shared *shared, obs_data_t *settings);
	void remove(Shared *shared);
	// The builder opens from a source's Options; the main window sets this.
	std::function<void(obs_source_t *)> openDesigner;
	void replay(const StreamEvent &e); // show an event again in every alert box

private:
	struct Instance;
	Engine();
	void tick();
	void paint(Instance &in, qint64 now);
	void onEvent(const StreamEvent &e);
	void onChat(const ChatMessage &m);
	QVector<std::shared_ptr<Instance>> m_instances;
	QTimer *m_timer;
	obs_source_t *m_sound = nullptr, *m_voice = nullptr;
	friend void playSound(const QString &, int);
	friend void speak(const QString &, const QString &, int);
};

} // namespace overlays
