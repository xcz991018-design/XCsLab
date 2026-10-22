#pragma once
#include <QString>
#include <QTableView>

namespace UiTheme {
inline QString scrollBarStyle() {
    return QString::fromLatin1(R"(
        QScrollBar:vertical { background: #12131a; width: 12px; margin: 0; border: none; }
        QScrollBar:horizontal { background: #12131a; height: 12px; margin: 0; border: none; }
        QScrollBar::handle { background: #4b526b; border-radius: 5px; }
        QScrollBar::handle:vertical { min-height: 28px; }
        QScrollBar::handle:horizontal { min-width: 28px; }
        QScrollBar::handle:hover { background: #68728e; }
        QScrollBar::handle:pressed { background: #008c99; }
        QScrollBar::handle:disabled { background: #33384c; }
        QScrollBar::add-line, QScrollBar::sub-line { background: #12131a; border: none; width: 0; height: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: #12131a; }
        QAbstractScrollArea::corner { background: #12131a; border: none; }
    )");
}
inline QString tableStyle() {
    return QString::fromLatin1(R"(
        QTableView {
            background-color: #14151c; alternate-background-color: #1a1b26;
            color: #e0e0e0; gridline-color: #272a38;
            border: 1px solid #272a38; border-radius: 4px; font-size: 13px;
            selection-background-color: #005b64; selection-color: #ffffff;
        }
        QTableView::item:selected { background-color: #005b64; color: #ffffff; }
        QTableView::item:selected:!active { background-color: #005b64; color: #ffffff; }
        QHeaderView { background-color: #1f2230; color: #d1d5db; }
        QHeaderView::section {
            background-color: #1f2230; color: #00e5ff; font-weight: bold;
            padding: 6px; border: 1px solid #272a38;
        }
        QHeaderView::section:vertical { background-color: #1f2230; color: #d1d5db; }
        QTableCornerButton::section { background-color: #1f2230; border: 1px solid #272a38; }
    )");
}
inline QString scrollAndTableStyle() { return scrollBarStyle()+tableStyle(); }
inline void applyTable(QTableView* table) {
    // A local rule takes precedence over legacy dialog/parent styles.
    table->setStyleSheet(scrollAndTableStyle());
    table->setAlternatingRowColors(true);
}
}
