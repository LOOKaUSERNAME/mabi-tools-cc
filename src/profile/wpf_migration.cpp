// Explicit read-only import of the sealed legacy snapshot format. Validate its
// proof and hashes, copy into a temporary sibling of the separate import profile,
// then recheck sources before publishing without replacement. Original reports
// remain archived bytes and never enter native scan recommendations.

#include "wpf_migration.hpp"
#include "profile_paths.hpp"
#include "channel_settings.hpp"

#include <QCryptographicHash>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMap>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <stdexcept>

namespace cc::ui {
namespace {
constexpr auto parent_name = "MabiTools-CC-Legacy-Import";
constexpr auto archive_name = "wpf-history-archive";
constexpr auto profile_directory_name = "MabiTools-ChannelChecker";
constexpr qint64 record_limit = 4 * 1024 * 1024;
constexpr qint64 settings_limit = 128 * 1024;
constexpr qint64 total_limit = 128 * 1024 * 1024;
constexpr qsizetype count_limit = 1200;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

bool exists(const QString& path) {
    const QFileInfo item(path);
    return item.exists() || item.isSymLink() || item.isJunction();
}

QString checked(const QString& path, bool dir) {
    const auto full = QDir::cleanPath(QDir::fromNativeSeparators(path));
    require(QDir::isAbsolutePath(full) && !full.startsWith("//"),
            "Migration requires a local path.");
    for (auto current = full; !current.isEmpty();) {
        const QFileInfo item(current);
        require(!item.isSymLink() && !item.isJunction() &&
                    (current == full && !dir ? item.isFile() : item.isDir())
#ifdef Q_OS_WIN
                    && item.canonicalFilePath().compare(current, Qt::CaseInsensitive) == 0
#else
                    && item.canonicalFilePath() == current
#endif
                ,
                "Migration refuses missing or redirected files and directories.");
        if (QDir(current).isRoot())
            break;
        const auto parent = item.absolutePath();
        require(parent != current, "Cannot verify migration path ancestors.");
        current = parent;
    }
    return full;
}

QString make_directory(const QString& parent, const QString& name) {
    const QDir folder(checked(parent, true));
    const auto path = folder.filePath(name);
    if (!exists(path))
        require(folder.mkdir(name), "Could not create migration directory.");
    return checked(path, true);
}

QByteArray read(const QString& file_path, qint64 limit) {
    QFile file(checked(file_path, false));
    require(file.open(QIODevice::ReadOnly), "Could not read a migration file.");
    const auto bytes = file.read(limit + 1);
    require(bytes.size() <= limit && file.error() == QFileDevice::NoError,
            "Migration file exceeded its read limit or could not be read.");
    return bytes;
}

void write(const QString& path, const QByteArray& bytes) {
    require(!exists(path), "Migration refuses to replace a file.");
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(),
            "Could not write migration staging file.");
}

QStringList scans(const QString& root) {
    QStringList names;
    static const QRegularExpression pattern(R"(\Ascan-[a-f0-9]{32}\.json\z)");
    const auto folder = QDir(checked(root, true));
    for (const auto& name :
         folder.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
        if (pattern.match(name).hasMatch())
            names.append(name);
        require(names.size() <= count_limit, "Too many scan records for migration.");
    }
    return names;
}

QStringList contents(const QString& source) {
    const QDir root(checked(source, true));
    QStringList names;
    for (const auto& name :
         {QStringLiteral("channels.csv"), QStringLiteral("history-preferences.json")})
        if (exists(root.filePath(name)))
            names.append(name);
    const auto folder = root.filePath("history");
    if (exists(folder))
        for (const auto& name : scans(folder))
            names.append("history/" + name);
    return names;
}

struct verified_source {
    QString path;
    qsizetype expected_records;
};

verified_source snapshot_location(const QString& requested, const QString& app_data) {
    const QDir local(checked(app_data, true));
    const auto snapshot = checked(requested, true);
    const QFileInfo source(snapshot);
    const QFileInfo run(source.absolutePath());
    static const QRegularExpression run_name(R"(\Ar-[a-f0-9]{32}\z)");
    require(source.fileName() == "source-snapshot" && run_name.match(run.fileName()).hasMatch() &&
                QDir(run.absolutePath()) == QDir(local.filePath("MabiTools-CC-Rehearsals")),
            "Select the source-snapshot folder from a completed Channel Checker rehearsal.");
    const auto proof =
        QJsonDocument::fromJson(
            read(QDir(run.absoluteFilePath()).filePath("readiness-result.json"), settings_limit))
            .object()
            .value("profile")
            .toObject();
    require(proof.value("SourceUnchanged").toBool() && proof.value("SnapshotUnchanged").toBool(),
            "Rehearsal did not confirm an unchanged source and snapshot.");
    const auto expected = proof.value("ArchivedScans").toInt(-1);
    require(expected >= 0 && expected <= count_limit,
            "Rehearsal did not record a valid scan count.");
    return {snapshot, expected};
}

QJsonObject validated_preferences(const QByteArray& bytes) {
    QJsonParseError error;
    const auto obj = QJsonDocument::fromJson(bytes, &error).object();
    require(error.error == QJsonParseError::NoError && obj.value("SchemaVersion").toInt() == 1 &&
                obj.value("SaveHistory").isBool() && obj.value("AutomaticMaintenance").isBool(),
            "Unrecognised WPF history preferences; no settings were imported.");
    return obj;
}

bool same(const cc::scanning::endpoint& first, const cc::scanning::endpoint& second) {
    return first.number == second.number && first.host == second.host && first.port == second.port;
}

QString opened_scan(const QString& archive, const QString& name) {
    return QString::fromUtf8(read(QDir(archive).filePath(name), record_limit));
}
}

qsizetype import_wpf_snapshot(const QString& snapshot, const QString& app_data) {
    const auto verified = snapshot_location(snapshot, app_data);
    const QDir source(verified.path);
    const auto parent = make_directory(app_data, parent_name);
    const auto destination = QDir(parent).filePath(profile_directory_name);
    require(!exists(destination),
            "An imported profile already exists. No existing profile was changed.");
    // Same-volume staging keeps the imported profile unavailable until every
    // copy and setting is verified. No write or lock touches the WPF source.
    QTemporaryDir stage(QDir(parent).filePath("cc-import-XXXXXX"));
    require(stage.isValid(), "Could not stage the imported profile.");
    const auto staged = open_profile_root_under(stage.path());
    const auto archive = make_directory(staged, archive_name);
    const auto archive_scans = make_directory(archive, "history");
    const auto files = contents(source.path());
    QMap<QString, QByteArray> hashes;
    qint64 remaining = total_limit;
    qsizetype records = 0;
    bool saving = true;
    bool automatic = true;
    for (const auto& relative : files) {
        const bool scan = relative.startsWith("history/");
        const auto bytes = read(source.filePath(relative),
                                std::min(remaining, scan ? record_limit : settings_limit));
        remaining -= bytes.size();
        hashes.insert(relative, QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
        const auto path = scan ? QDir(archive_scans).filePath(QFileInfo(relative).fileName())
                               : QDir(archive).filePath(relative);
        write(path, bytes);
        require(QCryptographicHash::hash(read(path, scan ? record_limit : settings_limit),
                                         QCryptographicHash::Sha256) == hashes.value(relative),
                "Archived file did not match the original bytes.");
        if (scan)
            ++records;
        else if (relative == "channels.csv") {
            write(QDir(staged).filePath(relative), bytes);
            const auto selected = channel_settings::load_profile(staged);
            const auto bundled = channel_settings::bundled();
            require(
                !selected.empty() &&
                    std::all_of(selected.begin(), selected.end(),
                                [&](const auto& row) {
                                    return std::any_of(
                                        bundled.begin(), bundled.end(),
                                        [&](const auto& original) { return same(row, original); });
                                }),
                "The WPF list contains a destination outside the bundled channel list. No imported profile was created.");
        } else if (relative == "history-preferences.json") {
            const auto prefs = validated_preferences(bytes);
            saving = prefs.value("SaveHistory").toBool();
            automatic = prefs.value("AutomaticMaintenance").toBool();
        }
    }
    require(records == verified.expected_records,
            "Snapshot scan count differs from the rehearsal result.");
    const QJsonObject native_prefs{{"Format", "MabiTools.Qt.CandidateHistory.1"},
                                   {"Saving", saving},
                                   {"InvalidatedUtcMs", -1},
                                   {"Epoch", QUuid::createUuid().toString(QUuid::Id128)}};
    write(QDir(staged).filePath("native-history-preferences.json"),
          QJsonDocument(native_prefs).toJson(QJsonDocument::Compact));
    write(QDir(staged).filePath("automatic-maintenance.txt"), automatic ? "on\n" : "off\n");
    // Re-read the source after copying. A changed file set or digest invalidates
    // the staged import; publish only after the complete snapshot remains stable.
    require(files == contents(source.path()), "Snapshot file set changed during migration.");
    for (const auto& relative : files)
        require(QCryptographicHash::hash(
                    read(source.filePath(relative),
                         relative.startsWith("history/") ? record_limit : settings_limit),
                    QCryptographicHash::Sha256) == hashes.value(relative),
                "Snapshot bytes changed during migration.");
    require(!exists(destination) && QDir(parent).rename(staged, destination),
            "Could not create the imported profile without replacing existing data.");
    return records;
}

QString imported_profile_root(const QString& app_data) {
    const auto parent = checked(QDir(app_data).filePath(parent_name), true);
    const auto root = checked_profile_root(QDir(parent).filePath(profile_directory_name));
    (void)checked(wpf_archive_path(root), true);
    return root;
}

QString wpf_archive_path(const QString& root) {
    return QDir(checked_profile_root(root)).filePath(archive_name);
}

void show_wpf_archive(QWidget* parent, const QString& root) {
    const auto archive = checked(QDir(wpf_archive_path(root)).filePath("history"), true);
    QDialog dialog(parent);
    dialog.setWindowTitle("Imported WPF history");
    auto* layout = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(
        "Original reports, kept separately from current scans and recommendations.", &dialog);
    layout->addWidget(explanation);
    auto* list = new QListWidget(&dialog);
    layout->addWidget(list, 1);
    auto* report = new QPlainTextEdit(&dialog);
    report->setReadOnly(true);
    report->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(report, 2);
    for (const auto& name : scans(archive)) {
        auto* item = new QListWidgetItem(name, list);
        item->setData(Qt::UserRole, name);
    }
    QObject::connect(list, &QListWidget::currentItemChanged, &dialog,
                     [archive, report](QListWidgetItem* item, QListWidgetItem*) {
                         if (!item) {
                             report->clear();
                             return;
                         }
                         try {
                             report->setPlainText(
                                 opened_scan(archive, item->data(Qt::UserRole).toString()));
                         } catch (const std::exception&) {
                             report->setPlainText("This original report could not be opened.");
                         }
                     });
    if (list->count())
        list->setCurrentRow(0);
    else
        report->setPlainText("No WPF scans were saved in this snapshot.");
    dialog.resize(660, 480);
    dialog.exec();
}

void verify_wpf_migration(const QString& app_data) {
    const QDir local(app_data);
    const auto ordinary_profile = open_profile_root_under(app_data);
    const auto sentinel = QDir(ordinary_profile).filePath("ordinary-profile.keep");
    write(sentinel, "existing application profile");
    const auto rehearsal = make_directory(app_data, "MabiTools-CC-Rehearsals");
    const auto run = make_directory(rehearsal, "r-0123456789abcdef0123456789abcdef");
    const auto snapshot = make_directory(run, "source-snapshot");
    const auto history = make_directory(snapshot, "history");
    const auto name = QStringLiteral("scan-11111111111111111111111111111111.json");
    const QByteArray bytes = R"({"SchemaVersion":999,"Unknown":"Keep original bytes."})";
    write(QDir(history).filePath(name), bytes);
    write(QDir(run).filePath("readiness-result.json"),
          R"({"profile":{"SourceUnchanged":true,"SnapshotUnchanged":true,"ArchivedScans":1}})");
    write(
        QDir(snapshot).filePath("history-preferences.json"),
        R"({"SchemaVersion":1,"SaveHistory":false,"AutomaticMaintenance":false,"RetentionDays":90})");
    const auto list = channel_settings::bundled();
    require(list.size() == 16, "Fixture bundled list missing.");
    write(QDir(snapshot).filePath("channels.csv"), "1,127.0.0.1,40123\n");
    const auto rejects = [](const auto& fn) {
        try {
            fn();
        } catch (const std::runtime_error&) {
            return;
        }
        throw std::runtime_error("Expected a blocked migration.");
    };
    rejects([&] { (void)import_wpf_snapshot(snapshot, app_data); });
    require(!exists(QDir(local.filePath(parent_name)).filePath(profile_directory_name)),
            "Failed import published a profile.");
    require(QFile::remove(QDir(snapshot).filePath("channels.csv")), "Could not reset test input.");
    write(QDir(snapshot).filePath("channels.csv"),
          QByteArray::fromStdString(cc::catalog::format({list.back(), list.front()})));
    require(import_wpf_snapshot(snapshot, app_data) == 1, "Expected one archived scan.");
    const auto imported = imported_profile_root(app_data);
    const auto selected = channel_settings::load_profile(imported);
    require(selected.size() == 2 && same(selected[0], list.back()) &&
                same(selected[1], list.front()),
            "Imported list order changed.");
    const auto archived = QDir(QDir(wpf_archive_path(imported)).filePath("history")).filePath(name);
    require(read(archived, record_limit) == bytes &&
                read(QDir(history).filePath(name), record_limit) == bytes &&
                !exists(QDir(imported).filePath("native-history")),
            "Imported scan was modified or exposed as native history.");
    require(!QJsonDocument::fromJson(
                 read(QDir(imported).filePath("native-history-preferences.json"), settings_limit))
                    .object()
                    .value("Saving")
                    .toBool(true) &&
                read(QDir(imported).filePath("automatic-maintenance.txt"), settings_limit) ==
                    "off\n",
            "WPF Off settings did not survive.");
    rejects([&] { (void)import_wpf_snapshot(snapshot, app_data); });
    require(read(archived, record_limit) == bytes &&
                read(sentinel, settings_limit) == "existing application profile",
            "Import or refusal changed an existing application profile.");
}
}
