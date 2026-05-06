#pragma once
#include <QProxyStyle>
#include <QStyleOptionTab>
#include <QPainter>

/// Forces all QTabBars to never elide (truncate) tab text.
/// Overrides style hint, size calculation, and drawing to ensure
/// full tab text is always visible. Scroll buttons handle overflow.
class NoElideTabStyle : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;

    int styleHint(StyleHint hint, const QStyleOption* option = nullptr,
                  const QWidget* widget = nullptr,
                  QStyleHintReturn* returnData = nullptr) const override
    {
        if (hint == SH_TabBar_ElideMode)
            return Qt::ElideNone;
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }

    QSize sizeFromContents(ContentsType type, const QStyleOption* option,
                           const QSize& contentsSize,
                           const QWidget* widget) const override
    {
        QSize sz = QProxyStyle::sizeFromContents(type, option, contentsSize, widget);
        if (type == CT_TabBarTab) {
            if (auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option)) {
                int textWidth = tab->fontMetrics.horizontalAdvance(tab->text);
                int iconWidth = tab->icon.isNull() ? 0 : 20;
                int padding   = 24 + iconWidth;
                int needed    = textWidth + padding;
                if (sz.width() < needed)
                    sz.setWidth(needed);
            }
        }
        return sz;
    }

    void drawControl(ControlElement element, const QStyleOption* option,
                     QPainter* painter, const QWidget* widget = nullptr) const override
    {
        if (element == CE_TabBarTabLabel) {
            if (auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option)) {
                painter->save();

                QRect textRect = tab->rect.adjusted(8, 0, -8, 0);

                if (!tab->icon.isNull()) {
                    QSize iconSize = tab->iconSize;
                    if (!iconSize.isValid()) iconSize = QSize(16, 16);
                    QRect iconRect(textRect.left(), textRect.center().y() - iconSize.height() / 2,
                                   iconSize.width(), iconSize.height());
                    tab->icon.paint(painter, iconRect);
                    textRect.setLeft(iconRect.right() + 4);
                }

                QColor textColor = tab->palette.color(
                    (tab->state & QStyle::State_Selected) ? QPalette::WindowText : QPalette::WindowText);
                if (!(tab->state & QStyle::State_Enabled))
                    textColor = tab->palette.color(QPalette::Disabled, QPalette::WindowText);
                painter->setPen(textColor);
                if (widget) painter->setFont(widget->font());
                painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextShowMnemonic, tab->text);

                painter->restore();
                return;
            }
        }
        QProxyStyle::drawControl(element, option, painter, widget);
    }
};
