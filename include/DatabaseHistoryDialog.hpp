#pragma once
#include "MySqlRepository.hpp"
#include <QDialog>
#include <QThreadPool>
#include <functional>
class QLineEdit;
class QDateTimeEdit;
class QTableWidget;
class QPushButton;
class QLabel;
class DatabaseHistoryDialog : public QDialog {
public:
    explicit DatabaseHistoryDialog(QWidget* parent,
        std::function<void(InspectionTask)> analyzeOriginal);
    ~DatabaseHistoryDialog() override;
private:
    void query();
    void viewOriginal();
    void showOriginal(const StoredImage& original, const QImage& image);
    QThreadPool m_pool;
    QLineEdit* m_camera;
    QDateTimeEdit *m_from, *m_until;
    QTableWidget* m_table;
    QPushButton *m_search, *m_previous, *m_next, *m_view;
    QLabel* m_status;
    QList<StoredInspection> m_rows;
    int m_page{0};
    std::function<void(InspectionTask)> m_analyze;
};
