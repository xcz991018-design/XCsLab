#ifndef CARDINAL_SELECTOR_DIALOG_HPP
#define CARDINAL_SELECTOR_DIALOG_HPP

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPaintEvent>
#include <opencv2/opencv.hpp>
#include <vector>

#include "GaugeProfile.hpp"

class InteractiveImageLabel : public QLabel {
    Q_OBJECT

public:
    explicit InteractiveImageLabel(QWidget *parent = nullptr);
    void setImageAndPoints(const cv::Mat& mat, const std::vector<cv::Point2f>& pts, const cv::Point2f& centerPt = cv::Point2f(-1.0f, -1.0f));
    std::vector<cv::Point2f> getPoints() const { return m_pts; }
    cv::Point2f getCenterPoint() const { return m_centerPt; }
    void setPointerBaseRadius(double r) { m_pointerBaseRadius = r; update(); }
    double getPointerBaseRadius() const { return m_pointerBaseRadius; }
    QJsonArray getScalePoints() const { return m_scalePoints; }
    void setScalePoints(const QJsonArray& points) { m_scalePoints=points; update(); }
    void selectScaleRatio(double ratio) { m_pendingRatio=ratio; m_pickCenter=false; }
    void selectCenter() { m_pickCenter=true; m_pendingRatio=-1; setCursor(Qt::CrossCursor); }
    void setRectifiedMode(bool enabled) { m_rectifiedMode=enabled; m_selectedIdx=-1; }



signals:
    void pointsChanged();
    void centerClicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    bool m_rectifiedMode{false};
    bool m_pickCenter{false};
    QJsonArray m_scalePoints;
    double m_pendingRatio{-1};
    cv::Mat m_origImage;
    std::vector<cv::Point2f> m_pts; // 4 points [12H, 3H, 6H, 9H]
    cv::Point2f m_centerPt{-1.0f, -1.0f}; // 5th point: Dial Center
    double m_pointerBaseRadius{50.0};     // Deadzone radius around center pin (px)
    int m_selectedIdx{-1};
    float m_scale{1.0f};
    int m_offsetX{0};
    int m_offsetY{0};

    void updateTransformParams();
    QPointF mapToCanvas(const cv::Point2f& imgPt) const;
    cv::Point2f mapToImage(const QPointF& canvasPt) const;
};

class CardinalSelectorDialog : public QDialog {
    Q_OBJECT

public:
    CardinalSelectorDialog(const cv::Mat& frame, GaugeProfile& profile, const QString& cameraId = "", QWidget *parent = nullptr);
    ~CardinalSelectorDialog() override = default;
    void reject() override;

private slots:
    void onPointsUpdated();
    void onAutoDetect();
    void onReset();
    void onSaveAndApply();
    void onConfirmPerspective();

private:
    void setupUi();
    void updatePreview();

    cv::Mat m_frame;
    GaugeProfile& m_profile;
    QString m_cameraId;
    std::vector<cv::Point2f> m_currentPts;

    InteractiveImageLabel *m_interactiveLabel{nullptr};
    InteractiveImageLabel *m_previewLabel{nullptr};
    QPushButton* m_confirmPerspectiveBtn{nullptr};
    QPushButton* m_centerBtn{nullptr};
    cv::Mat m_inverseHomography;
    bool m_perspectiveConfirmed{false};
    bool m_centerConfirmed{false};
    bool m_loadExistingPoints{true};
    QDoubleSpinBox *m_baseRadiusSpinBox{nullptr};
    QPushButton *m_autoBtn{nullptr};
    QPushButton *m_resetBtn{nullptr};
    QPushButton *m_saveBtn{nullptr};
    QPushButton *m_cancelBtn{nullptr};
    QLabel *m_saveStatus{nullptr};
    QWidget* m_scaleControls{nullptr};
    QLabel* m_scaleStatus{nullptr};
    bool m_saving{false};
};

#endif // CARDINAL_SELECTOR_DIALOG_HPP
