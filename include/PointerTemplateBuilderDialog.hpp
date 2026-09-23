#ifndef POINTER_TEMPLATE_BUILDER_DIALOG_HPP
#define POINTER_TEMPLATE_BUILDER_DIALOG_HPP

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QMouseEvent>
#include <QPaintEvent>
#include <opencv2/opencv.hpp>
#include <vector>

#include "GaugeProfile.hpp"
#include "PointerDetector.hpp"

/**
 * @brief Calibration Step State Machine for Interactive Template Builder.
 */
enum class BuilderStep {
    SetCenter = 0, // Step 1: Click Dial Center Pin 🎯 (1 point)
    SetTip = 1,    // Step 2: Freely click Needle Tip points ▲ (1~N points: vertex & base)
    SetBody = 3,   // Step 3: Freely click Needle Body side edge points █ (2~N points)
    SetTail = 4,   // Step 4: Freely click Needle Tail counterweight points ▄ (2~N points)
    Completed = 5  // Step 5: Calibration Complete (Free Drag, Point Edit & Fine Tuning)
};

/**
 * @brief Selected point descriptor for drag-and-drop interaction.
 */
struct DragTarget {
    BuilderStep step{BuilderStep::Completed};
    int index{-1}; // -1 for center, 0..N for multi-points
};

/**
 * @brief Interactive Canvas Label displaying original gauge frame and real-time 2D template overlay.
 */
class InteractiveTemplateLabel : public QLabel {
    Q_OBJECT

public:
    explicit InteractiveTemplateLabel(QWidget *parent = nullptr);

    void setImageAndModel(const cv::Mat& mat, const PointerShapeModel& model);
    void setModel(const PointerShapeModel& model);
    PointerShapeModel getModel() const { return m_model; }

    void setBuilderStep(BuilderStep step);
    BuilderStep getBuilderStep() const { return m_step; }

    void clearCurrentStepPoints();
    void clearAllPoints();
    void undoLastPoint();

    int getPointCount(BuilderStep step) const;

signals:
    void modelChanged(const PointerShapeModel& model);
    void stepChanged(BuilderStep newStep);
    void pointCountsChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    cv::Mat m_origImage;
    PointerShapeModel m_model;
    BuilderStep m_step{BuilderStep::SetCenter};

    // User clicked feature point collections per region
    cv::Point2f m_ptCenter{-1.0f, -1.0f};
    std::vector<cv::Point2f> m_tipPts;
    std::vector<cv::Point2f> m_bodyPts;
    std::vector<cv::Point2f> m_tailPts;

    DragTarget m_dragTarget{BuilderStep::Completed, -1};
    float m_scale{1.0f};
    int m_offsetX{0};
    int m_offsetY{0};

    void updateTransformParams();
    QPointF mapToCanvas(const cv::Point2f& imgPt) const;
    cv::Point2f mapToImage(const QPointF& canvasPt) const;

    void recomputeParametersFromPoints();
};

/**
 * @brief Interactive Pointer Shape Template Calibration & Tuning Dialog.
 */
class PointerTemplateBuilderDialog : public QDialog {
    Q_OBJECT

public:
    PointerTemplateBuilderDialog(
        const cv::Mat& frame,
        GaugeProfile& profile,
        const QString& configPath,
        const QString& cameraId = "",
        QWidget *parent = nullptr);

    ~PointerTemplateBuilderDialog() override = default;

private slots:
    void onModelUpdatedFromCanvas(const PointerShapeModel& model);
    void onStepChangedFromCanvas(BuilderStep newStep);
    void onPointCountsUpdated();

    void onSpinBoxValueChanged();
    void onLoadNewImage();
    void onResetCalibration();
    void onClearCurrentStep();
    void onUndoLastPoint();
    void onSaveAndApply();

private:
    void setupUi();
    void updateSpinBoxControlsFromModel(const PointerShapeModel& model);
    PointerShapeModel getModelFromSpinBoxControls() const;
    void updateStepGuidanceLabel(BuilderStep step);
    void updateStepButtonsState();

    cv::Mat m_frame;
    GaugeProfile& m_profile;
    QString m_configPath;
    QString m_cameraId;
    PointerShapeModel m_currentModel;

    InteractiveTemplateLabel *m_canvasLabel{nullptr};
    QLabel *m_guidanceLabel{nullptr};

    // Region Mode Selector Buttons
    QPushButton *m_btnStepCenter{nullptr};
    QPushButton *m_btnStepTip{nullptr};
    QPushButton *m_btnStepBody{nullptr};
    QPushButton *m_btnStepTail{nullptr};
    QPushButton *m_btnCompleted{nullptr};
    QPushButton *m_btnClearStep{nullptr};
    QPushButton *m_btnUndoPoint{nullptr};
    QPushButton *m_btnReset{nullptr};

    // SpinBoxes for Center & Exclusion
    QDoubleSpinBox *m_spinCenterX{nullptr};
    QDoubleSpinBox *m_spinCenterY{nullptr};
    QDoubleSpinBox *m_spinCenterExclusion{nullptr};

    // SpinBoxes for Tip
    QDoubleSpinBox *m_spinTipMinR{nullptr};
    QDoubleSpinBox *m_spinTipMaxR{nullptr};
    QDoubleSpinBox *m_spinTipBaseWidth{nullptr};

    // SpinBoxes for Body
    QDoubleSpinBox *m_spinBodyMinR{nullptr};
    QDoubleSpinBox *m_spinBodyMaxR{nullptr};
    QDoubleSpinBox *m_spinBodyWidth{nullptr};

    // SpinBoxes for Tail
    QDoubleSpinBox *m_spinTailMinR{nullptr};
    QDoubleSpinBox *m_spinTailMaxR{nullptr};
    QDoubleSpinBox *m_spinTailWidth{nullptr};

    // SpinBoxes for Weights & Sampling
    QDoubleSpinBox *m_spinWTip{nullptr};
    QDoubleSpinBox *m_spinWBody{nullptr};
    QDoubleSpinBox *m_spinWTail{nullptr};
    QDoubleSpinBox *m_spinRadialStep{nullptr};
    QDoubleSpinBox *m_spinDtSigma{nullptr};

    // Action Buttons
    QPushButton *m_btnLoadImage{nullptr};
    QPushButton *m_btnSave{nullptr};
    QPushButton *m_btnCancel{nullptr};

    bool m_updatingSpinBoxes{false};
};

#endif // POINTER_TEMPLATE_BUILDER_DIALOG_HPP
