// Scene collections (whole sets of scenes, one file each), profiles (stream, video and recording settings),
// and importing scene collections made in OBS Studio or shipped with overlay packs.
#include "core/ObsCore.h"
#include "core/I18n.h"

#include "core/Settings.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace {
// Settings that belong to a profile. Everything else (window, hotkeys, audio devices) is shared.
const QStringList PROFILE_GROUPS = {"stream/", "video/", "record/", "replay/"};

bool validName(const QString &name)
{
	static const QRegularExpression bad("[\\\\/:*?\"<>|]");
	return !name.trimmed().isEmpty() && !name.contains(bad) && name.size() <= 60;
}

// Fixes file paths in an imported collection: a pack made on someone else's PC points at their folders.
struct PathFixer {
	QStringList roots;
	QHash<QString, QString> found; // lower-case file name -> path on this PC
	bool indexed = false;
	int fixed = 0, missing = 0;

	void index()
	{
		if (indexed)
			return;
		indexed = true;
		for (const QString &root : roots) {
			QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
			int budget = 20000; // a pack folder, not the whole disk
			while (it.hasNext() && budget-- > 0) {
				const QString path = it.next();
				const QString key = QFileInfo(path).fileName().toLower();
				if (!found.contains(key))
					found.insert(key, path);
			}
		}
	}

	static bool looksLikeFile(const QString &s)
	{
		static const QRegularExpression path("^([A-Za-z]:[\\\\/]|\\\\\\\\|/).+\\.[A-Za-z0-9]{2,5}$");
		return path.match(s).hasMatch();
	}

	QJsonValue fix(const QJsonValue &v)
	{
		if (v.isObject()) {
			QJsonObject o = v.toObject();
			for (auto it = o.begin(); it != o.end(); ++it)
				it.value() = fix(it.value());
			return o;
		}
		if (v.isArray()) {
			QJsonArray a = v.toArray();
			for (int i = 0; i < a.size(); i++)
				a[i] = fix(a[i]);
			return a;
		}
		if (!v.isString())
			return v;
		const QString s = v.toString();
		if (!looksLikeFile(s) || QFileInfo::exists(s))
			return v;
		index();
		const QString name = QFileInfo(QString(s).replace('\\', '/')).fileName().toLower();
		if (found.contains(name)) {
			fixed++;
			return QDir::fromNativeSeparators(found.value(name));
		}
		missing++;
		return v;
	}
};
} // namespace

QStringList ObsCore::collections() const
{
	QStringList names;
	for (const QFileInfo &f : QDir(Settings::dataDir() + "/collections").entryInfoList({"*.json"}, QDir::Files, QDir::Name))
		names << f.completeBaseName();
	if (!names.contains(m_collection) && !m_collection.isEmpty())
		names << m_collection;
	names.sort(Qt::CaseInsensitive);
	return names;
}

bool ObsCore::switchCollection(const QString &name, QString *error)
{
	if (name == m_collection)
		return true;
	if (!QFile::exists(collectionPath(name))) {
		*error = L("That scene collection is missing.");
		return false;
	}
	if (streaming() || recording()) {
		// switching rebuilds every source; a game capture or webcam would blink on stream
		*error = L("Stop the stream and recording first: switching rebuilds every source.");
		return false;
	}
	save();
	clearScenes();
	m_collection = name;
	Settings::get().set("scenes/collection", name);
	loadScenes(collectionPath(name));
	save();
	emit collectionChanged();
	emit scenesChanged();
	emit sceneChanged();
	return true;
}

bool ObsCore::newCollection(const QString &name, bool copyCurrent, QString *error)
{
	const QString n = name.trimmed();
	if (!validName(n) || collections().contains(n, Qt::CaseInsensitive)) {
		*error = L("Pick a name that isn't used yet (no \\ / : * ? \" < > |).");
		return false;
	}
	save();
	if (copyCurrent) {
		if (!QFile::copy(collectionPath(m_collection), collectionPath(n))) {
			*error = L("The copy could not be written.");
			return false;
		}
	} else {
		QFile f(collectionPath(n));
		if (!f.open(QIODevice::WriteOnly)) {
			*error = L("The new collection could not be written.");
			return false;
		}
		f.write(QJsonDocument(QJsonObject{{"name", n}, {"sources", QJsonArray()}}).toJson());
	}
	return switchCollection(n, error);
}

bool ObsCore::renameCollection(const QString &from, const QString &to, QString *error)
{
	const QString n = to.trimmed();
	if (!validName(n) || (collections().contains(n, Qt::CaseInsensitive) && n.compare(from, Qt::CaseInsensitive))) {
		*error = L("Pick a name that isn't used yet.");
		return false;
	}
	if (from == m_collection)
		save();
	if (!QFile::rename(collectionPath(from), collectionPath(n))) {
		*error = L("It could not be renamed.");
		return false;
	}
	QFile::remove(collectionPath(from) + ".bak");
	if (from == m_collection) {
		m_collection = n;
		Settings::get().set("scenes/collection", n);
		save();
	}
	emit collectionChanged();
	return true;
}

bool ObsCore::removeCollection(const QString &name, QString *error)
{
	if (collections().size() <= 1) {
		*error = L("One scene collection is always kept.");
		return false;
	}
	if (name == m_collection) {
		const QStringList all = collections();
		const QString other = all.first() == name ? all[1] : all.first();
		if (!switchCollection(other, error))
			return false;
	}
	QFile::remove(collectionPath(name));
	QFile::remove(collectionPath(name) + ".bak");
	emit collectionChanged();
	return true;
}

QString ObsCore::importCollection(const QString &jsonPath, const QString &searchFolder, QStringList *notes, QString *error)
{
	QFile file(jsonPath);
	if (!file.open(QIODevice::ReadOnly)) {
		*error = L("That file could not be opened.");
		return {};
	}
	QJsonParseError parse{};
	const QJsonObject in = QJsonDocument::fromJson(file.readAll(), &parse).object();
	if (parse.error != QJsonParseError::NoError || !in.contains("sources")) {
		*error = L("That isn't a scene collection. Look for the .json file that came with the OBS version of the pack.");
		return {};
	}

	// OBS keeps groups apart from other sources; both load the same way.
	QJsonArray sources = in.value("groups").toArray();
	for (const QJsonValue &v : in.value("sources").toArray())
		sources.append(v);

	PathFixer fixer;
	fixer.roots << QFileInfo(jsonPath).absolutePath();
	if (!searchFolder.isEmpty())
		fixer.roots << searchFolder;
	QStringList unsupported;
	QJsonArray kept;
	for (const QJsonValue &v : sources) {
		QJsonObject s = v.toObject();
		const QString name = s.value("name").toString();
		if (name == "Desktop audio" || name == "Microphone") // ours; theirs gets a new name
			s["name"] = name + L(" (imported)");
		s["settings"] = fixer.fix(s.value("settings"));
		const QString id = s.value("id").toString();
		if (obs_get_source_output_flags(id.toUtf8().constData()) == 0 && !unsupported.contains(id))
			unsupported << id;
		kept.append(s);
	}

	QString base = in.value("name").toString().trimmed();
	if (!validName(base))
		base = QFileInfo(jsonPath).completeBaseName();
	if (!validName(base))
		base = L("Imported");
	QString name = base;
	for (int i = 2; collections().contains(name, Qt::CaseInsensitive); i++)
		name = QString("%1 %2").arg(base).arg(i);

	QJsonObject out;
	out["name"] = name;
	out["sources"] = kept;
	out["scene_order"] = in.value("scene_order");
	out["current_scene"] = in.value("current_scene");
	out["current_program_scene"] = in.value("current_program_scene");
	QFile target(collectionPath(name));
	if (!target.open(QIODevice::WriteOnly)) {
		*error = L("The imported collection could not be written.");
		return {};
	}
	target.write(QJsonDocument(out).toJson());
	target.close();

	if (notes) {
		notes->append(QString(L("%1 scenes and sources brought in.")).arg(kept.size()));
		if (fixer.fixed)
			notes->append(QString(L("%1 file paths pointed at another PC and were found here.")).arg(fixer.fixed));
		if (fixer.missing)
			notes->append(QString(L("%1 files could not be found. Keep the pack's folder where it is, or pick it "
					      "as the search folder and import again."))
					      .arg(fixer.missing));
		for (const QString &id : unsupported) {
			if (id == "browser_source")
				notes->append(L("Browser sources (alerts and widgets from websites) are kept and start working "
					      "once the browser source is switched on."));
			else
				notes->append(L("Kept but not shown yet (needs an OBS plugin Zunimi doesn't have): ") + id);
		}
		notes->append(L("Positions were made for the pack's canvas. If things look misplaced, match its "
			      "resolution in Settings > Video (most packs use 1920 x 1080)."));
	}
	QString switchError;
	if (!switchCollection(name, &switchError) && notes)
		notes->append(L("Saved as \"") + name + L("\". Switch to it when you're not live: ") + switchError);
	return name;
}

// ---- profiles
QStringList ObsCore::profiles() const
{
	QStringList names{"Default"};
	for (const QString &k : Settings::get().keys("profiles/"))
		if (k.count('/') >= 2) {
			const QString n = k.section('/', 1, 1);
			if (!names.contains(n))
				names << n;
		}
	const QString current = profile();
	if (!names.contains(current))
		names << current;
	return names;
}

QString ObsCore::profile() const
{
	return Settings::get().value("profile/current", "Default").toString();
}

namespace {
void storeProfile(const QString &name)
{
	Settings &st = Settings::get();
	for (const QString &k : st.keys("profiles/" + name + "/"))
		st.remove(k);
	for (const QString &group : PROFILE_GROUPS)
		for (const QString &k : st.keys(group))
			st.set("profiles/" + name + "/" + k, st.value(k));
}
} // namespace

bool ObsCore::switchProfile(const QString &name, QString *error)
{
	if (name == profile())
		return true;
	if (busy()) {
		*error = L("Stop the stream, recording, replay buffer and virtual camera first.");
		return false;
	}
	Settings &st = Settings::get();
	storeProfile(profile());
	const QStringList stored = st.keys("profiles/" + name + "/");
	if (!stored.isEmpty()) { // a brand-new profile starts as a copy of the current one
		for (const QString &group : PROFILE_GROUPS)
			for (const QString &k : st.keys(group))
				st.remove(k);
		for (const QString &k : stored)
			st.set(k.mid(QString("profiles/" + name + "/").size()), st.value(k));
	}
	st.set("profile/current", name);
	VideoPlan plan;
	plan.width = st.value("video/width", m_plan.width).toInt();
	plan.height = st.value("video/height", m_plan.height).toInt();
	plan.fps = st.value("video/fps", m_plan.fps).toInt();
	plan.bitrate = st.value("video/bitrate", m_plan.bitrate).toInt();
	plan.encoder = st.value("video/encoder", m_plan.encoder).toString();
	return applyPlan(plan, error);
}

bool ObsCore::newProfile(const QString &name, QString *error)
{
	const QString n = name.trimmed();
	if (!validName(n) || profiles().contains(n, Qt::CaseInsensitive)) {
		*error = L("Pick a name that isn't used yet.");
		return false;
	}
	storeProfile(profile());
	storeProfile(n); // same settings to begin with
	return switchProfile(n, error);
}

bool ObsCore::removeProfile(const QString &name, QString *error)
{
	if (name == "Default") {
		*error = L("The Default profile is always kept.");
		return false;
	}
	if (name == profile() && !switchProfile("Default", error))
		return false;
	Settings &st = Settings::get();
	for (const QString &k : st.keys("profiles/" + name + "/"))
		st.remove(k);
	return true;
}
