#include "CardinalSelectorDialog.hpp"
#include "GaugeReaderEngine.hpp"
#include "DeviceConfigManager.hpp"
#include <QPainter>
#include <QMessageBox>
#include <QDebug>
#include <cmath>

InteractiveImageLabel::InteractiveImageLabel(QWidget *parent) : QLabel(parent) {
    setMouseTracking(true);
    setMinimumSize(540, 480);
    setStyleSheet("background-color: #1a1a22; border: 1px solid #333344; border-radius: 4px;");
}

void InteractiveImageLabel::setImageAndPoints(const cv::Mat& mat, const std::vector<cv::Point2f>& pts, const cv::Point2f& centerPt) {
    m_origImage = mat.clone();
    m_pts = pts;
    if (m_pts.size() != 4 && !mat.empty()) {
        m_pts = GaugeReaderEngine::autoDetectCardinalPoints(mat);
    }

    if (centerPt.x > 0 && centerPt.y > 0 && !mat.empty() && centerPt.x < mat.cols && centerPt.y < mat.rows) {
        m_centerPt = centerPt;
    } else if (m_pts.size() == 4) {
        m_centerPt = cv::Point2f((m_pts[0].x + m_pts[1].x + m_pts[2].x + m_pts[3].x) * 0.25f,
                                (m_pts[0].y + m_pts[1].y + m_pts[2].y + m_pts[3].y) * 0.25f);
    } else if (!mat.empty()) {
        m_centerPt = cv::Point2f(mat.cols * 0.5f, mat.rows * 0.5f);
    }
    update();
}

void InteractiveImageLabel::updateTransformParams() {
    if (m_origImage.empty() || width() <= 0 || height() <= 0) return;

    float scaleX = static_cast<float>(width()) / m_origImage.cols;
    float scaleY = static_cast<float>(height()) / m_origImage.rows;
    m_scale = std::min(scaleX, scaleY);

    int dispW = static_cast<int>(m_origImage.cols * m_scale);
    int dispH = static_cast<int>(m_origImage.rows * m_scale);

    m_offsetX = (width() - dispW) / 2;
    m_offsetY = (height() - dispH) / 2;
}

QPointF InteractiveImageLabel::mapToCanvas(const cv::Point2f& imgPt) const {
    return QPointF(m_offsetX + imgPt.x * m_scale, m_offsetY + imgPt.y * m_scale);
}

cv::Point2f InteractiveImageLabel::mapToImage(const QPointF& canvasPt) const {
    float x = (canvasPt.x() - m_offsetX) / m_scale;
    float y = (canvasPt.y() - m_offsetY) / m_scale;
    if (!m_origImage.empty()) {
        x = std::max(0.0f, std::min(x, static_cast<float>(m_origImage.cols - 1)));
        y = std::max(0.0f, std::min(y, static_cast<float>(m_origImage.rows - 1)));
    }
    return cv::Point2f(x, y);
}

void InteractiveImageLabel::paintEvent(QPaintEvent *event) {
    QLabel::paintEvent(event);
    if (m_origImage.empty()) return;

    updateTransformParams();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Draw background original image scaled
    int dispW = static_cast<int>(m_origImage.cols * m_scale);
    int dispH = static_cast<int>(m_origImage.rows * m_scale);

    QImage qimg(m_origImage.data, m_origImage.cols, m_origImage.rows, static_cast<int>(m_origImage.step), QImage::Format_BGR888);
    painter.drawImage(QRect(m_offsetX, m_offsetY, dispW, dispH), qimg);

    if (m_pts.size() != 4) return;

    // Map 4 rim points to canvas coordinates
    QVector<QPointF> canvasPts;
    for (const auto& p : m_pts) {
        canvasPts.append(mapToCanvas(p));
    }

    // Draw connecting polygon
    QPen polyPen(QColor(0, 230, 255, 200), 1.5, Qt::SolidLine);
    painter.setPen(polyPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(canvasPts.data(), canvasPts.size());

    // Color theme matching fov_corrector.py [12H, 3H, 6H, 9H]
    QColor colors[4] = {
        QColor(0, 230, 255), // 12H Top
        QColor(255, 60, 60), // 3H Right
        QColor(255, 120, 0), // 6H Bottom
        QColor(255, 60, 255) // 9H Left
    };
    QString labels[4] = { "12H (Top)", "3H (Right)", "6H (Bot)", "9H (Left)" };

    QFont labelFont = painter.font();
    labelFont.setBold(true);
    labelFont.setPixelSize(11);

    for (int i = 0; i < 4; ++i) {
        QPointF cp = canvasPts[i];

        // 1. Fine 1px crosshair reticle (shadow + white line for crisp contrast)
        painter.setPen(QPen(QColor(0, 0, 0, 160), 3, Qt::SolidLine, Qt::SquareCap));
        painter.drawLine(cp + QPointF(-8, 0), cp + QPointF(8, 0));
        painter.drawLine(cp + QPointF(0, -8), cp + QPointF(0, 8));

        painter.setPen(QPen(QColor(255, 255, 255, 220), 1, Qt::SolidLine, Qt::SquareCap));
        painter.drawLine(cp + QPointF(-8, 0), cp + QPointF(8, 0));
        painter.drawLine(cp + QPointF(0, -8), cp + QPointF(0, 8));

        // 2. Small precision drag point circle (Radius: 4px)
        painter.setPen(QPen(Qt::white, 1.2));
        painter.setBrush(colors[i]);
        painter.drawEllipse(cp, 4, 4);

        // 3. Center pixel dot
        painter.setPen(QPen(Qt::black, 1));
        painter.drawPoint(cp);

        // 4. Label tag with drop shadow
        painter.setFont(labelFont);
        painter.setPen(QColor(0, 0, 0, 200));
        painter.drawText(cp + QPointF(-19, -9), labels[i]);
        painter.setPen(Qt::white);
        painter.drawText(cp + QPointF(-20, -10), labels[i]);
    }

    // Draw 5th point (Dial Center Pin 🎯)
    if (m_centerPt.x > 0 && m_centerPt.y > 0) {
        QPointF centerCp = mapToCanvas(m_centerPt);

        // 1. Fine red crosshair reticle (shadow + red line)
        painter.setPen(QPen(QColor(0, 0, 0, 160), 3, Qt::SolidLine, Qt::SquareCap));
        painter.drawLine(centerCp + QPointF(-10, 0), centerCp + QPointF(10, 0));
        painter.drawLine(centerCp + QPointF(0, -10), centerCp + QPointF(0, 10));

        painter.setPen(QPen(QColor(255, 50, 50), 1.2, Qt::SolidLine, Qt::SquareCap));
        painter.drawLine(centerCp + QPointF(-10, 0), centerCp + QPointF(10, 0));
        painter.drawLine(centerCp + QPointF(0, -10), centerCp + QPointF(0, 10));

        // 2. Small precision center drag point circle (Radius: 4px, Gold fill)
        painter.setPen(QPen(Qt::white, 1.2));
        painter.setBrush(QColor(255, 215, 0));
        painter.drawEllipse(centerCp, 4, 4);

        // 3. Center pixel dot
        painter.setPen(QPen(Qt::black, 1));
        painter.drawPoint(centerCp);

        // 4. Label tag with drop shadow for Center Point
        painter.setFont(labelFont);
        painter.setPen(QColor(0, 0, 0, 200));
        painter.drawText(centerCp + QPointF(-29, -9), tr("🎯 表盘中心"));
        painter.setPen(QColor(255, 235, 50));
        painter.drawText(centerCp + QPointF(-30, -10), tr("🎯 表盘中心"));

        // 5. Center Hub Exclusion Deadzone Circle (pointerBaseRadius)
        float baseR_canvas = static_cast<float>(m_pointerBaseRadius * m_scale);
        if (baseR_canvas > 1.0f) {
            painter.setPen(QPen(QColor(255, 220, 0, 220), 1.5, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(centerCp, baseR_canvas, baseR_canvas);

            painter.setFont(labelFont);
            QString rStr = QString("R=%1px (轴帽死区)").arg(m_pointerBaseRadius, 0, 'f', 0);
            painter.setPen(QColor(0, 0, 0, 200));
            painter.drawText(centerCp + QPointF(baseR_canvas + 4, 4), rStr);
            painter.setPen(QColor(255, 220, 0));
            painter.drawText(centerCp + QPointF(baseR_canvas + 3, 3), rStr);
        }
    }
}

void InteractiveImageLabel::mousePressEvent(QMouseEvent *event) {
    if (m_pts.size() != 4) return;
    QPointF pos = event->localPos();

    float hitRadius = 20.0f;
    m_selectedIdx = -1;

    // Check 4 rim points
    for (int i = 0; i < 4; ++i) {
        QPointF cp = mapToCanvas(m_pts[i]);
        float dist = std::hypot(pos.x() - cp.x(), pos.y() - cp.y());
        if (dist < hitRadius) {
            m_selectedIdx = i;
            return;
        }
    }

    // Check 5th center point (index 4)
    if (m_centerPt.x > 0 && m_centerPt.y > 0) {
        QPointF centerCp = mapToCanvas(m_centerPt);
        float dist = std::hypot(pos.x() - centerCp.x(), pos.y() - centerCp.y());
        if (dist < hitRadius) {
            m_selectedIdx = 4;
        }
    }
}

void InteractiveImageLabel::mouseMoveEvent(QMouseEvent *event) {
    if (m_selectedIdx >= 0 && m_selectedIdx < 4) {
        m_pts[m_selectedIdx] = mapToImage(event->localPos());
        update();
        emit pointsChanged();
    } else if (m_selectedIdx == 4) {
        m_centerPt = mapToImage(event->localPos());
        update();
        emit pointsChanged();
    }
}

void InteractiveImageLabel::mouseReleaseEvent(QMouseEvent *event) {
    Q_UNUSED(event);
    m_selectedIdx = -1;
}

// ---------------- CardinalSelectorDialog ----------------

CardinalSelectorDialog::CardinalSelectorDialog(const cv::Mat& frame, GaugeProfile& profile, const QString& cameraId, QWidget *parent)
    : QDialog(parent), m_frame(frame), m_profile(profile), m_cameraId(cameraId) {
    setupUi();
    
    bool ptsValid = (m_profile.perspective_pts.size() == 4);
    if (ptsValid) {
        for (const auto& p : m_profile.perspective_pts) {
            if (p.x <= 0 || p.x >= m_frame.cols || p.y <= 0 || p.y >= m_frame.rows) {
                ptsValid = false;
                break;
            }
        }
    }

    if (ptsValid) {
        m_currentPts = m_profile.perspective_pts;
    } else {
        m_currentPts = GaugeReaderEngine::autoDetectCardinalPoints(m_frame);
    }

    if (m_profile.center.x > 0 && m_profile.center.y > 0 && m_profile.center.x < m_frame.cols && m_profile.center.y < m_frame.rows) {
        m_currentCenterPt = m_profile.center;
    } else {
        m_currentCenterPt = cv::Point2f((m_currentPts[0].x + m_currentPts[1].x + m_currentPts[2].x + m_currentPts[3].x) * 0.25f,
                                        (m_currentPts[0].y + m_currentPts[1].y + m_currentPts[2].y + m_currentPts[3].y) * 0.25f);
    }

    m_interactiveLabel->setImageAndPoints(m_frame, m_currentPts, m_currentCenterPt);
    m_interactiveLabel->setPointerBaseRadius(m_profile.pointerBaseRadius);
    if (m_baseRadiusSpinBox) {
        m_baseRadiusSpinBox->setValue(m_profile.pointerBaseRadius);
    }
    updatePreview();
}

void CardinalSelectorDialog::setupUi() {
    setWindowTitle(tr("表盘 4 极值切点与中心点视场透视矫正配置 (FOV Cardinal & Center Corrector)"));
    resize(1100, 640);
    setStyleSheet("QDialog { background-color: #121216; color: #e0e0e0; font-family: Segoe UI, sans-serif; }");

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    QLabel *tipLabel = new QLabel(tr("💡 请在左侧原图上拖拽圆盘边缘 4 个极值切点（12点钟、3点钟、6点钟、9点钟位置）及 🎯 表盘中心点，调节黄色轴帽避让半径圈，右侧将实时呈现正视矫正效果。"), this);
    tipLabel->setStyleSheet("font-weight: bold; color: #00e5ff; padding: 6px; font-size: 13px;");
    mainLayout->addWidget(tipLabel);

    QHBoxLayout *contentLayout = new QHBoxLayout();

    // Left: Interactive Selector
    QVBoxLayout *leftLayout = new QVBoxLayout();
    leftLayout->addWidget(new QLabel(tr("<b>原图切点与中心点标注 (Drag 4 Rim Points & 🎯 Center Pin):</b>"), this));
    m_interactiveLabel = new InteractiveImageLabel(this);
    leftLayout->addWidget(m_interactiveLabel, 1);
    contentLayout->addLayout(leftLayout, 1);

    // Right: Live Rectified Preview
    QVBoxLayout *rightLayout = new QVBoxLayout();
    rightLayout->addWidget(new QLabel(tr("<b>正视透视矫正实时预览 (Front View Preview):</b>"), this));
    m_previewLabel = new QLabel(this);
    m_previewLabel->setMinimumSize(400, 400);
    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewLabel->setStyleSheet("background-color: #1a1a22; border: 1px solid #333344; border-radius: 4px;");
    rightLayout->addWidget(m_previewLabel, 1);
    contentLayout->addLayout(rightLayout, 1);

    mainLayout->addLayout(contentLayout, 1);

    // Bottom Action Bar
    QHBoxLayout *btnLayout = new QHBoxLayout();
    m_autoBtn = new QPushButton(tr("⚡ 自动检测切点"), this);
    m_resetBtn = new QPushButton(tr("🔄 重置切点"), this);
    m_saveBtn = new QPushButton(tr("💾 确认并保存到 devices.json"), this);
    m_cancelBtn = new QPushButton(tr("取消"), this);

    m_saveBtn->setStyleSheet("background-color: #00897b; color: white; font-weight: bold; padding: 8px 16px; border-radius: 4px;");
    m_autoBtn->setStyleSheet("background-color: #2a2a38; border: 1px solid #3a3a4c; padding: 8px 16px; border-radius: 4px;");
    m_resetBtn->setStyleSheet("background-color: #2a2a38; border: 1px solid #3a3a4c; padding: 8px 16px; border-radius: 4px;");
    m_cancelBtn->setStyleSheet("background-color: #2a2a38; border: 1px solid #3a3a4c; padding: 8px 16px; border-radius: 4px;");

    QLabel *radiusLabel = new QLabel(tr("🎯 中心轴帽避让半径 (pointerBaseRadius):"), this);
    radiusLabel->setStyleSheet("font-weight: bold; color: #ffd700;");

    m_baseRadiusSpinBox = new QDoubleSpinBox(this);
    m_baseRadiusSpinBox->setRange(5.0, 250.0);
    m_baseRadiusSpinBox->setSingleStep(1.0);
    m_baseRadiusSpinBox->setDecimals(1);
    m_baseRadiusSpinBox->setSuffix(" px");
    m_baseRadiusSpinBox->setValue(m_profile.pointerBaseRadius > 0 ? m_profile.pointerBaseRadius : 50.0);
    m_baseRadiusSpinBox->setStyleSheet("QDoubleSpinBox { background-color: #1a1a22; color: #ffd700; border: 1px solid #ffd700; border-radius: 4px; padding: 4px 8px; font-weight: bold; }");

    btnLayout->addWidget(m_autoBtn);
    btnLayout->addWidget(m_resetBtn);
    btnLayout->addSpacing(15);
    btnLayout->addWidget(radiusLabel);
    btnLayout->addWidget(m_baseRadiusSpinBox);
    btnLayout->addStretch();
    btnLayout->addWidget(m_saveBtn);
    btnLayout->addWidget(m_cancelBtn);

    mainLayout->addLayout(btnLayout);

    // Signals & Slots
    connect(m_interactiveLabel, &InteractiveImageLabel::pointsChanged, this, &CardinalSelectorDialog::onPointsUpdated);
    connect(m_baseRadiusSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double val) {
        m_interactiveLabel->setPointerBaseRadius(val);
        updatePreview();
    });
    connect(m_autoBtn, &QPushButton::clicked, this, &CardinalSelectorDialog::onAutoDetect);
    connect(m_resetBtn, &QPushButton::clicked, this, &CardinalSelectorDialog::onReset);
    connect(m_saveBtn, &QPushButton::clicked, this, &CardinalSelectorDialog::onSaveAndApply);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
}

void CardinalSelectorDialog::onPointsUpdated() {
    m_currentPts = m_interactiveLabel->getPoints();
    m_currentCenterPt = m_interactiveLabel->getCenterPoint();
    updatePreview();
}

void CardinalSelectorDialog::updatePreview() {
    if (m_frame.empty() || m_currentPts.size() != 4) return;

    GaugeReaderEngine engine;
    cv::Mat invH;
    cv::Mat rectified = engine.rectifyPerspective(m_frame, m_currentPts, invH, 800, 40);

    if (!rectified.empty()) {
        cv::Point2f origCenter = m_interactiveLabel->getCenterPoint();
        if (origCenter.x > 0 && origCenter.y > 0 && !invH.empty()) {
            cv::Mat H = invH.inv();
            std::vector<cv::Point2f> srcC = { origCenter };
            std::vector<cv::Point2f> dstC;
            cv::perspectiveTransform(srcC, dstC, H);

            // Overlay transformed center pin on rectified view
            cv::circle(rectified, dstC[0], 6, cv::Scalar(0, 0, 255), -1, cv::LINE_AA);
            cv::drawMarker(rectified, dstC[0], cv::Scalar(0, 255, 255), cv::MARKER_CROSS, 24, 2, cv::LINE_AA);

            // Overlay transformed center hub exclusion circle (pointerBaseRadius)
            double baseR = m_interactiveLabel->getPointerBaseRadius();
            if (baseR > 1.0) {
                cv::circle(rectified, dstC[0], static_cast<int>(baseR), cv::Scalar(255, 220, 0), 2, cv::LINE_AA);
                cv::putText(rectified, cv::format("Hub Mask: %.0fpx", baseR), dstC[0] + cv::Point2f(10, 22), cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(255, 220, 0), 1, cv::LINE_AA);
            }

            cv::putText(rectified, "Center Pin", dstC[0] + cv::Point2f(10, 5), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 255), 1, cv::LINE_AA);
        }

        QImage qimg(rectified.data, rectified.cols, rectified.rows, static_cast<int>(rectified.step), QImage::Format_BGR888);
        m_previewLabel->setPixmap(QPixmap::fromImage(qimg.copy()).scaled(m_previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
}

void CardinalSelectorDialog::onAutoDetect() {
    if (m_frame.empty()) return;
    m_currentPts = GaugeReaderEngine::autoDetectCardinalPoints(m_frame);
    m_currentCenterPt = cv::Point2f((m_currentPts[0].x + m_currentPts[1].x + m_currentPts[2].x + m_currentPts[3].x) * 0.25f,
                                    (m_currentPts[0].y + m_currentPts[1].y + m_currentPts[2].y + m_currentPts[3].y) * 0.25f);
    m_interactiveLabel->setImageAndPoints(m_frame, m_currentPts, m_currentCenterPt);
    updatePreview();
}

void CardinalSelectorDialog::onReset() {
    onAutoDetect();
}

void CardinalSelectorDialog::onSaveAndApply() {
    m_currentPts = m_interactiveLabel->getPoints();
    m_profile.perspective_pts = m_currentPts;
    m_profile.perspective_correction_enabled = true;
    m_profile.center = m_interactiveLabel->getCenterPoint();
    m_profile.pointerBaseRadius = m_interactiveLabel->getPointerBaseRadius();
    m_profile.auto_detect_roi = false;
    if (!m_frame.empty()) {
        m_profile.calibrated_width = m_frame.cols;
        m_profile.calibrated_height = m_frame.rows;
    }

    if (!m_cameraId.isEmpty()) {
        double p0x = m_currentPts.size() > 0 ? m_currentPts[0].x : 0;
        double p0y = m_currentPts.size() > 0 ? m_currentPts[0].y : 0;
        double p1x = m_currentPts.size() > 1 ? m_currentPts[1].x : 0;
        double p1y = m_currentPts.size() > 1 ? m_currentPts[1].y : 0;
        double p2x = m_currentPts.size() > 2 ? m_currentPts[2].x : 0;
        double p2y = m_currentPts.size() > 2 ? m_currentPts[2].y : 0;
        double p3x = m_currentPts.size() > 3 ? m_currentPts[3].x : 0;
        double p3y = m_currentPts.size() > 3 ? m_currentPts[3].y : 0;
        cv::Point2f cp = m_interactiveLabel->getCenterPoint();

        DeviceConfigManager::instance().updateDeviceCalibration(
            m_cameraId,
            true,
            !m_frame.empty() ? m_frame.cols : 800,
            !m_frame.empty() ? m_frame.rows : 448,
            p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y,
            cp.x, cp.y,
            m_interactiveLabel->getPointerBaseRadius(),
            true
        );
        QMessageBox::information(this, tr("保存成功"), tr("已成功更新摄像头 [%1] 的 4 切点与透视标定数据并保存到 devices.json！").arg(m_cameraId));
    } else {
        QMessageBox::information(this, tr("设置成功"), tr("已在当前内存会话中应用标定参数！"));
    }
    accept();
}
