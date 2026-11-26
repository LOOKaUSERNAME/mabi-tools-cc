#include "channel_settings.hpp"
#include "profile_paths.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringConverter>
#include <algorithm>

namespace cc::ui {
namespace {
constexpr qint64 max_csv = 64 * 1024;

QString path_in_fixture(const QString& root) {
    const QFileInfo file(QDir(channel_settings::fixture_root(root)).filePath("channels.csv"));
    if (file.isSymLink() || file.isJunction() || (file.exists() && !file.isFile()))
        throw std::runtime_error("The isolated channels.csv must be a regular file.");
    return file.absoluteFilePath();
}

std::string read_csv(QFile& file) {
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot open channel settings.");
    const auto bytes = file.read(max_csv + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > max_csv)
        throw std::runtime_error("Cannot read bounded channel settings.");
    // WPF File.ReadAllText detects UTF-8/16/32 BOMs. Reject malformed
    // encoding instead of overwriting unreadable input on launch.
    const auto encoding = QStringConverter::encodingForData(bytes).value_or(QStringConverter::Utf8);
    QStringDecoder decoder(encoding, QStringConverter::Flag::Stateless);
    const QString decoded = decoder(bytes);
    if (decoder.hasError())
        throw std::runtime_error("Invalid channel settings encoding.");
    return decoded.toUtf8().toStdString();
}

bool same(const cc::scanning::endpoint& a, const cc::scanning::endpoint& b) {
    return a.number == b.number && a.host == b.host && a.port == b.port;
}
}

QString channel_settings::fixture_root(const QString& path) {
    // Fixture storage requires an explicit disposable direct child of the
    // system temp directory, never LOCALAPPDATA or a default destination.
    const QFileInfo requested(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    const auto temp = QFileInfo(QDir::tempPath()).canonicalFilePath();
    const auto prefix = QStringLiteral("mabitools-cc-settings-");
    const auto name = requested.fileName();
#ifdef Q_OS_WIN
    constexpr auto path_case = Qt::CaseInsensitive;
#else
    constexpr auto path_case = Qt::CaseSensitive;
#endif
    if (path.isEmpty() || temp.isEmpty() || !requested.isDir() || requested.isSymLink() ||
        requested.isJunction() || !name.startsWith(prefix) || name == prefix ||
        QDir(requested.absolutePath()).canonicalPath().compare(temp, path_case) != 0 ||
        requested.canonicalFilePath().compare(QDir(temp).filePath(name), path_case) != 0)
        throw std::invalid_argument(
            "Use a disposable mabitools-cc-settings-* folder directly under the system temp directory.");
    return requested.canonicalFilePath();
}

std::vector<cc::scanning::endpoint> channel_settings::bundled() {
    QFile file(":/erinn-na.csv");
    return cc::catalog::parse(read_csv(file));
}

std::vector<cc::scanning::endpoint> channel_settings::load(const QString& fixture,
                                                           QString& warning) {
    warning.clear();
    auto defaults = bundled();
    try {
        QFile file(path_in_fixture(fixture));
        if (!file.exists())
            return defaults;
        auto saved = cc::catalog::parse(read_csv(file));
        if (saved.size() == 13 && defaults.size() >= 13 &&
            std::equal(saved.begin(), saved.end(), defaults.begin(), same))
            return defaults; // WPF upgrades the exact legacy preset in memory.
        return saved;
    } catch (const std::exception& error) {
        warning = "Saved channel list could not be loaded. Using the bundled list. " +
                  QString::fromUtf8(error.what());
        return defaults;
    }
}

std::vector<cc::scanning::endpoint> channel_settings::load_profile(const QString& root) {
    QFile file(checked_profile_file(root, "channels.csv"));
    if (!file.exists())
        return {};
    return cc::catalog::parse(read_csv(file));
}

void channel_settings::save_profile(const QString& root,
                                    const std::vector<cc::scanning::endpoint>& endpoints) {
    const auto bytes = QByteArray::fromStdString(cc::catalog::format(endpoints));
    QSaveFile file(checked_profile_file(root, "channels.csv"));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("Could not atomically save the channel list.");
}

void channel_settings::save(const QString& fixture,
                            const std::vector<cc::scanning::endpoint>& endpoints) {
    const auto bytes = QByteArray::fromStdString(cc::catalog::format(endpoints));
    QSaveFile file(path_in_fixture(fixture));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error(
            "Could not save the isolated channel list. The active list was not changed.");
}
}
