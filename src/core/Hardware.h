#pragma once
#include <QSize>
#include <QString>

// What this PC can do, and the settings that fit it. Auto-Tune picks a profile on first launch;
// the person can change it any time in Settings, and nothing is ever locked by profile.
struct Hardware {
	int cores = 1;
	double ramGb = 0;
	QString gpu;
	bool nvenc = false; // NVIDIA hardware encoder
	bool amf = false;   // AMD hardware encoder
	bool qsv = false;   // Intel QuickSync
	bool anyHardwareEncoder() const { return nvenc || amf || qsv; }
};

enum class Profile { Potato, Balanced, Ultra };

struct VideoPlan {
	int width = 1280;
	int height = 720;
	int fps = 30;
	int bitrate = 2500;      // kbps
	QString encoder;         // libobs encoder id
};

Hardware probeHardware(); // encoder checks need the encoder modules loaded first
Profile pickProfile(const Hardware &hw);
VideoPlan planFor(Profile profile, const Hardware &hw);
QString bestEncoder(const Hardware &hw);
QString encoderLabel(const QString &id);
// The family's encoder for a codec ("h264", "hevc", "av1"); the H.264 one when this PC can't do that codec.
QString encoderFor(const QString &family, const QString &codec);
bool codecAvailable(const QString &family, const QString &codec);
QString codecLabel(const QString &codec);
QString profileName(Profile profile);
QString profileKey(Profile profile);
Profile profileFromKey(const QString &key);
