// Owns scan admission, worker hand-off, visual motion and shutdown.
#include "main_window.hpp"
#ifdef CC_WINDOW_SERVICES
#include "service_flow.hpp"
#endif
#include <QCloseEvent>
#include <QGraphicsOpacityEffect>
#include <algorithm>
#include <windows.h>

namespace cc::ui {
namespace {
bool animations_allowed() {
#ifdef Q_OS_WIN
    BOOL enabled = FALSE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) && enabled;
#else
    return false; // Offscreen verification does not imitate Windows preferences.
#endif
}
}

// Admit in two stages: freeze UI choices while services drain, then launch the
// owned worker only when the coordinator grants readiness. Results cross back
// through queued Qt callbacks or the completion record, never direct widget access.
void main_window::start_scan() {
    if (!scan_config_) {
        if (profile_root_.isEmpty())
            unavailable_in_fixture("Live scanning");
        return;
    }
    if (scan_running_ || closing_)
        return;

    // Snapshot inputs on the GUI thread. Neither a settings edit nor a
    // service completion may change the targets of an admitted scan.
    auto config = *scan_config_;
    config.settings.samples = samples_->currentText().toInt();
    try {
        config.validate();
    } catch (const std::exception& error) {
        // A syntactically valid channel list may still violate the transport
        // policy. Validate it before starting the worker or pausing services.
        scan_error_ = QString::fromUtf8(error.what());
        refresh();
        return;
    }
#ifdef CC_WINDOW_SERVICES
    if (window_services_) {
        using cc::service_flow::scan_status;
        if (window_services_->scan() == scan_status::idle) {
            window_services_->request_scan();
            render_window_services();
            return; // Even a synchronous admission starts on the next Qt tick.
        }
        if (window_services_->scan() != scan_status::ready)
            return;
        window_services_->scan_started();
    }
#endif
    // History admission has its own generation. It may reject persistence
    // without cancelling a valid network check.
    const auto history_request = history_ ? std::optional{history_->capture()} : std::nullopt;
    scan_config_->settings = config.settings;
    const auto generation = ++scan_generation_;
    if (hover_dialog_)
        hover_dialog_->close();
    if (details_dialog_)
        details_dialog_->close();
    // Clear the old report, table, charts and picks before admitting any work.
    // The model object itself stays at the same address for card const views.
    model_ = replay_model::ready_scan(config);
    model_.state = "active";
    scan_running_ = true;
    cancel_requested_ = false;
    warming_up_ = true;
    scan_error_.clear();
    feedback_->clear();

    refresh();
    refresh_update_status();
    completion_ = std::make_shared<scan_completion>();
    const auto completion = completion_;
    completion->options = config.settings;
    // The worker owns its config copy and shares the completion record with the
    // GUI. The captured window/history store stay alive while closeEvent waits.
    try {
        scan_worker_ = std::jthread(
            [this, config, generation, completion, history_request](std::stop_token stop) {
                try {
                    if (history_request) {
                        completion->history_admission_pending = true;
                        completion->history_ticket = history_->admit(*history_request, stop);
                        completion->history_admission_pending = false;
                    }
                    if (stop.stop_requested())
                        throw cc::scanning::cancelled{};
                    cc::scanning::tcp_transport transport;
                    completion->result = cc::scanning::scan(
                        config.targets, config.settings,
                        [&](const auto& target, int round, int timeout, std::int64_t origin,
                            std::stop_token token) {
                            return transport.probe(target, round, timeout, origin, token);
                        },
                        [this, generation](const cc::scanning::progress& progress) {
                            QMetaObject::invokeMethod(
                                this,
                                [this, generation, progress] {
                                    receive_progress(generation, progress);
                                },
                                Qt::QueuedConnection);
                        },
                        stop);
                } catch (const cc::scanning::cancelled&) {
                    completion->cancelled = true;
                } catch (const std::exception& error) {
                    completion->error = error.what();
                } catch (...) {
                    completion->error = "Unexpected native scan failure";
                }
                completion->done = true; // Publishes result/error only after transport cleanup.
            });
        scan_poll_->start();
    } catch (const std::exception& error) {
        scan_running_ = false;
        model_.state = "cancelled";
        scan_error_ = QString::fromUtf8(error.what());
#ifdef CC_WINDOW_SERVICES
        if (window_services_)
            window_services_->scan_finished();
#endif
        refresh();
        refresh_update_status();
    }
}

void main_window::receive_progress(std::uint64_t generation,
                                   const cc::scanning::progress& progress) {
    // Qt may deliver already-queued progress after cancellation or restart.
    // Only the current worker is allowed to repaint the current model.
    if (closing_ || !scan_running_ || cancel_requested_ || generation != scan_generation_)
        return;
    warming_up_ = progress.warming_up;
    model_.apply_progress(*progress.values);
    refresh(!progress.warming_up && animations_allowed());
}

void main_window::cancel_scan() {
    if (!scan_config_) {
        model_.state = "cancelled";
        refresh();
        refresh_update_status();
        return;
    }
#ifdef CC_WINDOW_SERVICES
    if (window_services_ && !scan_running_ && !closing_) {
        window_services_->scan_finished();
        refresh();
        render_window_services();
        return;
    }
#endif
    if (!scan_running_)
        return;
    cancel_requested_ = true;
    progress_bar_->advance_fill(false);
    scan_worker_.request_stop();
    cancel_->setEnabled(false);
}

// Consume completion on the GUI thread after joining. Cancellation and failure
// do not become completed reports; only a valid report with a history ticket
// enters the disk queue. Closing skips normal rendering and retries shutdown.
void main_window::finish_scan() {
    if (!completion_ || !completion_->done.load())
        return;
    // Joining before consuming the result also drains the socket owner.
    scan_poll_->stop();
    scan_worker_.join();
    scan_running_ = false;
    const auto completed = std::move(completion_);
    if (closing_) {
        close();
        return;
    }
#ifdef CC_WINDOW_SERVICES
    if (window_services_)
        window_services_->scan_finished();
#endif
    if (cancel_requested_ || completed->cancelled)
        model_.state = "cancelled";
    else if (!completed->error.empty()) {
        model_.state = "cancelled";
        scan_error_ = QString::fromStdString(completed->error);
    } else {
        try {
            model_ = replay_model::completed_native(*completed->result, completed->options);
        } catch (const std::exception& error) {
            model_.state = "cancelled";
            scan_error_ = QString::fromUtf8(error.what());
        }
        if (model_.state == "completed" && completed->history_ticket) {
            try {
                history_->enqueue_save(*completed->history_ticket, model_.original_json);
            } catch (const std::exception&) {
                feedback_->setText("Report available. Could not queue scan history.");
            }
        }
    }
    if (model_.state == "completed" && model_.summary.picks.overall >= 0 && animations_allowed())
        start_result_fade();
    refresh();
    refresh_update_status();
}

void main_window::start_result_fade() {
    result_fade_.start();
    for (auto* card : recommendations_) {
        auto* effect = new QGraphicsOpacityEffect(card);
        effect->setOpacity(0.);
        card->setGraphicsEffect(effect);
    }
}

void main_window::tick_motion() {
    const bool allowed = animations_allowed();
    bool busy = progress_bar_->advance_fill(allowed);
    for (auto* card : cards_)
        busy = card->tick_motion(allowed) || busy;
    if (result_fade_.isValid()) {
        constexpr double result_fade_ms = 150.;
        const double opacity =
            allowed
                ? std::clamp(static_cast<double>(result_fade_.elapsed()) / result_fade_ms, 0., 1.)
                : 1.;
        for (auto* card : recommendations_) {
            if (opacity >= 1.)
                card->setGraphicsEffect(nullptr);
            else if (auto* effect = qobject_cast<QGraphicsOpacityEffect*>(card->graphicsEffect()))
                effect->setOpacity(opacity);
        }
        if (opacity >= 1.)
            result_fade_.invalidate();
        else
            busy = true;
    }
    if (!busy)
        motion_timer_->stop();
}

// A close request starts drainage rather than destroying the window immediately.
// Ignore the event while any owned work remains so Qt can deliver completion
// callbacks; the scan, history and service polls retry close as each drains.
void main_window::closeEvent(QCloseEvent* event) {
    closing_ = true;
#ifdef CC_WINDOW_SERVICES
    if (window_services_)
        window_services_->close();
#endif
    // Modeless owned windows must not keep the application alive after its owner exits.
    for (auto dialog : findChildren<QDialog*>(QString(), Qt::FindDirectChildrenOnly))
        dialog->close();
    if (scan_running_) {
        cancel_scan();
        event->ignore();
        // Keep pumping the real window until the owned worker has drained. The
        // poll joins it and retries close; no callback can update this closing UI.
        return;
    }
    if (history_ && history_->view().pending != 0) {
        event->ignore();
        return; // History poll retries close after queued writes drain.
    }
#ifdef CC_WINDOW_SERVICES
    if (window_services_) {
        if (!window_services_->closed()) {
            event->ignore();
            return;
        }
        service_poll_->stop();
    }
#endif
    if (history_poll_)
        history_poll_->stop();
    event->accept();
}
} // namespace cc::ui
