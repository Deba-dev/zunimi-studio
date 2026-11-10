#include "ui/Templates.h"

#include "core/I18n.h"
#include "core/ObsCore.h"
#include "core/Settings.h"
#include "overlays/Overlays.h"
#include "ui/SetupWizard.h"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadialGradient>
#include <QTimer>
#include <QVBoxLayout>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QUrl>
#include "events/Events.h"

namespace templates {
namespace {
struct Palette {
	QColor bg1, bg2, accent, accent2, text;
};

Palette palette(const QString &style)
{
	if (style == "clean")
		return {QColor("#0F172A"), QColor("#1E293B"), QColor("#38BDF8"), QColor("#E2E8F0"), QColor("#F8FAFC")};
	if (style == "sunset")
		return {QColor("#1A0B16"), QColor("#3B0D2A"), QColor("#FB923C"), QColor("#F43F5E"), QColor("#FFF7ED")};
	return {QColor("#07070D"), QColor("#140B2E"), QColor("#22D3EE"), QColor("#A855F7"), QColor("#FFFFFF")}; // neon
}

// Where things go, as parts of the canvas (x, y, w, h).
struct Layout {
	QRectF title, subtitle, timer, chat, events, label, webcam, alerts;
	bool background = true, game = false;
};

Layout layout(const QString &kind)
{
	Layout l;
	const QRectF chat(0.70, 0.08, 0.27, 0.84);
	if (kind == "starting") {
		l.title = {0.05, 0.26, 0.60, 0.16};
		l.timer = {0.13, 0.46, 0.44, 0.20};
		l.chat = chat;
		l.label = {0.05, 0.86, 0.40, 0.06};
	} else if (kind == "brb") {
		l.title = {0.05, 0.34, 0.60, 0.16};
		l.subtitle = {0.05, 0.52, 0.60, 0.07};
		l.chat = chat;
	} else if (kind == "ending") {
		l.title = {0.05, 0.18, 0.60, 0.16};
		l.subtitle = {0.05, 0.36, 0.60, 0.07};
		l.events = {0.13, 0.50, 0.44, 0.36};
		l.chat = chat;
	} else if (kind == "gaming") {
		l.background = false;
		l.game = true;
		l.webcam = {0.015, 0.655, 0.25, 0.32};
		l.alerts = {0.30, 0.04, 0.40, 0.40};
		l.label = {0.30, 0.935, 0.40, 0.05};
	} else { // chatting
		l.webcam = {0.04, 0.08, 0.62, 0.84};
		l.chat = chat;
		l.alerts = {0.10, 0.06, 0.50, 0.40};
	}
	return l;
}

QRectF scaled(const QRectF &r, const QSizeF &size)
{
	return {r.x() * size.width(), r.y() * size.height(), r.width() * size.width(), r.height() * size.height()};
}

QString titleOf(const QString &kind)
{
	if (kind == "starting")
		return L("Starting soon");
	if (kind == "brb")
		return L("Be right back");
	if (kind == "ending")
		return L("Thanks for watching");
	return {};
}

QString subtitleOf(const QString &kind)
{
	if (kind == "brb")
		return L("The stream continues in a moment");
	if (kind == "ending")
		return L("See you next stream");
	return {};
}

// A rounded frame with a soft glow, for the webcam and the chat panel.
void frame(QPainter &p, const QRectF &r, const Palette &pal, double unit, bool fill)
{
	const double radius = unit * 18;
	for (int i = 6; i >= 1; --i) {
		QColor glow = pal.accent;
		glow.setAlpha(10);
		p.setPen(QPen(glow, unit * i * 3));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(r, radius, radius);
	}
	if (fill) {
		QColor panel = pal.bg2;
		panel.setAlpha(170);
		p.setBrush(panel);
	}
	QLinearGradient edge(r.topLeft(), r.bottomRight());
	edge.setColorAt(0, pal.accent);
	edge.setColorAt(1, pal.accent2);
	p.setPen(QPen(QBrush(edge), unit * 3));
	p.drawRoundedRect(r, radius, radius);
}

// The picture behind a template (or, for Gaming, the see-through frame over the game).
QImage draw(const QString &kind, const QString &style, const QSize &size, bool preview)
{
	const Palette pal = palette(style);
	const Layout lay = layout(kind);
	const QSizeF s(size);
	const double unit = s.height() / 1080.0;
	QImage img(size, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	QPainter p(&img);
	p.setRenderHint(QPainter::Antialiasing);

	if (lay.background) {
		QLinearGradient g(0, 0, s.width(), s.height());
		g.setColorAt(0, pal.bg1);
		g.setColorAt(1, pal.bg2);
		p.fillRect(img.rect(), g);
		// soft lights
		const QPointF spots[] = {{0.12, 0.18}, {0.85, 0.82}, {0.55, 0.95}};
		const QColor colors[] = {pal.accent, pal.accent2, pal.accent};
		for (int i = 0; i < 3; ++i) {
			QRadialGradient light(QPointF(spots[i].x() * s.width(), spots[i].y() * s.height()), s.height() * 0.55);
			QColor c = colors[i];
			c.setAlpha(style == "clean" ? 26 : 46);
			light.setColorAt(0, c);
			c.setAlpha(0);
			light.setColorAt(1, c);
			p.fillRect(img.rect(), light);
		}
		if (style == "neon") { // a faint grid
			QColor line = pal.accent;
			line.setAlpha(14);
			p.setPen(QPen(line, std::max(1.0, unit)));
			for (double x = 0; x < s.width(); x += unit * 60)
				p.drawLine(QPointF(x, 0), QPointF(x, s.height()));
			for (double y = 0; y < s.height(); y += unit * 60)
				p.drawLine(QPointF(0, y), QPointF(s.width(), y));
		} else if (style == "sunset") { // bands near the bottom
			for (int i = 0; i < 5; ++i) {
				QColor band = i % 2 ? pal.accent2 : pal.accent;
				band.setAlpha(18 - i * 3);
				p.fillRect(QRectF(0, s.height() * (0.78 + i * 0.045), s.width(), s.height() * 0.02), band);
			}
		} else { // clean: one thin inset border
			QColor border = pal.accent2;
			border.setAlpha(40);
			p.setPen(QPen(border, unit * 2));
			p.drawRect(QRectF(unit * 24, unit * 24, s.width() - unit * 48, s.height() - unit * 48));
		}
		// an accent bar under where the title goes
		if (!lay.title.isEmpty()) {
			const QRectF t = scaled(lay.title, s);
			QLinearGradient bar(t.left(), 0, t.left() + t.width() * 0.4, 0);
			bar.setColorAt(0, pal.accent);
			bar.setColorAt(1, pal.accent2);
			p.fillRect(QRectF(t.center().x() - t.width() * 0.2, t.bottom() + unit * 6, t.width() * 0.4, unit * 6), bar);
		}
	} else if (preview) {
		p.fillRect(img.rect(), QColor("#2A2F3A")); // stands for the game
		p.setPen(QColor("#6B7280"));
		p.setFont(QFont("Segoe UI", std::max(6, int(s.height() * 0.06)), QFont::DemiBold));
		p.drawText(img.rect(), Qt::AlignCenter, L("Your game"));
	}

	if (!lay.webcam.isEmpty()) {
		const QRectF cam = scaled(lay.webcam, s);
		if (preview) {
			p.fillRect(cam, QColor("#3B4252"));
			p.setPen(QColor("#9CA3AF"));
			p.setFont(QFont("Segoe UI", std::max(6, int(s.height() * 0.05))));
			p.drawText(cam, Qt::AlignCenter, L("Webcam"));
		}
		frame(p, cam, pal, unit, false);
	}
	if (!lay.chat.isEmpty()) {
		const QRectF chat = scaled(lay.chat, s);
		frame(p, chat, pal, unit, true);
		if (preview) {
			p.setPen(Qt::NoPen);
			for (int i = 0; i < 6; ++i) {
				QColor line = pal.text;
				line.setAlpha(60);
				p.setBrush(line);
				const double y = chat.top() + chat.height() * (0.12 + i * 0.13);
				p.drawRoundedRect(QRectF(chat.left() + chat.width() * 0.1, y, chat.width() * (i % 2 ? 0.6 : 0.78), unit * 22), unit * 8, unit * 8);
			}
		}
	}
	if (lay.game) { // a strip along the bottom for the label
		const QRectF bar = scaled(lay.label, s).adjusted(-unit * 20, -unit * 6, unit * 20, unit * 6);
		QColor panel = pal.bg2;
		panel.setAlpha(200);
		p.setPen(QPen(pal.accent, unit * 2));
		p.setBrush(panel);
		p.drawRoundedRect(bar, bar.height() / 2, bar.height() / 2);
	}
	if (preview) { // the words, as they will look
		p.setPen(pal.text);
		if (!lay.title.isEmpty()) {
			p.setFont(QFont("Segoe UI", std::max(7, int(s.height() * 0.085)), QFont::Bold));
			p.drawText(scaled(lay.title, s), Qt::AlignCenter, titleOf(kind));
		}
		if (!lay.subtitle.isEmpty()) {
			p.setPen(pal.accent);
			p.setFont(QFont("Segoe UI", std::max(6, int(s.height() * 0.04))));
			p.drawText(scaled(lay.subtitle, s), Qt::AlignCenter, subtitleOf(kind));
		}
		if (!lay.timer.isEmpty()) {
			p.setPen(pal.text);
			p.setFont(QFont("Segoe UI", std::max(7, int(s.height() * 0.09)), QFont::Bold));
			p.drawText(scaled(lay.timer, s), Qt::AlignCenter, "05:00");
		}
	}
	p.end();
	return img;
}

quint32 abgr(const QColor &c)
{
	return (quint32(c.alpha()) << 24) | (quint32(c.blue()) << 16) | (quint32(c.green()) << 8) | quint32(c.red());
}

void addText(ObsCore *core, const QString &name, const QString &text, const QRectF &box, int px, const QColor &color,
	     bool bold)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "text", text.toUtf8().constData());
	obs_data_t *font = obs_data_create();
	// Nirmala UI has Devanagari as well as Latin letters
	obs_data_set_string(font, "face", i18n::language() == "hi" ? "Nirmala UI" : "Segoe UI");
	obs_data_set_int(font, "size", px);
	obs_data_set_string(font, "style", bold ? "Bold" : "Regular");
	obs_data_set_int(font, "flags", bold ? 1 : 0);
	obs_data_set_obj(s, "font", font);
	obs_data_release(font);
	obs_data_set_int(s, "color", abgr(color));
	obs_data_set_string(s, "align", "center");
	core->addSourceAt("text_gdiplus", name, s, box);
	obs_data_release(s);
}

void addWidget(ObsCore *core, const QString &kind, const QString &name, const QRectF &box, const Palette &pal,
	       const std::function<void(obs_data_t *)> &more = {})
{
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "kind", kind.toUtf8().constData());
	obs_data_set_int(s, "width", int(box.width()));
	obs_data_set_int(s, "height", int(box.height()));
	obs_data_set_string(s, "color", pal.accent.name().toUtf8().constData());
	obs_data_set_string(s, "text_color", pal.text.name().toUtf8().constData());
	if (more)
		more(s);
	core->addSourceAt("zunimi_overlay", name, s, box);
	obs_data_release(s);
}
} // namespace

QVector<Style> styles()
{
	return {{"neon", L("Neon")}, {"clean", L("Clean")}, {"sunset", L("Sunset")}};
}

QVector<Kind> kinds()
{
	return {{"starting", L("Starting soon"), L("Countdown, chat and the latest follower while people arrive.")},
		{"brb", L("Be right back"), L("A pause screen with chat, so viewers stay.")},
		{"ending", L("Stream ending"), L("Thanks, and who followed, subbed and tipped.")},
		{"gaming", L("Gaming"), L("Your game with a framed webcam, alerts and the latest follower.")},
		{"chatting", L("Just chatting"), L("A big framed webcam, chat beside it and alerts.")}};
}

QImage preview(const QString &kind, const QString &style, const QSize &size)
{
	return draw(kind, style, size, true);
}

QString apply(ObsCore *core, const QString &kind, const QString &style, QString *error)
{
	if (core->editingVertical()) {
		*error = L("Switch the preview to Landscape first: templates are made for the normal picture.");
		return {};
	}
	const QSize canvas = core->canvas();
	const QSizeF s(canvas);
	const Palette pal = palette(style);
	const Layout lay = layout(kind);
	const double unit = s.height() / 1080.0;

	// the drawn picture is kept with the user's data, like any image they add
	QDir dir(Settings::dataDir() + "/templates");
	dir.mkpath(".");
	const QString file = dir.filePath(QString("%1-%2-%3x%4.png").arg(style, kind).arg(canvas.width()).arg(canvas.height()));
	if (!QFile::exists(file) && !draw(kind, style, canvas, false).save(file)) {
		*error = L("The template picture could not be saved.");
		return {};
	}
	QString name;
	for (const Kind &k : kinds())
		if (k.id == kind)
			name = k.name;
	if (name.isEmpty()) {
		*error = L("That template doesn't exist.");
		return {};
	}
	const QString scene = core->addScene(name);
	const QRectF full(0, 0, s.width(), s.height());
	auto addPicture = [&] {
		obs_data_t *img = obs_data_create();
		obs_data_set_string(img, "file", file.toUtf8().constData());
		core->addSourceAt("image_source", lay.background ? L("Background") : L("Frame"), img, full);
		obs_data_release(img);
	};
	// back to front: a background goes behind everything; the Gaming frame is see-through and goes over the game
	// and the webcam
	if (lay.background)
		addPicture();
	if (lay.game) {
		obs_data_t *g = obs_data_create();
		obs_data_set_string(g, "capture_mode", "any_fullscreen");
		core->addSourceAt("game_capture", L("Game"), g, full);
		obs_data_release(g);
	}
	if (!lay.webcam.isEmpty()) {
		const auto cams = SetupWizard::webcams();
		if (!cams.isEmpty()) {
			obs_data_t *c = obs_data_create();
			obs_data_set_string(c, "video_device_id", cams.first().second.toUtf8().constData());
			const QRectF box = scaled(lay.webcam, s).adjusted(unit * 3, unit * 3, -unit * 3, -unit * 3);
			if (obs_source_t *cam = core->addSourceAt("dshow_input", L("Webcam"), c, box)) {
				// a camera's size is only known once it runs: place it again then
				const QString camName = QString::fromUtf8(obs_source_get_name(cam));
				QTimer::singleShot(2500, core, [core, scene, camName, box] {
					if (core->currentScene() == scene)
						if (obs_sceneitem_t *item = core->item(camName))
							core->place(item, box);
				});
			}
			obs_data_release(c);
		}
	}
	if (!lay.background)
		addPicture();

	if (!lay.title.isEmpty())
		addText(core, L("Title"), titleOf(kind), scaled(lay.title, s), int(110 * unit), pal.text, true);
	if (!lay.subtitle.isEmpty())
		addText(core, L("Subtitle"), subtitleOf(kind), scaled(lay.subtitle, s), int(46 * unit), pal.accent, false);
	if (!lay.timer.isEmpty())
		addWidget(core, "timer", L("Timer"), scaled(lay.timer, s), pal, [unit](obs_data_t *d) {
			obs_data_set_string(d, "mode", "countdown");
			obs_data_set_int(d, "minutes", 5);
			obs_data_set_string(d, "title", "");
			obs_data_set_int(d, "font_size", int(96 * unit));
			obs_data_set_string(d, "done_text", L("Starting now!").toUtf8().constData());
		});
	if (!lay.events.isEmpty())
		addWidget(core, "events", L("Recent events"), scaled(lay.events, s), pal,
			  [unit](obs_data_t *d) { obs_data_set_int(d, "font_size", int(30 * unit)); });
	if (!lay.chat.isEmpty())
		addWidget(core, "chat", L("Chat"), scaled(lay.chat, s).adjusted(unit * 16, unit * 16, -unit * 16, -unit * 16),
			  pal, [unit](obs_data_t *d) { obs_data_set_int(d, "font_size", int(26 * unit)); });
	if (!lay.label.isEmpty())
		addWidget(core, "label", L("Latest follower"), scaled(lay.label, s), pal, [unit](obs_data_t *d) {
			obs_data_set_string(d, "what", "latest_follow");
			obs_data_set_int(d, "font_size", int(36 * unit));
		});
	if (!lay.alerts.isEmpty())
		addWidget(core, "alerts", L("Alerts"), scaled(lay.alerts, s), pal, [](obs_data_t *d) {
			obs_data_set_string(d, "design",
					    QJsonDocument(overlays::defaultDesign()).toJson(QJsonDocument::Compact).constData());
		});
	return scene;
}
} // namespace templates

// ---- the picker

TemplatesDialog::TemplatesDialog(ObsCore *core, QWidget *parent) : QDialog(parent), m_core(core)
{
	setWindowTitle(L("Scene templates"));
	resize(820, 560);
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(20, 18, 20, 16);
	auto *intro = new QLabel(L("Pick a scene and a style. Everything in it can be moved, resized and changed afterwards."));
	intro->setObjectName("hint");
	intro->setWordWrap(true);
	layout->addWidget(intro);
	auto *styleRow = new QHBoxLayout;
	m_styles = new QButtonGroup(this);
	int i = 0;
	for (const templates::Style &st : templates::styles()) {
		auto *b = new QPushButton(st.name);
		b->setObjectName("choice");
		b->setCheckable(true);
		b->setProperty("style", st.id);
		m_styles->addButton(b, i++);
		styleRow->addWidget(b);
	}
	styleRow->addStretch();
	m_styles->button(0)->setChecked(true);
	layout->addLayout(styleRow);
	m_list = new QListWidget;
	m_list->setViewMode(QListView::IconMode);
	m_list->setIconSize(QSize(240, 135));
	m_list->setGridSize(QSize(262, 200));
	m_list->setResizeMode(QListView::Adjust);
	m_list->setMovement(QListView::Static);
	m_list->setWordWrap(true);
	m_list->setSpacing(6);
	layout->addWidget(m_list, 1);
	auto *more = new QLabel(L("More packs from zunimilabs.online"));
	more->setObjectName("h2");
	layout->addWidget(more);
	m_onlineNote = new QLabel;
	m_onlineNote->setObjectName("hint");
	m_onlineNote->setWordWrap(true);
	layout->addWidget(m_onlineNote);
	m_online = new QListWidget;
	m_online->setViewMode(QListView::IconMode);
	m_online->setIconSize(QSize(160, 90));
	m_online->setGridSize(QSize(190, 150));
	m_online->setResizeMode(QListView::Adjust);
	m_online->setMovement(QListView::Static);
	m_online->setWordWrap(true);
	m_online->setMaximumHeight(170);
	layout->addWidget(m_online);
	connect(m_online, &QListWidget::itemDoubleClicked, this,
		[this](QListWidgetItem *item) { getPack(item->data(Qt::UserRole).toJsonObject()); });
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
	auto *add = buttons->addButton(L("Add this scene"), QDialogButtonBox::AcceptRole);
	add->setObjectName("primary");
	layout->addWidget(buttons);
	connect(m_styles, &QButtonGroup::idClicked, this, &TemplatesDialog::refresh);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	auto go = [this] {
		QListWidgetItem *item = m_list->currentItem();
		if (!item)
			return;
		QString error;
		m_added = templates::apply(m_core, item->data(Qt::UserRole).toString(),
					   m_styles->checkedButton()->property("style").toString(), &error);
		if (m_added.isEmpty()) {
			setWindowTitle(error);
			return;
		}
		accept();
	};
	connect(buttons, &QDialogButtonBox::accepted, this, go);
	connect(m_list, &QListWidget::itemDoubleClicked, this, go);
	refresh();
	loadCatalog();
}

void TemplatesDialog::refresh()
{
	const QString style = m_styles->checkedButton()->property("style").toString();
	const int row = std::max(0, m_list->currentRow());
	m_list->clear();
	for (const templates::Kind &k : templates::kinds()) {
		auto *item = new QListWidgetItem(QIcon(QPixmap::fromImage(templates::preview(k.id, style, QSize(480, 270)))),
						 k.name + "\n" + k.description);
		item->setData(Qt::UserRole, k.id);
		item->setToolTip(k.description);
		m_list->addItem(item);
	}
	m_list->setCurrentRow(row);
}

// ---- more packs from zunimilabs.online
// catalog.json: {"packs": [{"id", "name", "description", "preview", "zip", "sha256"}]}. A pack is a zip of a scene
// collection (.json) with its pictures; it is brought in with the same importer as OBS packs, as its own collection.
namespace {
const char *CATALOG_URL = "https://zunimilabs.online/templates/catalog.json";

bool fromOurSite(const QUrl &url)
{
	return url.scheme() == "https" && (url.host() == "zunimilabs.online" || url.host().endsWith(".zunimilabs.online"));
}
} // namespace

void TemplatesDialog::loadCatalog()
{
	m_onlineNote->setText(L("Looking for packs..."));
	QNetworkReply *reply = EventHub::get().network()->get(QNetworkRequest(QUrl(CATALOG_URL)));
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		reply->deleteLater();
		const QJsonArray packs = QJsonDocument::fromJson(reply->readAll()).object().value("packs").toArray();
		if (reply->error() != QNetworkReply::NoError) {
			m_onlineNote->setText(L("The pack list could not be loaded. Check your internet and open this again."));
			return;
		}
		m_online->clear();
		for (const QJsonValue &v : packs) {
			const QJsonObject p = v.toObject();
			if (!fromOurSite(QUrl(p.value("zip").toString())))
				continue;
			auto *item = new QListWidgetItem(p.value("name").toString() + "\n" + p.value("description").toString());
			item->setData(Qt::UserRole, p);
			m_online->addItem(item);
			const QUrl preview(p.value("preview").toString());
			if (fromOurSite(preview)) {
				QNetworkReply *img = EventHub::get().network()->get(QNetworkRequest(preview));
				connect(img, &QNetworkReply::finished, this, [item, img, this] {
					img->deleteLater();
					QImage pic;
					if (pic.loadFromData(img->readAll()) && m_online->row(item) >= 0)
						item->setIcon(QIcon(QPixmap::fromImage(pic.scaled(480, 270, Qt::KeepAspectRatio, Qt::SmoothTransformation))));
				});
			}
		}
		m_onlineNote->setText(m_online->count() ? L("Double-click a pack to bring it in as its own scene collection.")
							: L("No packs yet. New ones show up here by themselves."));
	});
}

void TemplatesDialog::getPack(const QJsonObject &pack)
{
	const QUrl url(pack.value("zip").toString());
	const QString id = pack.value("id").toString();
	static const QRegularExpression safe("^[a-z0-9-]{1,40}$");
	if (!fromOurSite(url) || !safe.match(id).hasMatch())
		return;
	m_onlineNote->setText(QString(L("Downloading %1...")).arg(pack.value("name").toString()));
	QNetworkReply *reply = EventHub::get().network()->get(QNetworkRequest(url));
	connect(reply, &QNetworkReply::finished, this, [this, reply, pack, id] {
		reply->deleteLater();
		QString message;
		if (reply->error() != QNetworkReply::NoError) {
			m_onlineNote->setText(L("The pack did not download correctly, so it was not used. Try again."));
			return;
		}
		templates::installPack(m_core, id, reply->readAll(), pack.value("sha256").toString(), &message);
		m_onlineNote->setText(message);
	});
}

QString templates::installPack(ObsCore *core, const QString &id, const QByteArray &zip, const QString &sha256, QString *message)
{
	static const QRegularExpression safe("^[a-z0-9-]{1,40}$");
	if (!safe.match(id).hasMatch() || sha256.isEmpty() ||
	    QCryptographicHash::hash(zip, QCryptographicHash::Sha256).toHex() != sha256.toLower().toLatin1()) {
		*message = L("The pack did not download correctly, so it was not used. Try again.");
		return {};
	}
	// unpacked next to the user's other data; Windows' own tar reads zip files
	QDir dir(Settings::dataDir() + "/packs/" + id);
	dir.removeRecursively();
	dir.mkpath(".");
	QFile file(dir.filePath("pack.zip"));
	if (!file.open(QIODevice::WriteOnly) || file.write(zip) != zip.size()) {
		*message = L("The pack could not be saved.");
		return {};
	}
	file.close();
	QProcess tar;
	tar.start("tar.exe", {"-xf", dir.filePath("pack.zip"), "-C", dir.absolutePath()});
	const bool unpacked = tar.waitForFinished(60000) && tar.exitCode() == 0;
	QFile::remove(dir.filePath("pack.zip"));
	const QFileInfoList jsons = dir.entryInfoList({"*.json"}, QDir::Files);
	if (!unpacked || jsons.isEmpty()) {
		*message = L("The pack could not be unpacked.");
		return {};
	}
	QStringList notes;
	QString error;
	const QString name = core->importCollection(jsons.first().absoluteFilePath(), dir.absolutePath(), &notes, &error);
	*message = name.isEmpty() ? error : QString(L("\"%1\" is in your scene collections (Menu > Scene collections).")).arg(name);
	return name;
}
