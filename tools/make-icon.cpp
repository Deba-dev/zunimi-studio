// Renders resources/logo.svg into the app icon (zunimi.ico, every Windows size) and the PNGs the app shows.
// Run after changing the logo:  scripts\build.ps1 -Icon
#include <QBuffer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <QtEndian>

#include <cstdio>

static QByteArray png(QSvgRenderer &svg, int size)
{
	QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	QPainter p(&image);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	svg.render(&p);
	p.end();
	QByteArray out;
	QBuffer buffer(&out);
	buffer.open(QIODevice::WriteOnly);
	image.save(&buffer, "PNG");
	return out;
}

int main(int argc, char *argv[])
{
	QGuiApplication app(argc, argv);
	if (argc < 3) {
		std::fprintf(stderr, "usage: make-icon logo.svg out-folder\n");
		return 2;
	}
	QSvgRenderer svg(QString::fromLocal8Bit(argv[1]));
	if (!svg.isValid()) {
		std::fprintf(stderr, "could not read %s\n", argv[1]);
		return 1;
	}
	const QString dir = QString::fromLocal8Bit(argv[2]);

	// ICO with PNG-compressed images (Windows Vista and later read these at every size).
	const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 128, 256};
	QList<QByteArray> images;
	for (int s : sizes)
		images << png(svg, s);
	QByteArray ico;
	auto u16 = [&ico](quint16 v) { ico.append(char(v & 0xff)).append(char(v >> 8)); };
	auto u32 = [&ico](quint32 v) {
		for (int i = 0; i < 4; i++)
			ico.append(char((v >> (8 * i)) & 0xff));
	};
	u16(0);
	u16(1);
	u16(quint16(images.size()));
	quint32 offset = 6 + 16 * quint32(images.size());
	for (int i = 0; i < images.size(); i++) {
		ico.append(char(sizes[i] >= 256 ? 0 : sizes[i])).append(char(sizes[i] >= 256 ? 0 : sizes[i]));
		ico.append(char(0)).append(char(0));
		u16(1);
		u16(32);
		u32(quint32(images[i].size()));
		u32(offset);
		offset += quint32(images[i].size());
	}
	for (const QByteArray &image : images)
		ico.append(image);
	QFile out(dir + "/zunimi.ico");
	if (!out.open(QIODevice::WriteOnly) || out.write(ico) != ico.size())
		return 1;
	out.close();

	for (int s : {32, 64, 256}) {
		QFile f(QString("%1/logo-%2.png").arg(dir).arg(s));
		if (!f.open(QIODevice::WriteOnly) || f.write(png(svg, s)) < 0)
			return 1;
	}
	std::printf("wrote zunimi.ico and logo PNGs to %s\n", argv[2]);
	return 0;
}
