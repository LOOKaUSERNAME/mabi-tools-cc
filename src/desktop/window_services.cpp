// Bridges the Qt window to its service coordinator. The caller supplies the
// transport: the application uses the restricted service policy, while tests use
// local fixtures. The window borrows the coordinator for its lifetime.
#include "main_window.hpp"
#include "service_flow.hpp"
#include <QLabel>
#include <QTimer>
#include <stdexcept>
#include <QDesktopServices>
#include <QMessageBox>
#include <QUrl>
#ifdef CC_PROFILE_HISTORY
#include "profile_paths.hpp"
#include <QFile>
#include <QSaveFile>
#endif

namespace cc::ui {
void main_window::attach_window_services(cc::service_flow::coordinator& flow) {
    if (window_services_ || !scan_config_
#ifndef CC_PROFILE_HISTORY
        || history_ || !profile_root_.isEmpty()
#endif
    )
        throw std::logic_error("Services require a fresh window with a scan configuration.");
    window_services_ = &flow;
#ifdef CC_PROFILE_HISTORY
    setWindowTitle("Channel Checker");
    title_->setText("MabiTools - Channel Checker");
    if (auto* summary = page_->findChild<QLabel*>("EndpointSummary"))
        summary->setText("Bundled Erinn channels");
#else
    setWindowTitle("Channel Checker | Service fixture");
#endif
    service_poll_ = new QTimer(this);
    QObject::connect(service_poll_, &QTimer::timeout, this, [this] {
#ifdef CC_PROFILE_HISTORY
        if (profile_settings_ && history_ && !closing_) {
            const auto history = history_->view();
            window_services_->maintenance_context(
                automatic_maintenance_, history.loaded && (history.saving || history.count > 0));
        }
#endif
        if (closing_) {
            if (window_services_->closed() && !scan_running_)
                close();
        } else if (window_services_->scan() == cc::service_flow::scan_status::ready) {
            start_scan();
        } else {
            render_window_services();
            const auto update = window_services_->updates();
            if (manual_update_pending_ &&
                (update == cc::service_flow::update_status::failed ||
                 update == cc::service_flow::update_status::current ||
                 update == cc::service_flow::update_status::ready ||
                 (update == cc::service_flow::update_status::available &&
                  window_services_->mode() != cc::service_flow::update_mode::installed))) {
                manual_update_pending_ = false;
                show_update_result();
            }
        }
    });
    service_poll_->start(10);
    arrange();
    render_window_services();
}

void main_window::render_window_services() {
    using namespace cc::service_flow;
    const auto phase = window_services_->scan();
    const auto state = window_services_->updates();
    const bool busy = phase != scan_status::idle;
    const bool waiting = phase == scan_status::draining || phase == scan_status::ready;
    if (waiting)
        status_->setText("Waiting for background requests to stop…");
    start_->setEnabled(!closing_ && !busy);
    // The clicked scan owns the selected inputs even while services drain.
    edit_->setEnabled(!closing_ && !busy);
    samples_->setEnabled(!closing_ && !busy);
    cancel_->setEnabled(!closing_ && (waiting || (scan_running_ && !cancel_requested_)));
    const bool can_export = !closing_ && !busy && model_.state == "completed";
    export_->setEnabled(can_export);
    copy_->setEnabled(can_export);
    const bool updates_allowed = window_services_->updates_allowed();
    updates_->setEnabled(updates_allowed && !closing_ && !busy &&
                         state != update_status::fetching && state != update_status::waiting &&
                         state != update_status::downloading);
    updates_->setText(!updates_allowed ? "Updates disabled"
                      : state == update_status::fetching || state == update_status::waiting
                          ? "Checking…"
                      : state == update_status::ready       ? "Update ready"
                      : state == update_status::downloading ? "Downloading…"
                      : state == update_status::available   ? "Update available"
                      : state == update_status::current     ? "Up to date"
                                                            : "Check for updates");
    updates_->setAccessibleName(updates_->text());
    const auto known = window_services_->confirmed().value("completedUtc").toString();
    const auto when = QDateTime::fromString(known, Qt::ISODate);
    const QString previous =
        when.isValid() ? "Last confirmed maintenance: " + when.toString("yyyy-MM-dd HH:mm 'UTC'")
                       : "No confirmed maintenance date.";
    QString maintenance = previous;
    if (busy)
        maintenance = "Maintenance refresh paused for scan. " + previous;
    else if (window_services_->maintenance() == notice_status::fetching)
        maintenance = "Checking maintenance notices… " + previous;
    else if (window_services_->maintenance() == notice_status::unavailable)
        maintenance = "Couldn't refresh maintenance notices. " + previous;
    else if (window_services_->maintenance() == notice_status::uncertain)
        maintenance = "Some notices could not be confirmed. " + previous;
    else if (window_services_->maintenance() == notice_status::stale)
        maintenance = "Maintenance information is out of date. " + previous;
#ifdef CC_PROFILE_HISTORY
    else if (profile_settings_ && !maintenance_preference_readable_)
        maintenance = "Maintenance preference unreadable. Existing file kept. " + previous;
    else if (profile_settings_ && !automatic_maintenance_)
        maintenance = "Automatic maintenance checks are off. " + previous;
#endif
    maintenance_status_ = maintenance;
    if (settings_dialog_)
        if (auto* status = settings_dialog_->findChild<QLabel*>("MaintenanceStatus"))
            status->setText(maintenance_status_);
}

void main_window::update_clicked() {
    if (closing_ || !window_services_ || !window_services_->updates_allowed() ||
        window_services_->scan() != cc::service_flow::scan_status::idle)
        return;
    if (window_services_->mode() == cc::service_flow::update_mode::metadata_only) {
        window_services_->request_update();
        render_window_services();
        return;
    }
    const auto state = window_services_->updates();
    if (state == cc::service_flow::update_status::ready ||
        state == cc::service_flow::update_status::available)
        show_update_result();
    else {
        manual_update_pending_ = true;
        window_services_->request_update();
        render_window_services();
    }
}

void main_window::show_update_result() {
    using namespace cc::service_flow;
    if (closing_ || !window_services_ || window_services_->scan() != scan_status::idle)
        return;
    const auto state = window_services_->updates();
    // A successful check is acknowledged on the button, without interrupting
    // the player. Errors and installation still require an explicit dialog.
    if (state == update_status::current)
        return;
    const auto metadata = window_services_->update_metadata();
    const bool ready =
        state == update_status::ready && window_services_->mode() == update_mode::installed;
    const bool portable =
        state == update_status::available && window_services_->mode() == update_mode::portable;
    const QString title = ready      ? "Update ready"
                          : portable ? "Update available"
                                     : "Couldn't check for updates";
    const QString message =
        ready
            ? "Version " + metadata["version"].toString() +
                  " is downloaded and verified. Close Channel Checker and open Setup? Your history and settings will be kept."
        : portable
            ? "Version " + metadata["version"].toString() +
                  " is available as a portable ZIP. Extract it to a new folder to update your portable copy."
        : window_services_->update_error().isEmpty()
            ? "Couldn't complete the update check or download. Try again or open the downloads page."
            : window_services_->update_error();
    QMessageBox dialog(QMessageBox::Information, title, message, QMessageBox::NoButton, this);
    dialog.setWindowTitle("Channel Checker — " + title);
    auto* cancel = dialog.addButton(ready ? "Cancel" : "Close", QMessageBox::RejectRole);
    auto* action = dialog.addButton(ready      ? "Install update"
                                    : portable ? "Open portable ZIP"
                                               : "Open downloads",
                                    QMessageBox::AcceptRole);
    dialog.setDefaultButton(cancel);
    dialog.setEscapeButton(cancel);
    dialog.exec();
    if (!action || dialog.clickedButton() != action || closing_)
        return;
    if (ready && window_services_->updates() == update_status::ready &&
        window_services_->scan() == scan_status::idle) {
        install_request_ = metadata;
        install_request_["verifiedPath"] = window_services_->ready_file();
        close();
    } else if (!ready) {
        const QUrl target(portable
                              ? metadata["portableUrl"].toString()
                              : "https://github.com/LOOKaUSERNAME/mabi-tools-releases/releases");
        if (!QDesktopServices::openUrl(target))
            QMessageBox::warning(
                this, "Open downloads",
                "Couldn't open your browser. Visit github.com/LOOKaUSERNAME/mabi-tools-releases/releases.");
    }
}
#ifdef CC_PROFILE_HISTORY
void main_window::load_channel_history(std::size_t index) {
    if (!history_ || !scan_config_)
        return;
    QDateTime maintenance;
    if (window_services_ && automatic_maintenance_)
        maintenance = QDateTime::fromString(
            window_services_->confirmed().value("completedUtc").toString(), Qt::ISODate);
    model_.saved_history = history_->compare(model_.endpoint(index), scan_config_->settings,
                                             QDateTime::currentDateTimeUtc(), maintenance);
}

void main_window::enable_profile_settings() {
    if (profile_root_.isEmpty() || !window_services_)
        throw std::logic_error("Profile settings require history and services.");
    profile_settings_ = true;
    try {
        QFile file(checked_profile_file(profile_root_, "automatic-maintenance.txt"));
        if (file.exists()) {
            if (!file.open(QIODevice::ReadOnly) || file.size() > 4)
                throw std::runtime_error("Invalid maintenance preference.");
            const auto preference_bytes = file.read(4);
            if (preference_bytes != "on\n" && preference_bytes != "off\n")
                throw std::runtime_error("Invalid maintenance preference.");
            automatic_maintenance_ = preference_bytes == "on\n";
        }
    } catch (const std::exception&) {
        maintenance_preference_readable_ = false;
    }
    render_window_services();
}

void main_window::set_automatic_maintenance(bool enabled) {
    try {
        QSaveFile file(checked_profile_file(profile_root_, "automatic-maintenance.txt"));
        file.setDirectWriteFallback(false);
        const QByteArray preference_bytes = enabled ? "on\n" : "off\n";
        if (!maintenance_preference_readable_ || !file.open(QIODevice::WriteOnly) ||
            file.write(preference_bytes) != preference_bytes.size() || !file.commit())
            throw std::runtime_error("Cannot save maintenance preference.");
        automatic_maintenance_ = enabled;
    } catch (const std::exception&) {
        maintenance_preference_readable_ = false;
        return;
    }
    const auto history = history_->view();
    window_services_->maintenance_context(enabled,
                                          history.loaded && (history.saving || history.count > 0));
}
#endif

}
