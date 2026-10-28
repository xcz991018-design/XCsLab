#ifndef CARDINAL_SELECTOR_DIALOG_HPP
#define CARDINAL_SELECTOR_DIALOG_HPP

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QKeyEvent>
#include <opencv2/opencv.hpp>
#include <vector>
#include <utility>

#include "GaugeProfile.hpp"
#include "LiquidSurface.hpp"
#include "MinorTickPreview.hpp"

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
    QJsonArray ocrNumbers() const { return m_ocrNumbers; }
    void setMinorPreview(const MinorTickPreview::Result& result) {m_minorPreview=result;update();}
    MinorTickPreview::Result minorPreview() const {return m_minorPreview;}
    void clearMinorPreview() {m_minorPreview={};update();}
    void setOcrNumbers(const QJsonArray& numbers) {m_ocrNumbers=numbers;update();}
    void setScalePoints(const QJsonArray& points) { m_scalePoints=points; clearMinorPreview(); clearScaleSelection(); update(); }
    void setScaleRange(double minimum,double maximum) {m_rangeMinimum=minimum;m_rangeMaximum=maximum;m_hasScaleRange=maximum>minimum;update();}
    void clearScaleSelection() { m_selectedScale=-1; emit scaleSelectionChanged(); update(); }
    bool hasSelectedIntermediateScale() const { return m_selectedScale>=0 && m_selectedScale<m_scalePoints.size() && m_scalePoints[m_selectedScale].toObject()["ratio"].toDouble()>0 && m_scalePoints[m_selectedScale].toObject()["ratio"].toDouble()<1; }
    void deleteSelectedIntermediateScale();
    void selectScaleRatio(double ratio) { m_pendingRatio=ratio; m_pickCenter=false; m_pickLiquidSurface=false; m_liquidClicks.clear(); if(ratio>=0)setCursor(Qt::CrossCursor);else unsetCursor(); emit pickModeChanged(); }
    void selectCenter() { m_pickLiquidSurface=false; m_liquidClicks.clear(); m_pickCenter=true; m_pendingRatio=-1; setCursor(Qt::CrossCursor); emit pickModeChanged(); }
    bool pickingCenter() const { return m_pickCenter; }
    double pendingScaleRatio() const { return m_pendingRatio; }
    void setLiquidSurface(const LiquidSurface& surface) {m_liquidSurface=surface; clearMinorPreview(); update();}
    LiquidSurface liquidSurface() const {return m_liquidSurface;}
    void showLiquidMask(bool enabled) {m_showLiquidMask=enabled; clearMinorPreview(); update();}
    void selectLiquidSurface() {m_pickLiquidSurface=true;m_pickCenter=false;m_pendingRatio=-1;m_liquidClicks.clear();setCursor(Qt::CrossCursor);emit pickModeChanged();}
    void setRectifiedMode(bool enabled) { m_rectifiedMode=enabled; m_selectedIdx=-1; }



signals:
    void pointsChanged();
    void centerClicked();
    void liquidSurfaceChanged();
    void pickModeChanged();
    void scaleSelectionChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    bool m_rectifiedMode{false};
    bool m_pickCenter{false};
    bool m_pickLiquidSurface{false}, m_showLiquidMask{true};
    std::vector<cv::Point2f> m_liquidClicks;
    LiquidSurface m_liquidSurface;
    MinorTickPreview::Result m_minorPreview;
    QJsonArray m_scalePoints;
    double m_rangeMinimum{0},m_rangeMaximum{1};
    bool m_hasScaleRange{false};
    QJsonArray m_ocrNumbers;
    double m_pendingRatio{-1};
    cv::Mat m_origImage;
    std::vector<cv::Point2f> m_pts; // 4 points [12H, 3H, 6H, 9H]
    cv::Point2f m_centerPt{-1.0f, -1.0f}; // 5th point: Dial Center
    double m_pointerBaseRadius{50.0};     // Deadzone radius around center pin (px)
    int m_selectedIdx{-1};
    int m_selectedScale{-1};
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
    CardinalSelectorDialog(const cv::Mat& frame, GaugeProfile& profile, const QString& cameraId = "", QWidget *parent = nullptr, bool historicalSession = false);
    ~CardinalSelectorDialog() override = default;
    void reject() override;

private slots:
    void onPointsUpdated();
    void onAutoDetect();
    void onSaveAndApply();
    void onConfirmPerspective();
    void onDetectLiquidSurface();
    void onAutoScaleCalibration();
    void onMinorTickPreview();


private:
    void setupUi();
    void updatePreview();
    void setManualEditing(bool enabled);
    void setAutoDetectionBusy(bool busy);
    void startAutoDetection(bool cornersOnly);
    std::pair<cv::Point2f,QJsonArray> sourceAnnotations() const;

    cv::Mat m_rawFrame;
    cv::Mat m_frame;
    GaugeProfile& m_profile;
    QString m_cameraId;
    std::vector<cv::Point2f> m_currentPts;

    InteractiveImageLabel *m_interactiveLabel{nullptr};
    InteractiveImageLabel *m_previewLabel{nullptr};
    QPushButton* m_confirmPerspectiveBtn{nullptr};
    QPushButton* m_centerBtn{nullptr};
    QPushButton* m_autoScaleBtn{nullptr};
    QDoubleSpinBox *m_rangeMinimum{nullptr},*m_rangeMaximum{nullptr};
    bool m_rangeConfirmed{false};
    bool m_autoEndpointCalibration{false};
    bool m_endpointReviewRequired{false};
    cv::Mat m_inverseHomography;
    bool m_perspectiveConfirmed{false};
    bool m_centerConfirmed{false};
    cv::Point2f m_sourceCenter{-1,-1};
    cv::Point2f m_graduationSourceCenter{-1,-1};
    QJsonArray m_sourceScalePoints;
    QDoubleSpinBox *m_baseRadiusSpinBox{nullptr};
    QPushButton *m_autoBtn{nullptr};
    QPushButton *m_saveBtn{nullptr};
    QPushButton *m_cancelBtn{nullptr};
    QLabel *m_saveStatus{nullptr};
    QWidget* m_scaleControls{nullptr};
    QLabel* m_scaleStatus{nullptr};
    QPushButton* m_minorPreviewBtn{nullptr};
    QDoubleSpinBox* m_minorDivision{nullptr};
    QCheckBox* m_saveMinorTicks{nullptr};
    QLabel* m_geometryStatus{nullptr};
    QCheckBox* m_geometryAcknowledged{nullptr};
    QByteArray m_geometryReviewSignature;
    QWidget* m_liquidControls{nullptr};
    QCheckBox* m_liquidMaskEnabled{nullptr};
    bool m_manualLiquidSurface{false};
    QString m_liquidNote;
    bool m_autoDialReviewPending{false};
    bool m_saving{false};
    bool m_autoDetectRunning{false};
    QProgressBar* m_autoProgress{nullptr};
    QLabel* m_autoProgressLabel{nullptr};
    bool m_historicalSession{false};
    bool m_automaticReviewMode{false};
    bool m_automaticAttemptFailed{false};
    QPushButton* m_manualEditBtn{nullptr};
    std::vector<QWidget*> m_manualWidgets;
};

#endif // CARDINAL_SELECTOR_DIALOG_HPP
