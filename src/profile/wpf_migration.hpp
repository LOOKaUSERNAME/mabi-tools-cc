#pragma once

#include <QString>

class QWidget;

namespace cc::ui {
// Explicitly imports a sealed local rehearsal snapshot, never a live WPF
// profile. Legacy scan files stay in an archive outside native history.
qsizetype import_wpf_snapshot(const QString& snapshot, const QString& app_data);
QString imported_profile_root(const QString& app_data);
QString wpf_archive_path(const QString& root);
void show_wpf_archive(QWidget* parent, const QString& root);
void verify_wpf_migration(const QString& disposable_app_data);
}
