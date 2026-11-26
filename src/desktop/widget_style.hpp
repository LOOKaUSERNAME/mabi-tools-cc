#pragma once
#include <QProxyStyle>
#include <QStyleFactory>

namespace cc::ui {
class widget_style final : public QProxyStyle {
public:
    widget_style() : QProxyStyle(QStyleFactory::create("Fusion")) {}

    QRect subControlRect(ComplexControl control, const QStyleOptionComplex* option, SubControl part,
                         const QWidget* widget = nullptr) const override {
        auto rect = QProxyStyle::subControlRect(control, option, part, widget);
        // Qt anchors the list at the inclusive top/bottom edges of this rect.
        if (control == CC_ComboBox && part == SC_ComboBoxListBoxPopup)
            rect.adjust(0, -1, 0, 1);
        return rect;
    }

    int styleHint(StyleHint hint, const QStyleOption* option = nullptr,
                  const QWidget* widget = nullptr,
                  QStyleHintReturn* data = nullptr) const override {
        // Use a Windows-style list below (or above) the field. Menu-style
        // popups align the selected row over the field and obscure its value.
        if (hint == SH_ComboBox_Popup)
            return 0;
        return QProxyStyle::styleHint(hint, option, widget, data);
    }
};
}
