#pragma once
#include "channel_card.hpp"
#include "history_store.hpp"
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QComboBox>
#include <QTableWidget>
#include <QDialog>
#include <QTimer>
#include <QRadioButton>
#include <QPointer>
#include "scan_progress_bar.hpp"
#include <atomic>
#include <thread>

#ifdef CC_WINDOW_SERVICES
namespace cc::service_flow {
class coordinator;
}
#endif

namespace cc::ui {
// GUI-thread owner of the stable view model, child widgets, scan worker and
// history queue. Service coordination is borrowed from the application. Closing
// is asynchronous until all work drains; queued progress is generation-checked
// before replacing model data. Cards/dialogs must not outlive their model view.
class main_window final : public QWidget {
public:
    explicit main_window(replay_model model, std::optional<scan_config> configuration = {},
                         QString channel_settings_root = {}, QString settings_warning = {},
                         QString fixture_history_root = {}, QString profile_root = {});
    ~main_window() override;
    void capture(const QString& output);
#ifdef CC_WINDOW_SERVICES
    // The caller owns the coordinator until after the window is destroyed.
    void attach_window_services(cc::service_flow::coordinator&);

    QJsonObject install_request() const { return install_request_; }
#ifdef CC_PROFILE_HISTORY
    void enable_profile_settings();
    void fit_initial_results_grid();
#endif
#endif
    const replay_model& model() const { return model_; }

    QDialog* show_dialog(const QString& name);

    bool scan_running() const { return scan_running_; }

protected:
    void resizeEvent(QResizeEvent*) override;
    void closeEvent(QCloseEvent*) override;
    bool eventFilter(QObject* object, QEvent* event) override;

private:
    friend struct main_window_test_access;
    bool write_report(const QString& path, bool as_json) const;
    replay_model model_;
    QScrollArea* scroll_{};
    QScrollArea* activity_{};
    QWidget* card_grid_{};
    QWidget* page_{};
    QLabel *logo_{}, *title_{}, *range_{}, *status_{}, *progress_{}, *recommendation_{},
        *feedback_{};
    QWidget *options_{}, *results_{}, *recommendations_[3]{}, *timing_group_{};
    scan_progress_bar* progress_bar_{};
    QLabel *pick_[3]{}, *note_[3]{};
    QPushButton *help_{}, *updates_{}, *settings_{}, *edit_{}, *start_{}, *cancel_{}, *details_{},
        *copy_{}, *export_{};
    QRadioButton *paired_{}, *total_{};
    QComboBox* samples_{};
    QTableWidget* table_{};
    std::vector<channel_card*> cards_;
    bool total_view_{true}, details_view_{};
    bool cursor_sync_pending_{};
    QPointer<QWidget> cursor_action_;
    QPointer<QDialog> hover_dialog_, details_dialog_, settings_dialog_;
    QTimer* hover_leave_timer_{};
    std::size_t hovered_index_{};
    std::optional<scan_config> scan_config_;
    QString channel_settings_root_;
    QString profile_root_;
    std::optional<scan_config> authorized_scan_;
    std::vector<cc::scanning::endpoint> default_channels_;
    std::unique_ptr<history_store> history_;
    QTimer* history_poll_{};
    QDialog* history_settings_dialog();
    void refresh_history_settings();
#ifdef CC_PROFILE_HISTORY
    void change_history_limits(QDialog*, history_store::limits);
#endif
    // The worker writes result/error, then publishes done. The GUI polls done
    // and joins before consuming those non-atomic fields; progress uses separate
    // immutable snapshots queued through Qt.
    struct scan_completion {
        std::atomic<bool> done{};
        std::atomic<bool> history_admission_pending{};
        std::optional<cc::scanning::result> result;
        std::string error;
        bool cancelled{};
        cc::scanning::options options;
        std::optional<history_store::ticket> history_ticket;
    };

    std::shared_ptr<scan_completion> completion_;
    std::jthread scan_worker_;
    QTimer* scan_poll_{};
    std::uint64_t scan_generation_{};
    bool scan_running_{}, cancel_requested_{}, closing_{}, warming_up_{};
    QString scan_error_;
#ifdef CC_WINDOW_SERVICES
    cc::service_flow::coordinator* window_services_{};
    QString maintenance_status_;
    QTimer* service_poll_{};
    void render_window_services();
    void update_clicked();
    void show_update_result();
    bool manual_update_pending_{};
    QJsonObject install_request_;
#ifdef CC_PROFILE_HISTORY
    bool profile_settings_{}, automatic_maintenance_{}, maintenance_preference_readable_{true};
    void set_automatic_maintenance(bool);
    void load_channel_history(std::size_t index);
#endif
#endif
    void start_scan();
    void cancel_scan();
    void finish_scan();
    void receive_progress(std::uint64_t generation, const cc::scanning::progress&);
    void refresh_table();
    void apply_channels(std::vector<cc::scanning::endpoint>);
    void rebuild_channel_views();
    void clear_channel_views();
    void show_hover(std::size_t index);
    void leave_hover();
    void refresh_update_status();
    void arrange();
    void refresh(bool animate_progress = false);
    QTimer* motion_timer_{};
    QElapsedTimer result_fade_;
    void tick_motion();
    void start_result_fade();
    double measured_percent() const;
    void unavailable_in_fixture(const QString& feature);
    void show_details(std::size_t index);
};

QString stylesheet();
QLabel* label(QWidget* parent, const QString& text, int size = 13, const QString& color = "#F2F4F7",
              bool bold = false);
QPushButton* button(QWidget* parent, const QString& text, const QString& name);
void capture_widget(QWidget& widget, const QString& output);
}
