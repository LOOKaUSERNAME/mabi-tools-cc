// Arranges the window in Qt logical pixels, including compact and high-DPI layouts.
#include "main_window.hpp"
#include <QScreen>
#include <QScrollBar>
#include <QTextLayout>
#include <QWindow>
#include <algorithm>

namespace cc::ui {
void main_window::resizeEvent(QResizeEvent*) {
    arrange();
}

void main_window::arrange() {
    if (!page_)
        return;
    scroll_->setGeometry(rect());
    // The content can be wider than the viewport; controls that must remain
    // reachable at minimum width follow the actual toolbar width instead.
    const int w = std::max(760, width() - 44);
    const int h = std::max(760, height() - 30);
    const int toolbar = width() - 44;
    page_->resize(w + 44, h + 30);
    const int title_width = std::max(80, toolbar - 452);
    QFont title_font("Segoe UI Variable");
    title_font.setPixelSize(25);
    title_font.setWeight(QFont::Bold);
    QTextLayout title_layout(title_->text(), title_font);
    title_layout.beginLayout();
    int title_lines = 0;
    while (true) {
        auto line = title_layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(title_width);
        ++title_lines;
    }
    title_layout.endLayout();
    // WPF's display font uses 33-DIP lines. Reserve that height before positioning
    // later stages; minimum-width wrapping must never overlap the control panel.
    const int top_height = std::max(48, title_lines * 33);
    logo_->setGeometry(22, 18 + (top_height - 48) / 2, 48, 48);
    title_->setGeometry(84, 18, title_width, top_height);
    help_->setGeometry(22 + toolbar - 374, 18, 112, 38);
    updates_->setGeometry(22 + toolbar - 254, 18, 142, 38);
    settings_->setGeometry(22 + toolbar - 104, 18, 104, 38);
    const int options_y = 18 + top_height + 20;
    options_->setGeometry(22, options_y, w, 68);
    edit_->setGeometry(17, 15, 102, 38);
    start_->setGeometry(w - 142, 15, 125, 38);
    cancel_->setGeometry(w - 221, 15, 71, 38);
    const int result_y = options_y + 82;
    results_->setGeometry(22, result_y, w, 150);
    recommendation_->setGeometry(17, 42, w - 34, 95);
    const int pick_width = (w - 34) / 3;
    for (int i = 0; i < 3; ++i) {
        const int x = 17 + pick_width * i;
        recommendations_[i]->setGeometry(x, 42, i == 2 ? w - 17 - x : pick_width - 8, 95);
        note_[i]->resize(recommendations_[i]->width() - 30, 17);
    }
    const int graph_y = result_y + 164;
    page_->findChild<QLabel*>("GraphHeading")->setGeometry(37, graph_y, 160, 34);
    page_->findChild<QLabel*>("ScanPulse")->setGeometry(22, graph_y, 9, 34);
    const int timing_width = paired_->width() + total_->width() + 6;
    // Keep the range on a second line when either view's text would collide
    // with the controls; switching modes must not move the toolbar.
    const bool stacked = toolbar < timing_width + 277 + range_->property("requiredWidth").toInt();
    timing_group_->setGeometry(22 + toolbar - 83 - timing_width, graph_y, timing_width, 34);
    details_->setGeometry(22 + toolbar - 66, graph_y, 66, 34);
    range_->setGeometry(stacked ? 22 : 198, graph_y + (stacked ? 42 : 0),
                        stacked ? toolbar : std::max(0, toolbar - timing_width - 277),
                        stacked ? 16 : 34);
    const int status_y = graph_y + (stacked ? 68 : 42);
    status_->setGeometry(22, status_y, w - 60, 20);
    progress_->setGeometry(22 + w - 50, status_y, 50, 20);
    progress_bar_->setGeometry(22, status_y + 28, w, 5);
    constexpr int card_height = 88, gap = 8, columns = 4, stride = card_height + gap;
    const int cards_y = status_y + 45, footer_y = 18 + h - 70;
    const int rows = static_cast<int>((cards_.size() + columns - 1) / columns);
    const int grid_height = rows ? rows * stride - gap : 0;
    const int scroll_height = std::max(100, footer_y - 14 - cards_y);
    activity_->setGeometry(22, cards_y, w, scroll_height);
    // Reserve the full bar extent in scroll mode, even when the platform
    // paints over the viewport rather than making the viewport narrower.
    const int extent = activity_->verticalScrollBar()->sizeHint().width();
    const int grid_width = w - (grid_height > scroll_height ? std::max(0, extent) : 0);
    card_grid_->resize(grid_width, grid_height);
    for (std::size_t i = 0; i < cards_.size(); ++i) {
        const auto column = static_cast<int>(i % columns);
        const int left = column * (grid_width + gap) / columns;
        const int right = (column + 1) * (grid_width + gap) / columns - gap;
        cards_[i]->setGeometry(left, static_cast<int>(i / columns) * stride, right - left,
                               card_height);
    }
    table_->setGeometry(22, cards_y, w, std::max(100, footer_y - 14 - cards_y));
    // Leave the last column reachable through the table's scrollbar below its
    // readable minimum. At normal width it absorbs the remaining space.
    table_->setColumnWidth(9, std::max(134, w - 18 - 930));
    page_->findChild<QLabel*>("EndpointSummary")->setGeometry(22, footer_y + 7, w - 204, 24);
    copy_->setGeometry(22 + w - 196, footer_y, 105, 38);
    export_->setGeometry(22 + w - 83, footer_y, 83, 38);
    feedback_->setGeometry(22, footer_y + 30, w - 204, 18);
    page_->findChild<QLabel*>("VersionText")->setGeometry(22, footer_y + 54, 200, 16);
    page_->findChild<QLabel*>("PlatformText")->setGeometry(22 + w - 110, footer_y + 54, 110, 16);
}
#if defined(CC_WINDOW_SERVICES) && defined(CC_PROFILE_HISTORY)
void main_window::fit_initial_results_grid() {
    // Call after the first show: Windows now knows the screen and the frame
    // size at its current DPI. Only initial sizing is affected.
    const auto available = screen()->availableGeometry();
    const auto frame = windowHandle()->frameMargins();
    const int width_limit =
        std::max(minimumWidth(), available.width() - frame.left() - frame.right());
    const int height_limit =
        std::max(minimumHeight(), available.height() - frame.top() - frame.bottom());
    resize(std::min(width(), width_limit), std::min(height(), height_limit));
    arrange();
    const auto missing = std::max(0, card_grid_->height() - activity_->height());
    resize(width(), std::min(height() + missing, height_limit));
    arrange();
    const auto frame_bounds = frameGeometry();
    windowHandle()->setFramePosition(
        {std::clamp(frame_bounds.x(), available.left(),
                    std::max(available.left(), available.right() - frame_bounds.width() + 1)),
         std::clamp(frame_bounds.y(), available.top(),
                    std::max(available.top(), available.bottom() - frame_bounds.height() + 1))});
}
#endif
} // namespace cc::ui
