#include "scan_config.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace cc::ui {
void scan_config::validate() const {
    cc::scanning::validate(targets, settings);
    if (settings.samples != 10 && settings.samples != 20 && settings.samples != 40 &&
        settings.samples != 60 && settings.samples != 120)
        throw std::invalid_argument(
            "Choose one of the existing UI sample counts: 10, 20, 40, 60 or 120.");
}

scan_config scan_config::read_fixture(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536)
        throw std::invalid_argument("Cannot read bounded loopback test configuration.");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw std::invalid_argument("Expected a loopback test configuration object.");
    const auto object = document.object();
    const auto endpoints = object["Endpoints"].toArray();
    if (endpoints.empty() || endpoints.size() > 30)
        throw std::invalid_argument("Choose 1-30 loopback targets.");
    scan_config result;
    for (const auto& item : endpoints) {
        const auto endpoint = item.toObject();
        result.targets.push_back({endpoint["Channel"].toInt(),
                                  endpoint["Host"].toString().toStdString(),
                                  endpoint["Port"].toInt()});
    }
    if (object.contains("Options")) {
        if (!object["Options"].isObject())
            throw std::invalid_argument("Options must be an object.");
        const auto options = object["Options"].toObject();
        // Absent options use WPF defaults. Malformed present values must not fall
        // back to a permissive default or silently change the requested scan.
        const auto read = [&](const char* name, int fallback) {
            return options.contains(name) ? options[name].toInt(-1) : fallback;
        };
        result.settings = {read("Samples", 40), read("TimeoutMs", 1500), read("IntervalMs", 1000),
                           read("MaxConcurrency", 4)};
    }
    result.validate();
    return result;
}
}
