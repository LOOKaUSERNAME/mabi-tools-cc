#pragma once
#include "replay_model.hpp"
#include <QDialog>
#include <QStringList>
#include <functional>

namespace cc::ui {
// Each dialog owns its child widgets. Returned dialogs belong to the owner and
// delete on close. The caller decides whether channel edits persist to a profile
// or remain in memory. apply_channels must finish validation/persistence before
// returning: an exception keeps the draft open and displays its error.
// Detail charts borrow model history; close them before replacing that data.
QDialog* help_dialog(QWidget* owner);
QDialog* channel_dialog(
    QWidget* owner, const replay_model& model,
    const std::function<void(std::vector<cc::scanning::endpoint>)>& apply_channels,
    bool fixture_backed = false, const QStringList& default_channels = {});
QDialog* detail_dialog(QWidget* owner, const replay_model& model, std::size_t index,
                       bool expanded = true);
}
