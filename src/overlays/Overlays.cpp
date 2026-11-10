#include "overlays/Overlays.h"

#include "core/Settings.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMovie>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextDocument>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <deque>
#include <optional>
#include <thread>

#include <windows.h>
#include <sapi.h>

namespace overlays {

namespace {
const char *ID = "zunimi_overlay";
const QStringList BOTS = {"nightbot", "streamelements", "streamlabs", "moobot", "fossabot", "wizebot", "soundalerts", "sery_bot", "botrixoficial", "kicklet"};

QString emojiFor(const QString &type)
{
	static const QHash<QString, QString> e = {{"follow", "❤️"}, {"sub", "⭐"},       {"resub", "🔁"},
						  {"gift", "🎁"},   {"bits", "💎"},      {"raid", "🚀"},
						  {"tip", "💰"},    {"superchat", "💬"}, {"member", "👑"}};
	return e.value(type, "✨");
}

QColor platformColor(const QString &p)
{
	if (p == "twitch")
		return QColor("#9146FF");
	if (p == "youtube")
		return QColor("#FF3B3B");
	if (p == "kick")
		return QColor("#53FC18");
	return QColor("#22D3EE");
}

QString platformTag(const QString &p)
{
	return p == "twitch" ? "TW" : p == "youtube" ? "YT" : p == "kick" ? "K" : "•";
}

// a stable, readable colour for names that don't bring their own
QColor nameColor(const QString &name)
{
	static const QColor palette[] = {QColor("#FF7A90"), QColor("#7CC4FF"), QColor("#9BE38A"), QColor("#FFD166"),
					 QColor("#C3A6FF"), QColor("#5EEAD4"), QColor("#FFA45C")};
	const QByteArray h = QCryptographicHash::hash(name.toLower().toUtf8(), QCryptographicHash::Md5);
	return palette[uchar(h[0]) % 7];
}

double easeOutBack(double t)
{
	const double c1 = 1.70158, c3 = c1 + 1;
	return 1 + c3 * std::pow(t - 1, 3) + c1 * std::pow(t - 1, 2);
}

double easeOut(double t)
{
	return 1 - std::pow(1 - t, 3);
}

QString fill(QString text, const StreamEvent &e)
{
	text.replace("{name}", e.user.toHtmlEscaped());
	text.replace("{amount}", e.amountText().toHtmlEscaped());
	text.replace("{count}", QString::number(e.count));
	text.replace("{months}", QString::number(std::max(1, e.count)));
	text.replace("{message}", e.message.toHtmlEscaped());
	return text;
}
} // namespace

// ---- styles
QStringList alertTypes()
{
	return {"follow", "sub", "resub", "gift", "bits", "raid", "tip", "superchat", "member"};
}

QString alertTypeLabel(const QString &type)
{
	static const QHash<QString, QString> n = {{"follow", "Follow"},         {"sub", "New sub"},
						  {"resub", "Resub"},           {"gift", "Gift subs"},
						  {"bits", "Bits (cheer)"},     {"raid", "Raid"},
						  {"tip", "Tip / donation"},    {"superchat", "Super Chat"},
						  {"member", "YouTube member"}};
	return n.value(type, type);
}

QStringList themes()
{
	return {"neon", "clean", "gold", "minimal"};
}

QString themeLabel(const QString &theme)
{
	static const QHash<QString, QString> n = {{"neon", "Zunimi Neon"}, {"clean", "Clean card"}, {"gold", "Desi Gold"},
						  {"minimal", "Minimal (text only)"}};
	return n.value(theme, theme);
}

AlertStyle AlertStyle::fromJson(const QJsonObject &o, const QString &type)
{
	AlertStyle s;
	const AlertStyle d = [&type] {
		AlertStyle base;
		const QJsonObject def = defaultDesign().value(type).toObject();
		base.title = def.value("title").toString();
		return base;
	}();
	s.on = o.value("on").toBool(true);
	s.title = o.value("title").toString(d.title);
	s.showMessage = o.value("message").toBool(true);
	s.image = o.value("image").toString();
	s.sound = o.value("sound").toString();
	s.volume = o.value("volume").toInt(80);
	s.seconds = std::clamp(o.value("seconds").toInt(6), 2, 60);
	s.animation = o.value("animation").toString("pop");
	s.layout = o.value("layout").toString("above");
	s.font = o.value("font").toString("Segoe UI");
	s.size = std::clamp(o.value("size").toInt(44), 12, 160);
	s.color = o.value("color").toString("#FFFFFF");
	s.accent = o.value("accent").toString("#22D3EE");
	s.min = o.value("min").toDouble(0);
	s.tts = o.value("tts").toBool(false);
	s.ttsMin = o.value("ttsMin").toDouble(0);
	for (const QJsonValue &v : o.value("variants").toArray()) {
		const QJsonObject vo = v.toObject();
		s.variants.append({vo.value("min").toDouble(), vo.value("title").toString(), vo.value("image").toString(),
				   vo.value("sound").toString()});
	}
	return s;
}

QJsonObject AlertStyle::toJson() const
{
	QJsonArray vars;
	for (const Variant &v : variants)
		vars.append(QJsonObject{{"min", v.min}, {"title", v.title}, {"image", v.image}, {"sound", v.sound}});
	return {{"on", on},         {"title", title},   {"message", showMessage}, {"image", image},     {"sound", sound},
		{"volume", volume}, {"seconds", seconds}, {"animation", animation}, {"layout", layout}, {"font", font},
		{"size", size},     {"color", color},   {"accent", accent},       {"min", min},         {"tts", tts},
		{"ttsMin", ttsMin}, {"variants", vars}};
}

QJsonObject defaultDesign(const QString &theme)
{
	static const QHash<QString, QString> titles = {
		{"follow", "{name} just followed!"},
		{"sub", "{name} subscribed!"},
		{"resub", "{name} resubscribed for {months} months!"},
		{"gift", "{name} gifted {count} subs!"},
		{"bits", "{name} cheered {amount}!"},
		{"raid", "{name} is raiding with {count} viewers!"},
		{"tip", "{name} tipped {amount}!"},
		{"superchat", "{name} sent a Super Chat: {amount}"},
		{"member", "{name} became a member!"}};
	QString color = "#FFFFFF", accent = "#22D3EE", font = "Segoe UI", animation = "pop";
	if (theme == "clean") {
		color = "#111827";
		accent = "#6366F1";
		animation = "slide";
	} else if (theme == "gold") {
		color = "#FFF6E0";
		accent = "#F5B83D";
		font = "Georgia";
		animation = "bounce";
	} else if (theme == "minimal") {
		accent = "#FFFFFF";
		animation = "fade";
	}
	QJsonObject design{{"theme", theme}};
	for (const QString &type : alertTypes()) {
		const bool money = type == "tip" || type == "superchat";
		design.insert(type, QJsonObject{{"on", true},
						{"title", titles.value(type)},
						{"message", true},
						{"seconds", money || type == "raid" ? 8 : 6},
						{"animation", animation},
						{"layout", "above"},
						{"font", font},
						{"size", 44},
						{"color", color},
						{"accent", accent},
						{"volume", 80},
						{"tts", money},
						{"ttsMin", 0}});
	}
	return design;
}

QImage paintAlert(const AlertStyle &st, const StreamEvent &e, QSize size, double p, const QImage &picture)
{
	QImage img(size, QImage::Format_RGBA8888);
	img.fill(Qt::transparent);
	if (p <= 0.0 || p >= 1.0)
		return img;
	const double total = st.seconds, inT = std::min(0.7, total * 0.15), outT = std::min(0.6, total * 0.12);
	const double t = p * total;
	double opacity = 1.0, scale = 1.0, dy = 0.0;
	const double ain = std::clamp(t / inT, 0.0, 1.0), aout = std::clamp((total - t) / outT, 0.0, 1.0);
	if (st.animation == "pop") {
		scale = 0.6 + 0.4 * easeOutBack(ain);
		opacity = std::min(ain * 2, 1.0) * aout;
	} else if (st.animation == "slide") {
		dy = (1 - easeOut(ain)) * size.height() * 0.6 - (1 - aout) * size.height() * 0.3;
		opacity = std::min(ain * 1.5, 1.0) * aout;
	} else if (st.animation == "bounce") {
		dy = -(1 - easeOutBack(ain)) * size.height() * 0.5;
		opacity = std::min(ain * 3, 1.0) * aout;
		scale = 1.0 + 0.04 * std::sin(t * 6.0) * (ain >= 1.0 ? 1 : 0);
	} else {
		opacity = ain * aout;
	}

	QPainter g(&img);
	g.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
	g.setOpacity(std::clamp(opacity, 0.0, 1.0));
	g.translate(size.width() / 2.0, size.height() / 2.0 + dy);
	g.scale(scale, scale);
	g.translate(-size.width() / 2.0, -size.height() / 2.0);

	const QColor accent(st.accent), text(st.color);
	const bool card = QColor(st.color).lightness() < 128; // dark text needs a light card behind it
	const double W = size.width(), H = size.height();
	QRectF picArea, textArea;
	if (st.layout == "side") {
		picArea = QRectF(0, 0, W * 0.38, H);
		textArea = QRectF(W * 0.40, 0, W * 0.60, H);
	} else if (st.layout == "text") {
		textArea = QRectF(0, 0, W, H);
	} else {
		picArea = QRectF(0, 0, W, H * 0.52);
		textArea = QRectF(0, H * 0.54, W, H * 0.46);
	}
	if (card) {
		QPainterPath path;
		path.addRoundedRect(textArea.adjusted(W * 0.06, 4, -W * 0.06, -4), 22, 22);
		g.fillPath(path, QColor(255, 255, 255, 236));
		g.setPen(QPen(accent, 3));
		g.drawPath(path);
	}
	// the picture, or a glowing emoji badge
	if (!picArea.isEmpty()) {
		if (!picture.isNull()) {
			QSizeF s = picture.size();
			s.scale(picArea.size() * 0.95, Qt::KeepAspectRatio);
			g.drawImage(QRectF(picArea.center() - QPointF(s.width() / 2, s.height() / 2), s), picture);
		} else {
			const double r = std::min(picArea.width(), picArea.height()) * 0.36;
			QRadialGradient glow(picArea.center(), r * 1.6);
			QColor a = accent;
			a.setAlpha(150);
			glow.setColorAt(0, a);
			a.setAlpha(0);
			glow.setColorAt(1, a);
			g.setPen(Qt::NoPen);
			g.setBrush(glow);
			g.drawEllipse(picArea.center(), r * 1.6, r * 1.6);
			QFont emoji("Segoe UI Emoji");
			emoji.setPixelSize(int(r * 1.25));
			g.setFont(emoji);
			g.setPen(Qt::white);
			g.drawText(picArea, Qt::AlignCenter, emojiFor(e.type));
		}
	}
	// the title, with the name in the accent colour, and the message under it
	QString title = st.title;
	title.replace("{name}", "<span style='color:" + accent.name() + "'>{name}</span>");
	title.replace("{amount}", "<span style='color:" + accent.name() + "'>{amount}</span>");
	QTextDocument doc;
	QFont font(st.font);
	font.setPixelSize(st.size);
	font.setBold(true);
	doc.setDefaultFont(font);
	doc.setDefaultStyleSheet("body { color: " + text.name() + "; }");
	doc.setTextWidth(textArea.width() * 0.9);
	QString html = "<body><div align='center'>" + fill(title, e) + "</div>";
	if (st.showMessage && !e.message.trimmed().isEmpty() && e.type != "follow") {
		const QString msg = e.message.left(200).toHtmlEscaped();
		html += QString("<div align='center' style='font-size:%1px; font-weight:500; margin-top:8px'>%2</div>")
				.arg(int(st.size * 0.55))
				.arg(msg);
	}
	doc.setHtml(html + "</body>");
	const double tx = textArea.x() + (textArea.width() - doc.textWidth()) / 2;
	const double ty = textArea.y() + std::max(0.0, (textArea.height() - doc.size().height()) / 2);
	if (!card) { // a soft shadow keeps light text readable over any game
		QTextDocument shadow;
		shadow.setDefaultFont(font);
		shadow.setDefaultStyleSheet("body { color: #000000; } span { color: #000000 !important; }");
		shadow.setTextWidth(doc.textWidth());
		QString dark = html;
		dark.replace(QRegularExpression("color:#[0-9a-fA-F]{6}"), "color:#000000");
		shadow.setHtml(dark + "</body>");
		g.save();
		g.setOpacity(g.opacity() * 0.55);
		for (const QPointF off : {QPointF(2, 3), QPointF(0, 2), QPointF(3, 1)}) {
			g.save();
			g.translate(tx + off.x(), ty + off.y());
			shadow.drawContents(&g);
			g.restore();
		}
		g.restore();
	}
	g.translate(tx, ty);
	doc.drawContents(&g);
	return img;
}

// ---- sound and speech
QStringList voices()
{
	QStringList out;
	ISpObjectTokenCategory *cat = nullptr;
	if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&cat))))
		return out;
	IEnumSpObjectTokens *tokens = nullptr;
	if (SUCCEEDED(cat->SetId(SPCAT_VOICES, FALSE)) && SUCCEEDED(cat->EnumTokens(nullptr, nullptr, &tokens))) {
		ISpObjectToken *token = nullptr;
		while (tokens->Next(1, &token, nullptr) == S_OK) {
			LPWSTR name = nullptr;
			if (SUCCEEDED(token->GetStringValue(nullptr, &name))) {
				out << QString::fromWCharArray(name);
				CoTaskMemFree(name);
			}
			token->Release();
		}
		tokens->Release();
	}
	cat->Release();
	return out;
}

namespace {
const GUID WAVE_FORMAT_ID = {0xC31ADBAE, 0x527F, 0x4ff5, {0xA2, 0x30, 0xF6, 0x2B, 0xB6, 0x1F, 0xF7, 0x0C}};

// Says text into a .wav with a Windows voice (on a worker thread: speech takes a moment).
bool speakToFile(const QString &text, const QString &voiceName, const QString &path)
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	bool ok = false;
	ISpVoice *voice = nullptr;
	ISpStream *stream = nullptr;
	if (SUCCEEDED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&voice))) &&
	    SUCCEEDED(CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&stream)))) {
		WAVEFORMATEX wfx{WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0};
		if (SUCCEEDED(stream->BindToFile(reinterpret_cast<const wchar_t *>(path.utf16()), SPFM_CREATE_ALWAYS,
						 &WAVE_FORMAT_ID, &wfx, 0))) {
			if (!voiceName.isEmpty()) {
				ISpObjectTokenCategory *cat = nullptr;
				IEnumSpObjectTokens *tokens = nullptr;
				if (SUCCEEDED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&cat))) &&
				    SUCCEEDED(cat->SetId(SPCAT_VOICES, FALSE)) && SUCCEEDED(cat->EnumTokens(nullptr, nullptr, &tokens))) {
					ISpObjectToken *token = nullptr;
					while (tokens->Next(1, &token, nullptr) == S_OK) {
						LPWSTR name = nullptr;
						if (SUCCEEDED(token->GetStringValue(nullptr, &name))) {
							if (QString::fromWCharArray(name) == voiceName)
								voice->SetVoice(token);
							CoTaskMemFree(name);
						}
						token->Release();
					}
				}
				if (tokens)
					tokens->Release();
				if (cat)
					cat->Release();
			}
			voice->SetOutput(stream, TRUE);
			ok = SUCCEEDED(voice->Speak(reinterpret_cast<const wchar_t *>(text.utf16()), SPF_IS_NOT_XML, nullptr));
			stream->Close();
		}
	}
	if (stream)
		stream->Release();
	if (voice)
		voice->Release();
	CoUninitialize();
	return ok;
}

obs_source_t *player(obs_source_t **slot, const char *name, int channel)
{
	if (!*slot) {
		obs_data_t *s = obs_data_create();
		obs_data_set_bool(s, "is_local_file", true);
		obs_data_set_bool(s, "restart_on_activate", false);
		obs_data_set_bool(s, "close_when_inactive", false);
		*slot = obs_source_create_private("ffmpeg_source", name, s);
		obs_data_release(s);
		if (*slot) {
			obs_set_output_source(uint32_t(channel), *slot); // mixed into the stream like desktop audio
			obs_source_set_monitoring_enabled(*slot, Settings::get().value("alerts/hear", true).toBool());
		}
	}
	return *slot;
}
} // namespace

void playSound(const QString &file, int volume)
{
	if (file.isEmpty() || !QFileInfo::exists(file))
		return;
	obs_source_t *p = player(&Engine::get().m_sound, "Zunimi alert sound", 5);
	if (!p)
		return;
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "local_file", file.toUtf8().constData());
	obs_source_update(p, s);
	obs_data_release(s);
	obs_source_set_volume(p, float(std::clamp(volume, 0, 100)) / 100.0f);
	obs_source_set_monitoring_enabled(p, Settings::get().value("alerts/hear", true).toBool());
	obs_source_media_restart(p);
}

void speak(const QString &text, const QString &voice, int volume)
{
	if (text.trimmed().isEmpty())
		return;
	static int n = 0;
	const QString path = QDir::temp().filePath(QString("zunimi-tts-%1.wav").arg(n++ % 4));
	QPointer<Engine> engine(&Engine::get());
	std::thread([text, voice, volume, path, engine] {
		if (!speakToFile(text.left(300), voice, path))
			return;
		QMetaObject::invokeMethod(
			engine.data(),
			[path, volume] {
				obs_source_t *p = player(&Engine::get().m_voice, "Zunimi alert voice", 6);
				if (!p)
					return;
				obs_data_t *s = obs_data_create();
				obs_data_set_string(s, "local_file", path.toUtf8().constData());
				obs_source_update(p, s);
				obs_data_release(s);
				obs_source_set_volume(p, float(std::clamp(volume, 0, 100)) / 100.0f);
				obs_source_set_monitoring_enabled(p, Settings::get().value("alerts/hear", true).toBool());
				obs_source_media_restart(p);
			},
			Qt::QueuedConnection);
	}).detach();
}

// ---- the source
namespace {
void *srcCreate(obs_data_t *settings, obs_source_t *source)
{
	auto *holder = new std::shared_ptr<Shared>(std::make_shared<Shared>());
	(*holder)->source = source;
	Engine::get().add(*holder, settings);
	return holder;
}

void srcDestroy(void *data)
{
	auto *holder = static_cast<std::shared_ptr<Shared> *>(data);
	Shared *s = holder->get();
	{
		std::lock_guard<std::mutex> lock(s->lock);
		s->alive = false;
	}
	QMetaObject::invokeMethod(&Engine::get(), [s] { Engine::get().remove(s); }, Qt::QueuedConnection);
	if (s->texture) {
		obs_enter_graphics();
		gs_texture_destroy(s->texture);
		obs_leave_graphics();
		s->texture = nullptr;
	}
	delete holder; // the engine may still hold the shared part until it notices
}

void srcUpdate(void *data, obs_data_t *settings)
{
	Shared *s = static_cast<std::shared_ptr<Shared> *>(data)->get();
	obs_data_addref(settings);
	QMetaObject::invokeMethod(
		&Engine::get(),
		[s, settings] {
			Engine::get().update(s, settings);
			obs_data_release(settings);
		},
		Qt::QueuedConnection);
}

uint32_t srcWidth(void *data)
{
	return uint32_t(static_cast<std::shared_ptr<Shared> *>(data)->get()->width);
}

uint32_t srcHeight(void *data)
{
	return uint32_t(static_cast<std::shared_ptr<Shared> *>(data)->get()->height);
}

void srcRender(void *data, gs_effect_t *)
{
	Shared *s = static_cast<std::shared_ptr<Shared> *>(data)->get();
	{
		std::lock_guard<std::mutex> lock(s->lock);
		if (s->changed && !s->pending.isNull()) {
			const QImage &img = s->pending;
			const uint8_t *bits = img.constBits();
			if (s->texture && (gs_texture_get_width(s->texture) != uint32_t(img.width()) ||
					   gs_texture_get_height(s->texture) != uint32_t(img.height()))) {
				gs_texture_destroy(s->texture);
				s->texture = nullptr;
			}
			if (!s->texture)
				s->texture = gs_texture_create(uint32_t(img.width()), uint32_t(img.height()), GS_RGBA, 1, &bits, GS_DYNAMIC);
			else
				gs_texture_set_image(s->texture, bits, uint32_t(img.bytesPerLine()), false);
			s->changed = false;
		}
	}
	if (!s->texture)
		return;
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, s->texture);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(s->texture, 0, 0, 0);
	gs_blend_state_pop();
}

const char *srcName(void *)
{
	return "Zunimi overlay";
}

void srcDefaults(obs_data_t *s)
{
	obs_data_set_default_string(s, "kind", "alerts");
	obs_data_set_default_int(s, "width", 800);
	obs_data_set_default_int(s, "height", 450);
	obs_data_set_default_int(s, "font_size", 24);
	obs_data_set_default_int(s, "max", 10);
	obs_data_set_default_int(s, "fade", 0);
	obs_data_set_default_bool(s, "bubbles", true);
	obs_data_set_default_bool(s, "hide_bots", true);
	obs_data_set_default_string(s, "goal", "follows");
	obs_data_set_default_int(s, "target", 50);
	obs_data_set_default_string(s, "title", "");
	obs_data_set_default_string(s, "what", "latest_follow");
	obs_data_set_default_string(s, "color", "#22D3EE");
	obs_data_set_default_string(s, "text_color", "#FFFFFF");
	obs_data_set_default_string(s, "mode", "countdown");
	obs_data_set_default_int(s, "minutes", 10);
	obs_data_set_default_int(s, "add_sub", 60);
	obs_data_set_default_int(s, "add_tip", 60);
	obs_data_set_default_int(s, "add_bits", 30);
	obs_data_set_default_string(s, "done_text", "Starting now!");
}

obs_properties_t *srcProperties(void *data)
{
	Shared *sh = static_cast<std::shared_ptr<Shared> *>(data)->get();
	obs_data_t *settings = obs_source_get_settings(sh->source);
	const QString kind = QString::fromUtf8(obs_data_get_string(settings, "kind"));
	obs_data_release(settings);
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_int(p, "width", "Width", 100, 3840, 10);
	obs_properties_add_int(p, "height", "Height", 60, 2160, 10);
	if (kind == "alerts") {
		obs_properties_add_button(p, "design", "Design the alerts...", [](obs_properties_t *, obs_property_t *, void *d) {
			Shared *s = static_cast<std::shared_ptr<Shared> *>(d)->get();
			if (Engine::get().openDesigner)
				Engine::get().openDesigner(s->source);
			return false;
		});
		for (const QString &t : alertTypes())
			obs_properties_add_button(p, ("test_" + t).toUtf8().constData(),
						  ("Test: " + alertTypeLabel(t)).toUtf8().constData(),
						  [](obs_properties_t *, obs_property_t *prop, void *) {
							  EventHub::get().test(QString::fromUtf8(obs_property_name(prop)).mid(5));
							  return false;
						  });
	} else if (kind == "chat") {
		obs_properties_add_int_slider(p, "font_size", "Text size", 12, 64, 1);
		obs_properties_add_int(p, "max", "Messages shown", 1, 50, 1);
		obs_properties_add_int(p, "fade", "Hide a message after (seconds, 0 = never)", 0, 600, 5);
		obs_properties_add_bool(p, "bubbles", "Bubbles behind messages");
		obs_properties_add_bool(p, "hide_bots", "Hide bots (Nightbot, StreamElements...)");
		obs_properties_add_bool(p, "hide_commands", "Hide !commands");
	} else if (kind == "goal") {
		obs_property_t *goal = obs_properties_add_list(p, "goal", "Counts", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		for (const auto &[k, v] : {std::pair{"follows", "Follows"}, {"subs", "Subs"}, {"bits", "Bits"},
					   {"tips", "Tips (main currency)"}, {"members", "YouTube members"}})
			obs_property_list_add_string(goal, v, k);
		obs_properties_add_int(p, "target", "Goal", 1, 100000000, 1);
		obs_properties_add_text(p, "title", "Title (empty: automatic)", OBS_TEXT_DEFAULT);
		obs_properties_add_color(p, "color_int", "Bar colour");
		obs_properties_add_button(p, "restart", "Start counting from zero", [](obs_properties_t *, obs_property_t *, void *d) {
			Shared *s = static_cast<std::shared_ptr<Shared> *>(d)->get();
			obs_data_t *st = obs_source_get_settings(s->source);
			obs_data_set_double(st, "base", EventHub::get().total(QString::fromUtf8(obs_data_get_string(st, "goal"))));
			obs_source_update(s->source, st);
			obs_data_release(st);
			return false;
		});
	} else if (kind == "label") {
		obs_property_t *what = obs_properties_add_list(p, "what", "Shows", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		for (const auto &[k, v] :
		     {std::pair{"latest_follow", "Latest follower"}, {"latest_sub", "Latest sub"}, {"latest_tip", "Latest tip"},
		      {"latest_bits", "Latest cheer"}, {"latest_superchat", "Latest Super Chat"}, {"top_tip", "Top tipper"},
		      {"top_bits", "Top cheerer"}, {"total_follows", "Follows this stream"}, {"total_subs", "Subs this stream"},
		      {"total_tips", "Tips this stream"}, {"total_bits", "Bits this stream"}})
			obs_property_list_add_string(what, v, k);
		obs_properties_add_text(p, "title", "Before it (empty: automatic)", OBS_TEXT_DEFAULT);
		obs_properties_add_int_slider(p, "font_size", "Text size", 12, 120, 1);
	} else if (kind == "timer") {
		obs_property_t *mode = obs_properties_add_list(p, "mode", "Kind", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(mode, "Countdown (starting soon, BRB)", "countdown");
		obs_property_list_add_string(mode, "Subathon (subs, tips and bits add time)", "subathon");
		obs_property_list_add_string(mode, "Clock", "clock");
		obs_property_list_add_string(mode, "Time live", "uptime");
		obs_properties_add_int(p, "minutes", "Start with (minutes)", 0, 100000, 1);
		obs_properties_add_int(p, "add_sub", "A sub adds (seconds)", 0, 3600, 5);
		obs_properties_add_int(p, "add_tip", "Every 100 of your currency adds (seconds)", 0, 3600, 5);
		obs_properties_add_int(p, "add_bits", "Every 100 bits add (seconds)", 0, 3600, 5);
		obs_properties_add_text(p, "title", "Title", OBS_TEXT_DEFAULT);
		obs_properties_add_text(p, "done_text", "When it reaches zero", OBS_TEXT_DEFAULT);
		obs_properties_add_int_slider(p, "font_size", "Text size", 12, 200, 1);
		obs_properties_add_button(p, "restart", "Start / restart now", [](obs_properties_t *, obs_property_t *, void *d) {
			Shared *s = static_cast<std::shared_ptr<Shared> *>(d)->get();
			obs_data_t *st = obs_source_get_settings(s->source);
			obs_data_set_int(st, "ends", QDateTime::currentMSecsSinceEpoch() + obs_data_get_int(st, "minutes") * 60000);
			obs_source_update(s->source, st);
			obs_data_release(st);
			return false;
		});
	} else if (kind == "events") {
		obs_properties_add_int(p, "max", "Events shown", 1, 20, 1);
		obs_properties_add_int_slider(p, "font_size", "Text size", 12, 64, 1);
	}
	return p;
}

void enumActive(void *, obs_source_enum_proc_t, void *) {}
} // namespace

// ---- engine
struct Engine::Instance {
	std::weak_ptr<Shared> shared;
	Shared *raw = nullptr;
	QString kind;
	QJsonObject design;           // alerts
	QString designText;
	obs_data_t *settings = nullptr; // a copy of the values we read
	bool dirty = true;
	// alerts
	std::deque<StreamEvent> queue;
	std::optional<StreamEvent> showing;
	AlertStyle style;
	qint64 started = 0;
	QImage picture;
	std::unique_ptr<QMovie> movie;
	// chat
	std::deque<ChatMessage> chat;
	// timer
	qint64 lastSecond = -1;
	~Instance() { obs_data_release(settings); }
};

Engine &Engine::get()
{
	static Engine engine;
	return engine;
}

Engine::Engine() : m_timer(new QTimer(this))
{
	connect(m_timer, &QTimer::timeout, this, &Engine::tick);
	m_timer->start(33);
	connect(&EventHub::get(), &EventHub::event, this, &Engine::onEvent);
	connect(&EventHub::get(), &EventHub::chat, this, &Engine::onChat);
	connect(&EventHub::get(), &EventHub::totalsChanged, this, [this] {
		for (auto &in : m_instances)
			if (in->kind == "goal" || in->kind == "label")
				in->dirty = true;
	});
}

void Engine::registerSource()
{
	obs_source_info info{};
	info.id = ID;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = srcName;
	info.create = srcCreate;
	info.destroy = srcDestroy;
	info.update = srcUpdate;
	info.get_width = srcWidth;
	info.get_height = srcHeight;
	info.video_render = srcRender;
	info.get_defaults = srcDefaults;
	info.get_properties = srcProperties;
	info.icon_type = OBS_ICON_TYPE_TEXT;
	(void)enumActive;
	obs_register_source(&info);
}

void Engine::shutdown()
{
	m_timer->stop();
	for (obs_source_t **p : {&m_sound, &m_voice}) {
		if (*p) {
			obs_set_output_source(*p == m_sound ? 5 : 6, nullptr);
			obs_source_release(*p);
		}
		*p = nullptr;
	}
	m_instances.clear();
}

void Engine::add(const std::shared_ptr<Shared> &shared, obs_data_t *settings)
{
	auto in = std::make_shared<Instance>();
	in->shared = shared;
	in->raw = shared.get();
	m_instances << in;
	update(shared.get(), settings);
}

void Engine::remove(Shared *)
{
	// the source is gone once nothing holds its shared part (an address can be reused, so don't compare it)
	m_instances.erase(std::remove_if(m_instances.begin(), m_instances.end(),
					 [](const std::shared_ptr<Instance> &in) { return in->shared.expired(); }),
			  m_instances.end());
}

void Engine::update(Shared *shared, obs_data_t *settings)
{
	for (auto &in : m_instances) {
		if (in->raw != shared)
			continue;
		obs_data_release(in->settings);
		in->settings = obs_data_create();
		srcDefaults(in->settings); // defaults first, so unset values read as their defaults
		obs_data_apply(in->settings, settings);
		in->kind = QString::fromUtf8(obs_data_get_string(settings, "kind"));
		const QString designText = QString::fromUtf8(obs_data_get_string(settings, "design"));
		if (designText != in->designText || in->design.isEmpty()) {
			in->designText = designText;
			in->design = designText.isEmpty() ? defaultDesign() : QJsonDocument::fromJson(designText.toUtf8()).object();
		}
		if (in->kind == "timer" && obs_data_get_int(settings, "ends") == 0 &&
		    QString::fromUtf8(obs_data_get_string(settings, "mode")) != "clock") {
			obs_data_set_int(in->settings, "ends", QDateTime::currentMSecsSinceEpoch() + obs_data_get_int(settings, "minutes") * 60000);
		}
		if (auto s = in->shared.lock()) {
			std::lock_guard<std::mutex> lock(s->lock);
			s->width = int(std::clamp<long long>(obs_data_get_int(settings, "width"), 100, 3840));
			s->height = int(std::clamp<long long>(obs_data_get_int(settings, "height"), 60, 2160));
		}
		in->dirty = true;
	}
}

void Engine::replay(const StreamEvent &e)
{
	StreamEvent again = e;
	again.test = true;
	onEvent(again);
}

void Engine::onEvent(const StreamEvent &e)
{
	for (auto &in : m_instances) {
		if (in->kind == "alerts") {
			const AlertStyle st = AlertStyle::fromJson(in->design.value(e.type).toObject(), e.type);
			const bool money = e.type == "tip" || e.type == "superchat" || e.type == "bits";
			if (!st.on || (money && e.amount < st.min))
				continue;
			if (in->queue.size() < 50)
				in->queue.push_back(e);
		} else if (in->kind == "events") {
			in->dirty = true;
		} else if (in->kind == "timer" && !e.test) {
			const QString mode = QString::fromUtf8(obs_data_get_string(in->settings, "mode"));
			if (mode != "subathon")
				continue;
			qint64 add = 0;
			if (e.type == "sub" || e.type == "resub" || e.type == "member")
				add = obs_data_get_int(in->settings, "add_sub");
			else if (e.type == "gift")
				add = obs_data_get_int(in->settings, "add_sub") * std::max(1, e.count);
			else if (e.type == "tip" || e.type == "superchat")
				add = qint64(obs_data_get_int(in->settings, "add_tip") * e.amount / 100.0);
			else if (e.type == "bits")
				add = qint64(obs_data_get_int(in->settings, "add_bits") * e.amount / 100.0);
			if (add > 0) {
				const qint64 ends = std::max<qint64>(obs_data_get_int(in->settings, "ends"), QDateTime::currentMSecsSinceEpoch());
				obs_data_set_int(in->settings, "ends", ends + add * 1000);
				if (auto s = in->shared.lock()) { // keep it across restarts
					obs_data_t *st = obs_source_get_settings(s->source);
					obs_data_set_int(st, "ends", ends + add * 1000);
					obs_data_release(st);
				}
			}
		}
	}
}

void Engine::onChat(const ChatMessage &m)
{
	for (auto &in : m_instances) {
		if (in->kind != "chat")
			continue;
		if (obs_data_get_bool(in->settings, "hide_bots") && BOTS.contains(m.user.toLower()))
			continue;
		if (obs_data_get_bool(in->settings, "hide_commands") && m.text.startsWith('!'))
			continue;
		in->chat.push_back(m);
		while (int(in->chat.size()) > std::max<long long>(1, obs_data_get_int(in->settings, "max")))
			in->chat.pop_front();
		in->dirty = true;
	}
}

void Engine::tick()
{
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	for (auto &in : m_instances) {
		auto s = in->shared.lock();
		if (!s)
			continue;
		bool animating = false;
		if (in->kind == "alerts") {
			if (!in->showing && !in->queue.empty()) {
				in->showing = in->queue.front();
				in->queue.pop_front();
				const StreamEvent &e = *in->showing;
				in->style = AlertStyle::fromJson(in->design.value(e.type).toObject(), e.type);
				// a bigger amount can bring its own title, picture and sound
				for (const AlertStyle::Variant &v : in->style.variants) {
					if (e.amount >= v.min || e.count >= v.min) {
						if (!v.title.isEmpty())
							in->style.title = v.title;
						if (!v.image.isEmpty())
							in->style.image = v.image;
						if (!v.sound.isEmpty())
							in->style.sound = v.sound;
					}
				}
				in->movie.reset();
				in->picture = QImage();
				if (!in->style.image.isEmpty()) {
					if (in->style.image.endsWith(".gif", Qt::CaseInsensitive)) {
						in->movie = std::make_unique<QMovie>(in->style.image);
						in->movie->start();
					} else {
						in->picture.load(in->style.image);
					}
				}
				in->started = now;
				playSound(in->style.sound, in->style.volume);
				if (in->style.tts && !e.message.trimmed().isEmpty() && e.amount >= in->style.ttsMin)
					speak(e.user + " says: " + e.message, Settings::get().value("alerts/voice").toString(),
					      Settings::get().value("alerts/voiceVolume", 90).toInt());
			}
			if (in->showing) {
				animating = true;
				if (now - in->started > qint64(in->style.seconds) * 1000 + 400) {
					in->showing.reset();
					in->movie.reset();
					in->dirty = true; // one clear frame
					animating = false;
				}
			}
		} else if (in->kind == "chat") {
			const int fade = int(obs_data_get_int(in->settings, "fade"));
			if (fade > 0) {
				while (!in->chat.empty() && in->chat.front().at.msecsTo(QDateTime::currentDateTime()) > fade * 1000LL + 1000) {
					in->chat.pop_front();
					in->dirty = true;
				}
				for (const ChatMessage &m : in->chat)
					animating |= m.at.msecsTo(QDateTime::currentDateTime()) > fade * 1000LL; // fading out
			}
			for (const ChatMessage &m : in->chat)
				animating |= m.at.msecsTo(QDateTime::currentDateTime()) < 300; // sliding in
		} else if (in->kind == "timer" || in->kind == "label") {
			if (now / 1000 != in->lastSecond) {
				in->lastSecond = now / 1000;
				in->dirty |= in->kind == "timer";
			}
		}
		if (animating || in->dirty) {
			in->dirty = false;
			paint(*in, now);
		}
	}
}

void Engine::paint(Instance &in, qint64 now)
{
	auto s = in.shared.lock();
	if (!s)
		return;
	int w, h;
	{
		std::lock_guard<std::mutex> lock(s->lock);
		w = s->width;
		h = s->height;
	}
	QImage img(w, h, QImage::Format_RGBA8888);
	img.fill(Qt::transparent);
	obs_data_t *st = in.settings;
	const int fontSize = int(obs_data_get_int(st, "font_size"));
	const QColor accent(QString::fromUtf8(obs_data_get_string(st, "color")));
	const QColor textColor(QString::fromUtf8(obs_data_get_string(st, "text_color")));
	QPainter g(&img);
	g.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

	if (in.kind == "alerts") {
		if (in.showing) {
			const double p = double(now - in.started) / (in.style.seconds * 1000.0);
			const QImage pic = in.movie ? in.movie->currentImage() : in.picture;
			g.drawImage(0, 0, paintAlert(in.style, *in.showing, QSize(w, h), std::clamp(p, 0.001, 0.999), pic));
		}
	} else if (in.kind == "chat") {
		// newest at the bottom; older ones move up and out
		QFont f("Segoe UI");
		f.setPixelSize(fontSize);
		QFont bold = f;
		bold.setBold(true);
		const bool bubbles = obs_data_get_bool(st, "bubbles");
		const int fade = int(obs_data_get_int(st, "fade"));
		double y = h - 6;
		for (auto it = in.chat.rbegin(); it != in.chat.rend() && y > 0; ++it) {
			const ChatMessage &m = *it;
			const qint64 age = m.at.msecsTo(QDateTime::currentDateTime());
			QTextDocument doc;
			doc.setDefaultFont(f);
			doc.setTextWidth(w - 28);
			const QColor name = m.color.isEmpty() ? nameColor(m.user) : QColor(m.color);
			QString badges;
			if (m.owner)
				badges += "🎙 ";
			else if (m.mod)
				badges += "🛡 ";
			else if (m.vip)
				badges += "💎 ";
			doc.setHtml(QString("<span style='color:%1; font-weight:700'>%2</span> "
					    "<span style='color:%3; font-weight:800'>%4%5</span>"
					    "<span style='color:#FFFFFF'>  %6</span>")
					    .arg(platformColor(m.platform).name(), platformTag(m.platform), name.name(),
						 badges.toHtmlEscaped(), m.user.toHtmlEscaped(), m.text.toHtmlEscaped()));
			const double bh = doc.size().height() + 8;
			y -= bh + 6;
			double opacity = 1.0;
			if (fade > 0 && age > fade * 1000LL)
				opacity = std::max(0.0, 1.0 - (age - fade * 1000.0) / 1000.0);
			const double slide = age < 300 ? (1.0 - age / 300.0) * 20.0 : 0.0;
			g.save();
			g.setOpacity(opacity);
			g.translate(0, slide);
			if (bubbles) {
				QPainterPath path;
				path.addRoundedRect(QRectF(4, y, w - 8, bh), 12, 12);
				g.fillPath(path, QColor(11, 13, 18, 190));
			}
			g.translate(14, y + 4);
			doc.drawContents(&g);
			g.restore();
		}
	} else if (in.kind == "goal") {
		const QString key = QString::fromUtf8(obs_data_get_string(st, "goal"));
		const double target = std::max<double>(1, double(obs_data_get_int(st, "target")));
		const double now = std::max(0.0, EventHub::get().total(key) - obs_data_get_double(st, "base"));
		static const QHash<QString, QString> names = {{"follows", "Follower goal"}, {"subs", "Sub goal"},
							      {"bits", "Bits goal"}, {"tips", "Tip goal"},
							      {"members", "Member goal"}};
		QString title = QString::fromUtf8(obs_data_get_string(st, "title"));
		if (title.isEmpty())
			title = names.value(key, "Goal");
		long long colorInt = obs_data_get_int(st, "color_int");
		const QColor bar = colorInt ? QColor(int(colorInt & 0xFF), int((colorInt >> 8) & 0xFF), int((colorInt >> 16) & 0xFF)) : QColor("#22D3EE");
		QFont f("Segoe UI");
		f.setPixelSize(std::max(14, h / 4));
		f.setBold(true);
		g.setFont(f);
		const QRectF titleRect(0, 0, w, h * 0.42);
		g.setPen(QColor(0, 0, 0, 140));
		g.drawText(titleRect.translated(1, 2), Qt::AlignLeft | Qt::AlignVCenter, title);
		g.setPen(Qt::white);
		g.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, title);
		const QString count = key == "tips" ? QString("%1 / %2").arg(now, 0, 'f', 0).arg(target, 0, 'f', 0)
						    : QString("%1 / %2").arg(qlonglong(now)).arg(qlonglong(target));
		g.drawText(titleRect, Qt::AlignRight | Qt::AlignVCenter, count);
		const QRectF track(0, h * 0.5, w, h * 0.42);
		QPainterPath tp;
		tp.addRoundedRect(track, track.height() / 2, track.height() / 2);
		g.fillPath(tp, QColor(255, 255, 255, 40));
		const double frac = std::clamp(now / target, 0.0, 1.0);
		if (frac > 0) {
			QPainterPath fp;
			fp.addRoundedRect(QRectF(track.x(), track.y(), std::max(track.height(), track.width() * frac), track.height()),
					  track.height() / 2, track.height() / 2);
			QLinearGradient grad(track.topLeft(), track.topRight());
			grad.setColorAt(0, bar.darker(130));
			grad.setColorAt(1, bar);
			g.fillPath(fp, grad);
		}
	} else if (in.kind == "label") {
		const QString what = QString::fromUtf8(obs_data_get_string(st, "what"));
		EventHub &hub = EventHub::get();
		static const QHash<QString, QString> titles = {
			{"latest_follow", "Latest follower: "}, {"latest_sub", "Latest sub: "}, {"latest_tip", "Latest tip: "},
			{"latest_bits", "Latest cheer: "},      {"latest_superchat", "Latest Super Chat: "},
			{"top_tip", "Top tipper: "},            {"top_bits", "Top cheerer: "},
			{"total_follows", "Follows: "},         {"total_subs", "Subs: "},
			{"total_tips", "Tips: "},               {"total_bits", "Bits: "}};
		QString value;
		if (what.startsWith("latest_"))
			value = hub.latest(what.mid(7));
		else if (what.startsWith("top_"))
			value = hub.top(what.mid(4));
		else
			value = QString::number(hub.total(what.mid(6)), 'f', 0);
		QString title = QString::fromUtf8(obs_data_get_string(st, "title"));
		if (title.isEmpty())
			title = titles.value(what);
		QFont f("Segoe UI");
		f.setPixelSize(fontSize);
		f.setBold(true);
		g.setFont(f);
		const QString text = title + (value.isEmpty() ? QString("—") : value);
		g.setPen(QColor(0, 0, 0, 150));
		g.drawText(QRectF(2, 3, w, h), Qt::AlignLeft | Qt::AlignVCenter, text);
		g.setPen(textColor);
		g.drawText(QRectF(0, 0, w, h), Qt::AlignLeft | Qt::AlignVCenter, text);
	} else if (in.kind == "timer") {
		const QString mode = QString::fromUtf8(obs_data_get_string(st, "mode"));
		QString big;
		if (mode == "clock") {
			big = QDateTime::currentDateTime().toString("hh:mm");
		} else if (mode == "uptime") {
			const qint64 since = Settings::get().value("events/liveSince", 0).toLongLong();
			const qint64 secs = since ? (now - since) / 1000 : 0;
			big = QString("%1:%2:%3").arg(secs / 3600).arg((secs / 60) % 60, 2, 10, QChar('0')).arg(secs % 60, 2, 10, QChar('0'));
		} else {
			const qint64 left = std::max<qint64>(0, (obs_data_get_int(st, "ends") - now) / 1000);
			if (left == 0 && mode == "countdown")
				big = QString::fromUtf8(obs_data_get_string(st, "done_text"));
			else
				big = left >= 3600 ? QString("%1:%2:%3").arg(left / 3600).arg((left / 60) % 60, 2, 10, QChar('0')).arg(left % 60, 2, 10, QChar('0'))
						   : QString("%1:%2").arg(left / 60, 2, 10, QChar('0')).arg(left % 60, 2, 10, QChar('0'));
		}
		const QString title = QString::fromUtf8(obs_data_get_string(st, "title"));
		QFont f("Segoe UI");
		f.setBold(true);
		f.setPixelSize(fontSize > 24 ? fontSize : h / 2);
		QRectF area(0, 0, w, h);
		if (!title.isEmpty()) {
			QFont tf = f;
			tf.setPixelSize(std::max(12, int(f.pixelSize() * 0.35)));
			g.setFont(tf);
			g.setPen(QColor(255, 255, 255, 200));
			g.drawText(QRectF(0, 0, w, h * 0.3), Qt::AlignCenter, title);
			area = QRectF(0, h * 0.28, w, h * 0.72);
		}
		g.setFont(f);
		g.setPen(QColor(0, 0, 0, 150));
		g.drawText(area.translated(2, 3), Qt::AlignCenter, big);
		g.setPen(textColor);
		g.drawText(area, Qt::AlignCenter, big);
	} else if (in.kind == "events") {
		QFont f("Segoe UI");
		f.setPixelSize(fontSize);
		g.setFont(f);
		const int max = int(obs_data_get_int(st, "max"));
		const double row = fontSize * 1.7;
		int i = 0;
		for (const StreamEvent &e : EventHub::get().recent()) {
			if (i >= max || (i + 1) * row > h)
				break;
			const QRectF r(0, i * row, w, row - 4);
			QPainterPath path;
			path.addRoundedRect(r, 10, 10);
			g.fillPath(path, QColor(11, 13, 18, 170));
			g.setPen(Qt::white);
			QString line = emojiFor(e.type) + "  " + e.user;
			const QString amount = e.amountText();
			g.drawText(r.adjusted(12, 0, -12, 0), Qt::AlignLeft | Qt::AlignVCenter, line);
			g.setPen(accent);
			g.drawText(r.adjusted(12, 0, -12, 0), Qt::AlignRight | Qt::AlignVCenter, amount.isEmpty() ? e.label() : amount);
			i++;
		}
	}
	g.end();
	std::lock_guard<std::mutex> lock(s->lock);
	s->pending = img;
	s->changed = true;
}

} // namespace overlays
