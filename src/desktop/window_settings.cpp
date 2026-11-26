// Settings presentation and consent flow. A fixed heading/footer surround the
// scrollable history and maintenance sections. history_store owns persistence;
// this layer previews retention changes, offers export and confirms deletion,
// retrying consent if another writer changed the affected files.

#include "main_window.hpp"
#include "dialogs.hpp"
#ifdef CC_PROFILE_HISTORY
#include "wpf_migration.hpp"
#include <QFileInfo>
#endif
#include <QApplication>
#include <QCheckBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QDesktopServices>
#include <QPlainTextEdit>
#include <QScreen>
#include <QScrollArea>
#include <QUrl>
#include <QDir>
#include <QMessageBox>
#include <QPainter>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace cc::ui {
namespace {
class settings_switch final : public QCheckBox {
public:
    explicit settings_switch(QWidget* parent) : QCheckBox(parent) {}

    QSize sizeHint() const override { return {42, 24}; }

protected:
    bool hitButton(const QPoint& point) const override { return rect().contains(point); }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setOpacity(isEnabled() ? 1. : .45);
        painter.setPen(Qt::NoPen);
        painter.setBrush(isChecked() ? QColor("#70B1AA") : QColor("#59636A"));
        painter.drawRoundedRect(QRectF(0, 0, 42, 24), 12, 12);
        painter.setBrush(QColor("#EDF3F4"));
        painter.drawEllipse(QRectF(isChecked() ? 22 : 4, 4, 16, 16));
        if (hasFocus()) {
            painter.setPen(QPen(QColor("#D7E9E7"), 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(QRectF(1, 1, 40, 22), 11, 11);
        }
    }
};
}
#ifdef CC_PROFILE_HISTORY
void main_window::change_history_limits(QDialog* parent, history_store::limits desired) {
    if (!history_ || closing_)
        return;
    try {
        // No filesystem lock is held while the user reviews the dialog.
        // Export and apply each check the same plan under the profile lock.
        auto plan = history_->preview_limits(desired);
        bool review_again = false;
        bool exported = false;
        for (;;) {
            if (review_again || !plan.affected.empty() || plan.over_budget) {
                QMessageBox review(QMessageBox::Question, "Change history limits?", "",
                                   QMessageBox::NoButton, parent);
                const auto freed = static_cast<double>(plan.bytes) / (1024. * 1024.);
                const auto scan_count = static_cast<qulonglong>(plan.affected.size());
                const auto space =
                    freed > 0. && freed < .01 ? QString("<0.01") : QString::number(freed, 'f', 2);
                QString message = QString("The new limits would delete %1 %2 and free %3 MiB.")
                                      .arg(scan_count)
                                      .arg(scan_count == 1 ? "scan" : "scans", space);
                if (review_again)
                    message.prepend("History changed. Review the updated selection.\n\n");
                if (exported)
                    message += "\n\nExport saved. No scans have been deleted.";
                if (plan.over_budget)
                    message +=
                        "\n\nSome history could not be safely pruned. Usage may remain above the limit.";
                review.setText(message);
                auto* cancel = review.addButton(QMessageBox::Cancel);
                auto* confirm =
                    review.addButton(plan.affected.empty() ? "Apply limits" : "Apply and delete",
                                     QMessageBox::DestructiveRole);
                auto* archive = review.addButton("Export affected scans", QMessageBox::ActionRole);
                archive->setEnabled(!plan.affected.empty());
                review.setDefaultButton(cancel);
                review.setEscapeButton(cancel);
                review.exec();
                if (review.clickedButton() == archive) {
                    const auto target = QFileDialog::getSaveFileName(
                        parent, "Export affected scans", "ChannelChecker-History.json",
                        "JSON (*.json)");
                    if (target.isEmpty())
                        return;
                    const bool saved = history_->export_affected(plan, target);
                    if (!saved) {
                        plan = history_->preview_limits(desired);
                        review_again = true;
                    } else {
                        exported = true;
                        review_again = false;
                    }
                    continue;
                }
                if (review.clickedButton() != confirm)
                    return;
            }
            // A scan may have arrived during the dialog or export. Never
            // carry consent over to a different deletion set.
            if (history_->apply_limits(plan))
                break;
            plan = history_->preview_limits(desired);
            review_again = true;
            exported = false;
        }
    } catch (const std::exception& error) {
        QMessageBox::warning(parent, "History limits", QString::fromUtf8(error.what()));
    }
    refresh_history_settings();
}
#endif

QDialog* main_window::history_settings_dialog() {
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Channel Checker — Settings");
    dialog->setWindowIcon(windowIcon());
    dialog->setMinimumSize(420, 430);
    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(22, 18, 22, 18);
    layout->setSpacing(14);
    layout->addWidget(label(dialog, "Settings", 23, "#F2F4F7", true));
    auto* scroll = new QScrollArea(dialog);
    scroll->setObjectName("SettingsScroll");
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget(scroll);
    body->setStyleSheet("QWidget { background:transparent; }");
    auto* content = new QVBoxLayout(body);
    content->setContentsMargins(0, 0, 16, 0);
    content->setSpacing(12);
    scroll->setWidget(body);
    layout->addWidget(scroll, 1);
    const auto paragraph = [](QWidget* parent, const QString& message) {
        auto* text = label(parent, message, 12, "#A8B0BB");
        text->setWordWrap(true);
        return text;
    };
    const auto section = [&](const QString& title, const QString& name) {
        auto* card = new QFrame(body);
        card->setObjectName(name);
        card->setStyleSheet(
            "QFrame#" + name +
            " { background:#171C20; border:1px solid #343A3F; border-radius:8px; }");
        auto* box = new QVBoxLayout(card);
        box->setContentsMargins(16, 16, 16, 16);
        box->setSpacing(12);
        box->addWidget(label(card, title, 13, "#F2F4F7", true));
        content->addWidget(card);
        return box;
    };
    auto* historySection = section("History", "HistorySection");
    const auto toggle = [&](QVBoxLayout* box, const QString& title, const QString& id,
                            const QString& help) {
        auto* row = new QHBoxLayout;
        row->setSpacing(16);
        auto* copy = new QVBoxLayout;
        copy->setSpacing(4);
        copy->addWidget(label(dialog, title));
        auto* supporting = paragraph(dialog, help);
        supporting->setObjectName(id + "Help");
        copy->addWidget(supporting);
        row->addLayout(copy, 1);
        auto* control = new settings_switch(dialog);
        control->setObjectName(id);
        control->setAccessibleName(title);
        control->setFixedSize(42, 24);
        row->addWidget(control, 0, Qt::AlignTop);
        box->addLayout(row);
        return control;
    };
    auto* saving = toggle(historySection, "Save scan history", "SaveHistory",
                          "Completed scans are saved locally.");
    auto* note = paragraph(
        dialog,
        "When you save a new scan, older scans beyond your age or storage limit are removed. Exported reports are kept.");
#ifdef CC_PROFILE_HISTORY
    if (profile_settings_) {
        auto* options = new QVBoxLayout;
        options->setSpacing(12);
        auto* age = new QComboBox(dialog);
        age->setObjectName("HistoryAgeLimit");
        age->setAccessibleName("Keep saved scans for");
        age->addItem("7 days", 7);
        age->addItem("30 days", 30);
        age->addItem("90 days", 90);
        age->addItem("No age limit", 0);
        auto* budget = new QComboBox(dialog);
        budget->setObjectName("HistoryStorageLimit");
        budget->setAccessibleName("Storage limit");
        for (const int value : {100, 250, 500})
            budget->addItem(QString("%1 MiB").arg(value), value);
        for (auto* option : {age, budget}) {
            option->setMinimumHeight(34);
            option->setFixedWidth(130);
        }
        for (const auto& row : {std::pair<QString, QComboBox*>{"Keep saved scans for", age},
                                {"Storage limit", budget}}) {
            auto* line = new QHBoxLayout;
            line->addWidget(label(dialog, row.first), 1);
            line->addWidget(row.second);
            options->addLayout(line);
        }
        historySection->addLayout(options);
        historySection->addWidget(note);
        for (auto* option : {age, budget})
            connect(option, &QComboBox::activated, dialog, [this, dialog, age, budget] {
                const history_store::limits requested{age->currentData().toInt(),
                                                      budget->currentData().toInt()};
                refresh_history_settings();
                change_history_limits(dialog, requested);
                refresh_history_settings();
            });
    }
#endif
    historySection->addWidget(paragraph(
        dialog,
        "Channel comparisons use up to 7 days. Confirmed game maintenance can shorten this period."));
    auto* saved = new QFrame(dialog);
    saved->setObjectName("SavedHistoryCard");
    saved->setStyleSheet(
        "QFrame#SavedHistoryCard { background:#101518; border:1px solid #30383E; border-radius:6px; }");
    auto* savedLayout = new QVBoxLayout(saved);
    savedLayout->setContentsMargins(12, 12, 12, 12);
    savedLayout->setSpacing(10);
    auto* savedHeader = new QHBoxLayout;
    auto* savedLabels = new QVBoxLayout;
    savedLabels->setSpacing(4);
    savedLabels->addWidget(label(saved, "Saved history", 13, "#F2F4F7", true));
    auto* count = label(saved, "", 12, "#A8B0BB");
    count->setObjectName("HistoryCount");
    savedLabels->addWidget(count);
    savedHeader->addLayout(savedLabels, 1);
    auto* folder = button(saved, "Open folder", "OpenHistoryFolder");
    folder->setMinimumHeight(34);
    savedHeader->addWidget(folder);
    QPixmap folderImage(18, 18);
    folderImage.fill(Qt::transparent);
    {
        QPainter painter(&folderImage);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor("#EDF3F4"), 1.2));
        painter.drawPolyline(QPolygonF{{2, 6}, {2, 3}, {7, 3}, {9, 6}, {16, 6}, {16, 8}});
        painter.drawPolyline(QPolygonF{{2, 6}, {2, 16}, {14, 16}, {17, 8}, {5, 8}, {2, 16}});
    }
    folder->setIcon(QIcon(folderImage));
    savedLayout->addLayout(savedHeader);
    auto* path = new QPlainTextEdit(QDir::toNativeSeparators(history_->folder()), saved);
    path->setObjectName("HistoryFolderPath");
    path->setAccessibleName("History folder path");
    path->setReadOnly(true);
    path->setFrameShape(QFrame::NoFrame);
    path->setFixedHeight(54);
    path->setStyleSheet(
        "QPlainTextEdit { background:#101518; color:#A8B0BB; font-size:12px; border:0; }");
    savedLayout->addWidget(path);
    savedLayout->addWidget(paragraph(saved, "Saved on this PC."));
    historySection->addWidget(saved);
    auto* text = paragraph(dialog, "");
    text->setObjectName("HistoryStatus");
    connect(folder, &QPushButton::clicked, dialog, [this, text] {
        const auto directory = history_->folder();
        if (!QDir(directory).exists() || !QDesktopServices::openUrl(QUrl::fromLocalFile(directory)))
            text->setText("Couldn't open the history folder. You can copy the path above.");
    });
#ifdef CC_PROFILE_HISTORY
    if (profile_settings_) {
        auto* game = section("Game maintenance", "MaintenanceSection");
        auto* automatic = toggle(
            game, "Check game maintenance dates automatically", "AutomaticMaintenance",
            "Uses Nexon's Mabinogi notices to keep checks from before game maintenance out of your history comparisons.");
        automatic->setChecked(automatic_maintenance_);
        automatic->setEnabled(maintenance_preference_readable_ && !closing_);
        auto* maintenance =
            paragraph(dialog, maintenance_status_.isEmpty() ? "No confirmed maintenance date."
                                                            : maintenance_status_);
        maintenance->setObjectName("MaintenanceStatus");
        game->addWidget(maintenance);
        connect(automatic, &QCheckBox::toggled, dialog,
                [this, automatic, maintenance](bool enabled) {
                    set_automatic_maintenance(enabled);
                    const QSignalBlocker blocked(automatic);
                    automatic->setChecked(automatic_maintenance_);
                    automatic->setEnabled(maintenance_preference_readable_);
                    maintenance->setText(maintenance_status_);
                });
        if (QFileInfo(wpf_archive_path(profile_root_)).exists()) {
            auto* archive = button(dialog, "View imported WPF history", "ViewImportedWpfHistory");
            archive->setMinimumHeight(36);
            content->addWidget(archive);
            connect(archive, &QPushButton::clicked, dialog, [this, dialog] {
                try {
                    show_wpf_archive(dialog, profile_root_);
                } catch (const std::exception&) {
                    QMessageBox::warning(dialog, "Imported WPF history",
                                         "The archive could not be opened.");
                }
            });
        }
    }
#endif
    auto* action = new QHBoxLayout;
    auto* clearHelp = new QVBoxLayout;
    clearHelp->addWidget(label(dialog, "Delete saved scans from this PC."));
    clearHelp->addWidget(paragraph(dialog, "Archived history and exported reports are kept."));
    action->addLayout(clearHelp, 1);
    auto* clear = button(dialog, "Clear saved history", "ClearSavedHistory");
    clear->setMinimumHeight(36);
    action->addWidget(clear);
    clear->setStyleSheet("QPushButton { color:#F2B19E; }");
    content->addLayout(action);
    content->addWidget(text);
    content->addStretch();
    auto* bottom = new QHBoxLayout;
    bottom->setContentsMargins(0, 0, 16, 0);
    bottom->addStretch();
    auto* close_button = button(dialog, "Close", "CloseHistorySettings");
    close_button->setMinimumSize(90, 36);
    bottom->addWidget(close_button);
    layout->addLayout(bottom);
    connect(saving, &QCheckBox::toggled, dialog, [this](bool enabled) {
        history_->set_saving(enabled);
        refresh_history_settings();
    });
    connect(clear, &QPushButton::clicked, dialog, [this, dialog] {
        if (!profile_root_.isEmpty() &&
            QMessageBox::question(
                dialog, "Clear saved history?",
                "Delete saved scans from this profile? Archived history and exported reports are kept.",
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        history_->clear();
        refresh_history_settings();
    });
    connect(close_button, &QPushButton::clicked, dialog, &QDialog::close);
    const auto area = screen()->availableGeometry();
    dialog->resize(std::min(590, area.width()), std::min(820, area.height() - 60));
    return dialog;
}

void main_window::refresh_history_settings() {
    if (!settings_dialog_ || !history_)
        return;
    auto saving = settings_dialog_->findChild<QCheckBox*>("SaveHistory");
    auto clear = settings_dialog_->findChild<QPushButton*>("ClearSavedHistory");
    auto count = settings_dialog_->findChild<QLabel*>("HistoryCount");
    auto text = settings_dialog_->findChild<QLabel*>("HistoryStatus");
    if (!saving || !clear || !count || !text)
        return;
    const auto state = history_->view();
    saving->setEnabled(state.loaded && !closing_);
    clear->setEnabled(state.loaded && !closing_);
    if (state.pending == 0) {
        const QSignalBlocker blocked(saving);
        saving->setChecked(state.saving);
    }
    count->setText(QString("%1 saved scans").arg(state.count));
    if (auto* help = settings_dialog_->findChild<QLabel*>("SaveHistoryHelp"))
        help->setText(state.saving ? "Completed scans are saved locally."
                                   : "New scans aren't saved; history is kept.");
#ifdef CC_PROFILE_HISTORY
    if (auto* age = settings_dialog_->findChild<QComboBox*>("HistoryAgeLimit")) {
        auto* budget = settings_dialog_->findChild<QComboBox*>("HistoryStorageLimit");
        const QSignalBlocker age_block(age), budget_block(budget);
        age->setEnabled(state.loaded && !closing_ && !scan_running_ && state.pending == 0);
        budget->setEnabled(age->isEnabled());
        age->setCurrentIndex(age->findData(state.retention.days));
        budget->setCurrentIndex(budget->findData(state.retention.mib));
        count->setText(QString("%1 saved scans · %2 / %3 MiB")
                           .arg(state.count)
                           .arg(static_cast<double>(state.bytes) / (1024. * 1024.), 0, 'f', 1)
                           .arg(state.retention.mib));
    }
#endif
    text->setText(state.pending > 0 ? "Waiting for history storage…" : state.error);
}

QDialog* main_window::show_dialog(const QString& name) {
    QDialog* result = nullptr;
    if (name == "help")
        result = help_dialog(this);
    else if (name == "channels") {
        QStringList default_channels;
        for (const auto& target : default_channels_)
            default_channels << QString("%1,%2,%3")
                                    .arg(target.number)
                                    .arg(QString::fromStdString(target.host))
                                    .arg(target.port);
        result = channel_dialog(
            this, model_, [this](auto targets) { apply_channels(std::move(targets)); },
            !channel_settings_root_.isEmpty(), default_channels);
    } else if (name.startsWith("settings")) {
        if (settings_dialog_) {
            settings_dialog_->raise();
            settings_dialog_->activateWindow();
            return settings_dialog_;
        }
        if (!history_)
            throw std::logic_error("Settings require a history profile.");
        result = history_settings_dialog();
        settings_dialog_ = result;
        refresh_history_settings();
    } else if (name == "history-empty" || name == "history-saved") {
        show_details(0);
        return details_dialog_;
    } else
        throw std::invalid_argument("Unknown dialog");
    if (name == "help" || name == "channels")
        result->setWindowModality(Qt::WindowModal);
    result->show();
    return result;
}
} // namespace cc::ui
