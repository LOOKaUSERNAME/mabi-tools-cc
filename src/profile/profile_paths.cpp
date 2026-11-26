#include "profile_paths.hpp"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <windows.h>
#include <algorithm>
#include <stdexcept>

namespace cc::ui {
namespace {
constexpr auto directory_name = "MabiTools-ChannelChecker";

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void ordinary(const QString& path, bool directory, bool absent_allowed = false) {
    const auto attr = GetFileAttributesW(reinterpret_cast<const wchar_t*>(path.utf16()));
    if (attr == INVALID_FILE_ATTRIBUTES) {
        require(absent_allowed && GetLastError() == ERROR_FILE_NOT_FOUND,
                "Cannot access a profile storage path.");
        return;
    }
    require((attr & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) == 0 &&
                (((attr & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory),
            "Profile storage does not follow links, junctions or device paths.");
}

// Check the entire ancestor chain, not just the leaf: an ordinary-looking
// profile directory can still sit below a junction or redirected root. Callers
// use the verified local path for creation and per-file validation.
QString checked_base(const QString& path) {
    const auto full = QDir::cleanPath(QDir::fromNativeSeparators(path));
    require(!path.isEmpty() && QDir::isAbsolutePath(full) && !full.startsWith("//"),
            "Expected an existing local application data directory.");
    require(!QDir(full).canonicalPath().isEmpty() &&
                QDir(full).canonicalPath().compare(full, Qt::CaseInsensitive) == 0,
            "Profile storage will not resolve aliases or redirected directory roots.");
    QDir parent(full);
    for (;;) {
        ordinary(parent.absolutePath(), true);
        if (parent.isRoot())
            break;
        const auto old = parent.absolutePath();
        require(parent.cdUp() && parent.absolutePath() != old,
                "Cannot verify profile storage ancestors.");
    }
    return full;
}
}

QString open_profile_root() {
    return open_profile_root_under(
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
}

QString open_profile_root_under(const QString& base) {
    const QDir parent(checked_base(base));
    const auto root = parent.filePath(directory_name);
    if (!QFileInfo::exists(root)) {
        ordinary(root, true, true); // Reject even dangling links before attempting creation.
        if (!parent.mkdir(directory_name))
            ordinary(root, true);
    }
    return checked_profile_root(root);
}

QString checked_profile_root(const QString& root) {
    const QFileInfo info(root);
    require(info.fileName() == directory_name, "Refusing a unexpected profile storage root.");
    return checked_base(info.absoluteFilePath());
}

QString checked_profile_file(const QString& root, const QString& filename) {
    require(!filename.isEmpty() && QFileInfo(filename).fileName() == filename && filename != "." &&
                filename != "..",
            "Profile storage expects a single filename.");
    const auto path = QDir(checked_profile_root(root)).filePath(filename);
    ordinary(path, false, true);
    return path;
}
}
