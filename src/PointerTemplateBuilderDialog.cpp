#include "PointerTemplateBuilderDialog.hpp"
#include <QPainter>
#include <QFileDialog>
#include <QMessageBox>
#include <QDebug>
#include <cmath>
#include <numeric>

InteractiveTemplateLabel::InteractiveTemplateLabel(QWidget *parent) : QLabel(parent) {
    setMouseTracking(true);
    setMinimumSize(560, 500);
    setStyleSheet("background-color: #121319; border: 1px solid #2b2e3e; border-radius: 6px;");
}

void InteractiveTemplateLabel::setImageAndModel(const cv::Mat& mat, const PointerShapeModel& model) {
    m_origImage = mat.clone();
    m_model = model;

    if (m_model.center.x <= 0 || m_model.center.y <= 0) {
        m_model.center = cv::Point2f(mat.cols * 0.5f, mat.rows * 0.5f);
    }
    m_ptCenter = m_model.center;

    // Restore clicked image points if custom contour points exist in model
    m_tipPts.clear();
    m_bodyPts.clear();
    m_tailPts.clear();

    cv::Point2f u(1.0f, 0.0f);
    cv::Point2f n(0.0f, 1.0f);

    if (!m_model.customTipPoints.empty()) {
        for (const auto& localPt : m_model.customTipPoints) {
            m_tipPts.push_back(m_ptCenter + localPt.x * u + localPt.y * n);
        }
    }
    if (!m_model.customBodyPoints.empty()) {
        for (const auto& localPt : m_model.customBodyPoints) {
            m_bodyPts.push_back(m_ptCenter + localPt.x * u + localPt.y * n);
        }
    }
    if (!m_model.customTailPoints.empty()) {
        for (const auto& localPt : m_model.customTailPoints) {
            m_tailPts.push_back(m_ptCenter + localPt.x * u + localPt.y * n);
        }
    }

    // Default synthesis if empty
    if (m_tipPts.empty()) {
        float tipR = m_model.tipMaxRadius > 0 ? m_model.tipMaxRadius : (std::min(mat.cols, mat.rows) * 0.35f);
        float tipMinR = m_model.tipMinRadius > 0 ? m_model.tipMinRadius : (tipR * 0.75f);
        float halfW = m_model.tipBaseWidth > 0 ? (m_model.tipBaseWidth * 0.5f) : 6.0f;

        m_tipPts.push_back(m_ptCenter + cv::Point2f(tipR, 0.0f));
        m_tipPts.push_back(m_ptCenter + cv::Point2f(tipMinR, halfW));
        m_tipPts.push_back(m_ptCenter + cv::Point2f(tipMinR, -halfW));
    }

    if (m_bodyPts.empty()) {
        float bodyR = m_model.bodyMaxRadius > 0 ? m_model.bodyMaxRadius : (std::min(mat.cols, mat.rows) * 0.2f);
        float halfW = m_model.bodyWidth > 0 ? (m_model.bodyWidth * 0.5f) : 6.0f;

        m_bodyPts.push_back(m_ptCenter + cv::Point2f(bodyR, halfW));
        m_bodyPts.push_back(m_ptCenter + cv::Point2f(bodyR, -halfW));
    }

    if (m_tailPts.empty()) {
        float tailR = m_model.tailMaxRadius > 0 ? m_model.tailMaxRadius : (std::min(mat.cols, mat.rows) * 0.12f);
        float halfW = m_model.tailWidth > 0 ? (m_model.tailWidth * 0.5f) : 12.0f;

        m_tailPts.push_back(m_ptCenter + cv::Point2f(-tailR, halfW));
        m_tailPts.push_back(m_ptCenter + cv::Point2f(-tailR, -halfW));
    }

    recomputeParametersFromPoints();
    update();
    emit pointCountsChanged();
}

void InteractiveTemplateLabel::setModel(const PointerShapeModel& model) {
    m_model = model;
    m_ptCenter = m_model.center;
    recomputeParametersFromPoints();
    update();
}

void InteractiveTemplateLabel::setBuilderStep(BuilderStep step) {
    m_step = step;
    update();
    emit stepChanged(m_step);
}

void InteractiveTemplateLabel::clearCurrentStepPoints() {
    if (m_step == BuilderStep::SetTip) {
        m_tipPts.clear();
        m_model.customTipPoints.clear();
    } else if (m_step == BuilderStep::SetBody) {
        m_bodyPts.clear();
        m_model.customBodyPoints.clear();
    } else if (m_step == BuilderStep::SetTail) {
        m_tailPts.clear();
        m_model.customTailPoints.clear();
    } else if (m_step == BuilderStep::SetCenter) {
        m_ptCenter = cv::Point2f(-1.0f, -1.0f);
    }
    recomputeParametersFromPoints();
    update();
    emit pointCountsChanged();
}

void InteractiveTemplateLabel::clearAllPoints() {
    m_step = BuilderStep::SetCenter;
    m_ptCenter = cv::Point2f(-1.0f, -1.0f);
    m_tipPts.clear();
    m_bodyPts.clear();
    m_tailPts.clear();
    m_model.customTipPoints.clear();
    m_model.customBodyPoints.clear();
    m_model.customTailPoints.clear();
    recomputeParametersFromPoints();
    update();
    emit stepChanged(m_step);
    emit pointCountsChanged();
}

void InteractiveTemplateLabel::undoLastPoint() {
    if (m_step == BuilderStep::SetTip && !m_tipPts.empty()) {
        m_tipPts.pop_back();
    } else if (m_step == BuilderStep::SetBody && !m_bodyPts.empty()) {
        m_bodyPts.pop_back();
    } else if (m_step == BuilderStep::SetTail && !m_tailPts.empty()) {
        m_tailPts.pop_back();
    } else if (m_step == BuilderStep::SetCenter) {
        m_ptCenter = cv::Point2f(-1.0f, -1.0f);
    }
    recomputeParametersFromPoints();
    update();
    emit pointCountsChanged();
}

int InteractiveTemplateLabel::getPointCount(BuilderStep step) const {
    switch (step) {
        case BuilderStep::SetCenter: return (m_ptCenter.x > 0 && m_ptCenter.y > 0) ? 1 : 0;
        case BuilderStep::SetTip: return static_cast<int>(m_tipPts.size());
        case BuilderStep::SetBody: return static_cast<int>(m_bodyPts.size());
        case BuilderStep::SetTail: return static_cast<int>(m_tailPts.size());
        case BuilderStep::Completed: return 1 + static_cast<int>(m_tipPts.size() + m_bodyPts.size() + m_tailPts.size());
    }
    return 0;
}

void InteractiveTemplateLabel::updateTransformParams() {
    if (m_origImage.empty() || width() <= 0 || height() <= 0) return;

    float scaleX = static_cast<float>(width()) / m_origImage.cols;
    float scaleY = static_cast<float>(height()) / m_origImage.rows;
    m_scale = std::min(scaleX, scaleY);

    int dispW = static_cast<int>(m_origImage.cols * m_scale);
    int dispH = static_cast<int>(m_origImage.rows * m_scale);

    m_offsetX = (width() - dispW) / 2;
    m_offsetY = (height() - dispH) / 2;
}

QPointF InteractiveTemplateLabel::mapToCanvas(const cv::Point2f& imgPt) const {
    return QPointF(m_offsetX + imgPt.x * m_scale, m_offsetY + imgPt.y * m_scale);
}

cv::Point2f InteractiveTemplateLabel::mapToImage(const QPointF& canvasPt) const {
    float x = (canvasPt.x() - m_offsetX) / m_scale;
    float y = (canvasPt.y() - m_offsetY) / m_scale;
    if (!m_origImage.empty()) {
        x = std::max(0.0f, std::min(x, static_cast<float>(m_origImage.cols - 1)));
        y = std::max(0.0f, std::min(y, static_cast<float>(m_origImage.rows - 1)));
    }
    return cv::Point2f(x, y);
}

void InteractiveTemplateLabel::recomputeParametersFromPoints() {
    if (m_ptCenter.x < 0 || m_ptCenter.y < 0) return;

    m_model.center = m_ptCenter;

    cv::Point2f u(1.0f, 0.0f);
    cv::Point2f n(0.0f, 1.0f);
    float thetaRad = 0.0f;

    // 1. Determine reference direction vector u from Tip peak point
    if (!m_tipPts.empty()) {
        float maxDist = -1.0f;
        size_t peakIdx = 0;
        for (size_t i = 0; i < m_tipPts.size(); ++i) {
            float d = cv::norm(m_tipPts[i] - m_ptCenter);
            if (d > maxDist) {
                maxDist = d;
                peakIdx = i;
            }
        }

        if (maxDist > 5.0f) {
            cv::Point2f vecPeak = m_tipPts[peakIdx] - m_ptCenter;
            u = vecPeak * (1.0f / maxDist);
            n = cv::Point2f(-u.y, u.x);
            thetaRad = std::atan2(u.y, u.x);
            m_model.tipMaxRadius = maxDist;
        }
    }

    // 2. Convert clicked image points into local template space (relative to Center(0,0) at reference angle theta=0°)
    m_model.customTipPoints.clear();
    for (const auto& pt : m_tipPts) {
        cv::Point2f v = pt - m_ptCenter;
        float rx = v.dot(u);
        float ry = v.dot(n);
        m_model.customTipPoints.push_back(cv::Point2f(rx, ry));
    }

    m_model.customBodyPoints.clear();
    for (const auto& pt : m_bodyPts) {
        cv::Point2f v = pt - m_ptCenter;
        float rx = v.dot(u);
        float ry = v.dot(n);
        m_model.customBodyPoints.push_back(cv::Point2f(rx, ry));
    }

    m_model.customTailPoints.clear();
    for (const auto& pt : m_tailPts) {
        cv::Point2f v = pt - m_ptCenter;
        float rx = v.dot(u);
        float ry = v.dot(n);
        m_model.customTailPoints.push_back(cv::Point2f(rx, ry));
    }

    // 3. Update spinbox fallback parametric bounds for Tip, Body, Tail
    if (!m_model.customTipPoints.empty()) {
        std::vector<float> rxList, ryAbsList;
        for (const auto& pt : m_model.customTipPoints) {
            rxList.push_back(pt.x);
            ryAbsList.push_back(std::abs(pt.y));
        }
        m_model.tipMinRadius = std::max(10.0f, *std::min_element(rxList.begin(), rxList.end()));
        m_model.tipMaxRadius = std::max(m_model.tipMinRadius + 5.0f, *std::max_element(rxList.begin(), rxList.end()));
        m_model.tipBaseWidth = std::max(2.0f, 2.0f * (*std::max_element(ryAbsList.begin(), ryAbsList.end())));
    }

    if (!m_model.customBodyPoints.empty()) {
        std::vector<float> rxList, ryAbsList;
        for (const auto& pt : m_model.customBodyPoints) {
            rxList.push_back(pt.x);
            ryAbsList.push_back(std::abs(pt.y));
        }
        m_model.bodyMinRadius = std::max(10.0f, *std::min_element(rxList.begin(), rxList.end()));
        m_model.bodyMaxRadius = std::max(m_model.bodyMinRadius + 5.0f, *std::max_element(rxList.begin(), rxList.end()));
        m_model.bodyWidth = std::max(2.0f, 2.0f * (*std::max_element(ryAbsList.begin(), ryAbsList.end())));
    }

    if (!m_model.customTailPoints.empty()) {
        std::vector<float> rxList, ryAbsList;
        for (const auto& pt : m_model.customTailPoints) {
            rxList.push_back(-pt.x); // Tail extends backward (-x)
            ryAbsList.push_back(std::abs(pt.y));
        }
        m_model.tailMinRadius = std::max(5.0f, *std::min_element(rxList.begin(), rxList.end()));
        m_model.tailMaxRadius = std::max(m_model.tailMinRadius + 5.0f, *std::max_element(rxList.begin(), rxList.end()));
        m_model.tailWidth = std::max(2.0f, 2.0f * (*std::max_element(ryAbsList.begin(), ryAbsList.end())));
    }

    emit modelChanged(m_model);
}

void InteractiveTemplateLabel::paintEvent(QPaintEvent *event) {
    QLabel::paintEvent(event);
    if (m_origImage.empty()) return;

    updateTransformParams();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // 1. Render Background Image
    int dispW = static_cast<int>(m_origImage.cols * m_scale);
    int dispH = static_cast<int>(m_origImage.rows * m_scale);

    QImage qimg(m_origImage.data, m_origImage.cols, m_origImage.rows, static_cast<int>(m_origImage.step), QImage::Format_BGR888);
    painter.drawImage(QRect(m_offsetX, m_offsetY, dispW, dispH), qimg);

    // 2. Render User Custom Defined Polygon Outline Paths & Normal Vector Ticks
    auto drawCustomPolygonPath = [&](const std::vector<cv::Point2f>& pts, const QColor& color, bool closed) {
        if (pts.size() < 2) return;

        QPolygonF poly;
        for (const auto& pt : pts) {
            poly.append(mapToCanvas(pt));
        }

        // Draw Ultra-fine Polygon boundary (1.0px thin line)
        painter.setPen(QPen(color, 1.0, closed ? Qt::SolidLine : Qt::DashLine));
        painter.setBrush(QColor(color.red(), color.green(), color.blue(), closed ? 18 : 0));
        if (closed) {
            painter.drawPolygon(poly);
        } else {
            painter.drawPolyline(poly);
        }

        // Draw fine normal vector indicators (0.8px thin line, 5px length)
        painter.setPen(QPen(QColor(color.red(), color.green(), color.blue(), 180), 0.8, Qt::SolidLine));
        size_t nSegs = closed ? pts.size() : (pts.size() - 1);
        for (size_t i = 0; i < nSegs; ++i) {
            QPointF p1 = mapToCanvas(pts[i]);
            QPointF p2 = mapToCanvas(pts[(i + 1) % pts.size()]);
            QPointF mid = (p1 + p2) * 0.5;
            QPointF dir = p2 - p1;
            float len = std::hypot(dir.x(), dir.y());
            if (len > 1e-3f) {
                QPointF norm(-dir.y() / len, dir.x() / len);
                painter.drawLine(mid, mid + norm * 5.0);
            }
        }
    };

    drawCustomPolygonPath(m_tipPts, QColor(255, 0, 255), true);  // Magenta for Tip custom shape
    drawCustomPolygonPath(m_bodyPts, QColor(0, 230, 255), false); // Cyan for Body custom edges
    drawCustomPolygonPath(m_tailPts, QColor(255, 140, 0), true); // Orange for Tail custom shape

    // 3. Render Custom Chamfer Template Point Cloud Overlay (Small 1.2px dots)
    if (m_ptCenter.x > 0 && m_ptCenter.y > 0) {
        float thetaRad = 0.0f;
        if (!m_tipPts.empty()) {
            cv::Point2f vecPeak = m_tipPts[0] - m_ptCenter;
            for (const auto& tp : m_tipPts) {
                if (cv::norm(tp - m_ptCenter) > cv::norm(vecPeak)) vecPeak = tp - m_ptCenter;
            }
            thetaRad = std::atan2(vecPeak.y, vecPeak.x);
        }

        PointerShapeModel drawModel = m_model;
        drawModel.center = m_ptCenter;

        std::vector<TemplatePoint> templatePts = PointerDetector::buildUnifiedTemplate(drawModel);
        float cosA = std::cos(thetaRad);
        float sinA = std::sin(thetaRad);

        for (const auto& tp : templatePts) {
            float rx = tp.localPt.x * cosA - tp.localPt.y * sinA;
            float ry = tp.localPt.x * sinA + tp.localPt.y * cosA;

            QPointF canvasPt = mapToCanvas(m_ptCenter + cv::Point2f(rx, ry));

            QColor color;
            if (tp.regionType == RegionType::Tip) {
                color = QColor(255, 0, 255, 200);
            } else if (tp.regionType == RegionType::Body) {
                color = QColor(0, 230, 255, 200);
            } else {
                color = QColor(255, 140, 0, 200);
            }

            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawEllipse(canvasPt, 1.2, 1.2);
        }

        // Center Exclusion Radius Circle (Thin dash)
        QPointF centerCanvas = mapToCanvas(m_ptCenter);
        float rExcl_canvas = m_model.centerExclusionRadius * m_scale;
        if (rExcl_canvas > 1.0f) {
            painter.setPen(QPen(QColor(255, 215, 0, 200), 1.0, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(centerCanvas, rExcl_canvas, rExcl_canvas);
        }
    }

    // 4. Render Precision Interactive Clicked Point Markers & Badges
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(10);
    painter.setFont(font);

    auto drawMarkerPoint = [&](const cv::Point2f& imgPt, const QColor& color, const QString& labelText, bool isActiveRegion) {
        if (imgPt.x <= 0 || imgPt.y <= 0) return;
        QPointF cp = mapToCanvas(imgPt);

        if (isActiveRegion) {
            painter.setPen(QPen(color, 1.0, Qt::DashLine));
            painter.setBrush(QColor(color.red(), color.green(), color.blue(), 25));
            painter.drawEllipse(cp, 7.0, 7.0);
        }

        // Precise Hairline Crosshair (0.8px line, 4px radius)
        painter.setPen(QPen(QColor(0, 0, 0, 220), 2.0, Qt::SolidLine));
        painter.drawLine(cp + QPointF(-4, 0), cp + QPointF(4, 0));
        painter.drawLine(cp + QPointF(0, -4), cp + QPointF(0, 4));

        painter.setPen(QPen(color, 1.0, Qt::SolidLine));
        painter.drawLine(cp + QPointF(-4, 0), cp + QPointF(4, 0));
        painter.drawLine(cp + QPointF(0, -4), cp + QPointF(0, 4));

        // Pinpoint Core Dot (2.0px radius)
        painter.setPen(QPen(Qt::white, 0.8));
        painter.setBrush(color);
        painter.drawEllipse(cp, 2.2, 2.2);

        // Compact Label Badge
        painter.setPen(QColor(0, 0, 0, 220));
        painter.drawText(cp + QPointF(6, -5), labelText);
        painter.setPen(Qt::white);
        painter.drawText(cp + QPointF(5, -6), labelText);
    };

    if (m_ptCenter.x > 0 && m_ptCenter.y > 0) {
        drawMarkerPoint(m_ptCenter, QColor(255, 215, 0), tr("🎯 Center"), m_step == BuilderStep::SetCenter);
    }

    for (size_t i = 0; i < m_tipPts.size(); ++i) {
        drawMarkerPoint(m_tipPts[i], QColor(255, 0, 255), QString("▲ Tip #%1").arg(i + 1), m_step == BuilderStep::SetTip);
    }

    for (size_t i = 0; i < m_bodyPts.size(); ++i) {
        drawMarkerPoint(m_bodyPts[i], QColor(0, 230, 255), QString("█ Body #%1").arg(i + 1), m_step == BuilderStep::SetBody);
    }

    for (size_t i = 0; i < m_tailPts.size(); ++i) {
        drawMarkerPoint(m_tailPts[i], QColor(255, 140, 0), QString("▄ Tail #%1").arg(i + 1), m_step == BuilderStep::SetTail);
    }
}

void InteractiveTemplateLabel::mousePressEvent(QMouseEvent *event) {
    if (m_origImage.empty()) return;
    QPointF canvasPos = event->localPos();
    cv::Point2f imgPos = mapToImage(canvasPos);

    float hitR = 16.0f;
    m_dragTarget = { BuilderStep::Completed, -1 };

    if (m_ptCenter.x > 0 && m_ptCenter.y > 0) {
        QPointF cp = mapToCanvas(m_ptCenter);
        if (std::hypot(canvasPos.x() - cp.x(), canvasPos.y() - cp.y()) < hitR) {
            m_dragTarget = { BuilderStep::SetCenter, -1 };
        }
    }

    if (m_dragTarget.index == -1 && m_dragTarget.step == BuilderStep::Completed) {
        for (size_t i = 0; i < m_tipPts.size(); ++i) {
            QPointF cp = mapToCanvas(m_tipPts[i]);
            if (std::hypot(canvasPos.x() - cp.x(), canvasPos.y() - cp.y()) < hitR) {
                m_dragTarget = { BuilderStep::SetTip, static_cast<int>(i) };
                break;
            }
        }
    }

    if (m_dragTarget.index == -1 && m_dragTarget.step == BuilderStep::Completed) {
        for (size_t i = 0; i < m_bodyPts.size(); ++i) {
            QPointF cp = mapToCanvas(m_bodyPts[i]);
            if (std::hypot(canvasPos.x() - cp.x(), canvasPos.y() - cp.y()) < hitR) {
                m_dragTarget = { BuilderStep::SetBody, static_cast<int>(i) };
                break;
            }
        }
    }

    if (m_dragTarget.index == -1 && m_dragTarget.step == BuilderStep::Completed) {
        for (size_t i = 0; i < m_tailPts.size(); ++i) {
            QPointF cp = mapToCanvas(m_tailPts[i]);
            if (std::hypot(canvasPos.x() - cp.x(), canvasPos.y() - cp.y()) < hitR) {
                m_dragTarget = { BuilderStep::SetTail, static_cast<int>(i) };
                break;
            }
        }
    }

    if (m_dragTarget.step == BuilderStep::Completed && m_dragTarget.index == -1) {
        if (m_step == BuilderStep::SetCenter) {
            m_ptCenter = imgPos;
            m_step = BuilderStep::SetTip;
            emit stepChanged(m_step);
        } else if (m_step == BuilderStep::SetTip) {
            m_tipPts.push_back(imgPos);
        } else if (m_step == BuilderStep::SetBody) {
            m_bodyPts.push_back(imgPos);
        } else if (m_step == BuilderStep::SetTail) {
            m_tailPts.push_back(imgPos);
        }
    }

    recomputeParametersFromPoints();
    update();
    emit pointCountsChanged();
}

void InteractiveTemplateLabel::mouseMoveEvent(QMouseEvent *event) {
    if (m_dragTarget.step != BuilderStep::Completed || m_dragTarget.index != -1) {
        cv::Point2f imgPos = mapToImage(event->localPos());

        if (m_dragTarget.step == BuilderStep::SetCenter) {
            m_ptCenter = imgPos;
        } else if (m_dragTarget.step == BuilderStep::SetTip && m_dragTarget.index >= 0 && m_dragTarget.index < static_cast<int>(m_tipPts.size())) {
            m_tipPts[m_dragTarget.index] = imgPos;
        } else if (m_dragTarget.step == BuilderStep::SetBody && m_dragTarget.index >= 0 && m_dragTarget.index < static_cast<int>(m_bodyPts.size())) {
            m_bodyPts[m_dragTarget.index] = imgPos;
        } else if (m_dragTarget.step == BuilderStep::SetTail && m_dragTarget.index >= 0 && m_dragTarget.index < static_cast<int>(m_tailPts.size())) {
            m_tailPts[m_dragTarget.index] = imgPos;
        }

        recomputeParametersFromPoints();
        update();
    }
}

void InteractiveTemplateLabel::mouseReleaseEvent(QMouseEvent *event) {
    Q_UNUSED(event);
    m_dragTarget = { BuilderStep::Completed, -1 };
}

// ---------------- PointerTemplateBuilderDialog ----------------

PointerTemplateBuilderDialog::PointerTemplateBuilderDialog(
    const cv::Mat& frame,
    GaugeProfile& profile,
    const QString& configPath,
    const QString& cameraId,
    QWidget *parent)
    : QDialog(parent), m_frame(frame), m_profile(profile), m_configPath(configPath), m_cameraId(cameraId) {

    cv::Mat workImg = m_frame;
    if (workImg.empty()) {
        std::string testImgPath = "test_images/111.png";
        workImg = cv::imread(testImgPath);
    }
    if (workImg.empty()) {
        workImg = cv::Mat::zeros(800, 800, CV_8UC3);
    }
    m_frame = workImg;

    m_currentModel = m_profile.toShapeModel();
    setupUi();

    m_canvasLabel->setImageAndModel(m_frame, m_currentModel);
    updateSpinBoxControlsFromModel(m_currentModel);
    updateStepButtonsState();
    updateStepGuidanceLabel(m_canvasLabel->getBuilderStep());
}

void PointerTemplateBuilderDialog::setupUi() {
    setWindowTitle(tr("🛠 交互式 2D 指针任意自定义多边形轮廓标定与参数解算器"));
    resize(1240, 720);
    setStyleSheet("QDialog { background-color: #101117; color: #e0e0e0; font-family: Segoe UI, sans-serif; }");

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(8);

    // Guidance Header
    m_guidanceLabel = new QLabel(this);
    m_guidanceLabel->setStyleSheet("background-color: #171924; border: 1px solid #00e5ff; color: #00e5ff; font-weight: bold; font-size: 13px; padding: 8px; border-radius: 4px;");
    mainLayout->addWidget(m_guidanceLabel);

    QHBoxLayout *contentLayout = new QHBoxLayout();
    contentLayout->setSpacing(10);

    // Left: Interactive Canvas & Step Toolbar
    QVBoxLayout *leftLayout = new QVBoxLayout();
    leftLayout->setSpacing(6);

    QHBoxLayout *stepBar = new QHBoxLayout();
    m_btnStepCenter = new QPushButton(tr("🎯 1.Center (轴心)"), this);
    m_btnStepTip = new QPushButton(tr("▲ 2.Tip (自定义针尖轮廓)"), this);
    m_btnStepBody = new QPushButton(tr("█ 3.Body (自定义针身轮廓)"), this);
    m_btnStepTail = new QPushButton(tr("▄ 4.Tail (自定义针尾轮廓)"), this);
    m_btnCompleted = new QPushButton(tr("✅ 5.完成与微调"), this);

    m_btnClearStep = new QPushButton(tr("🗑️ 清空当前轮廓"), this);
    m_btnUndoPoint = new QPushButton(tr("↩️ 撤销点"), this);
    m_btnReset = new QPushButton(tr("🔄 重置全部"), this);

    QString stepStyle = "QPushButton { background-color: #232736; color: #e0e0e0; border: 1px solid #363b50; font-weight: bold; padding: 5px 10px; border-radius: 3px; }"
                        "QPushButton:hover { background-color: #2f3448; border-color: #00e5ff; }";
    m_btnStepCenter->setStyleSheet(stepStyle);
    m_btnStepTip->setStyleSheet(stepStyle);
    m_btnStepBody->setStyleSheet(stepStyle);
    m_btnStepTail->setStyleSheet(stepStyle);
    m_btnCompleted->setStyleSheet(stepStyle);

    m_btnClearStep->setStyleSheet("QPushButton { background-color: #3b2820; color: #ffab40; border: 1px solid #5a3a2a; font-weight: bold; padding: 5px 8px; border-radius: 3px; }");
    m_btnUndoPoint->setStyleSheet("QPushButton { background-color: #232736; color: #80d8ff; border: 1px solid #363b50; font-weight: bold; padding: 5px 8px; border-radius: 3px; }");
    m_btnReset->setStyleSheet("QPushButton { background-color: #3f2326; color: #ff5252; border: 1px solid #602b30; font-weight: bold; padding: 5px 8px; border-radius: 3px; }");

    stepBar->addWidget(m_btnStepCenter);
    stepBar->addWidget(m_btnStepTip);
    stepBar->addWidget(m_btnStepBody);
    stepBar->addWidget(m_btnStepTail);
    stepBar->addWidget(m_btnCompleted);
    stepBar->addStretch();
    stepBar->addWidget(m_btnClearStep);
    stepBar->addWidget(m_btnUndoPoint);
    stepBar->addWidget(m_btnReset);

    leftLayout->addLayout(stepBar);

    m_canvasLabel = new InteractiveTemplateLabel(this);
    leftLayout->addWidget(m_canvasLabel, 1);
    contentLayout->addLayout(leftLayout, 1);

    // Right: Fine-Tuning Control Panel
    QGroupBox *paramBox = new QGroupBox(tr("⚙️ 2D 轮廓几何参数解算与微调面板"), this);
    paramBox->setStyleSheet(
        "QGroupBox { background-color: #171924; border: 1px solid #2b2e3e; border-radius: 6px; margin-top: 6px; font-weight: bold; color: #00e5ff; }"
        "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; padding: 2px 8px; background-color: #171924; }"
    );

    QVBoxLayout *paramLayout = new QVBoxLayout(paramBox);
    paramLayout->setContentsMargins(10, 14, 10, 10);
    paramLayout->setSpacing(8);

    QGridLayout *grid = new QGridLayout();
    grid->setSpacing(6);

    auto createDoubleSpin = [](double minV, double maxV, double step, double defV, const QString& suffix) -> QDoubleSpinBox* {
        QDoubleSpinBox *sp = new QDoubleSpinBox();
        sp->setRange(minV, maxV);
        sp->setSingleStep(step);
        sp->setDecimals(1);
        sp->setSuffix(suffix);
        sp->setValue(defV);
        sp->setStyleSheet("QDoubleSpinBox { background-color: #0c0d12; color: #00e676; border: 1px solid #232736; border-radius: 3px; padding: 3px 6px; font-weight: bold; }");
        return sp;
    };

    int row = 0;
    grid->addWidget(new QLabel(tr("🎯 Dial Center (X, Y):")), row, 0);
    QHBoxLayout *cBox = new QHBoxLayout();
    m_spinCenterX = createDoubleSpin(0.0, 5000.0, 1.0, m_currentModel.center.x, " px");
    m_spinCenterY = createDoubleSpin(0.0, 5000.0, 1.0, m_currentModel.center.y, " px");
    cBox->addWidget(m_spinCenterX);
    cBox->addWidget(m_spinCenterY);
    grid->addLayout(cBox, row, 1);

    row++;
    grid->addWidget(new QLabel(tr("⭕ Exclusion Radius (死区):")), row, 0);
    m_spinCenterExclusion = createDoubleSpin(0.0, 200.0, 1.0, m_currentModel.centerExclusionRadius, " px");
    grid->addWidget(m_spinCenterExclusion, row, 1);

    row++;
    grid->addWidget(new QLabel(tr("▲ Tip Radius Range (Min~Max):")), row, 0);
    QHBoxLayout *tipRBox = new QHBoxLayout();
    m_spinTipMinR = createDoubleSpin(0.0, 1000.0, 1.0, m_currentModel.tipMinRadius, " px");
    m_spinTipMaxR = createDoubleSpin(0.0, 1000.0, 1.0, m_currentModel.tipMaxRadius, " px");
    tipRBox->addWidget(m_spinTipMinR);
    tipRBox->addWidget(m_spinTipMaxR);
    grid->addLayout(tipRBox, row, 1);

    row++;
    grid->addWidget(new QLabel(tr("▲ Tip Base Width (针尖底宽):")), row, 0);
    m_spinTipBaseWidth = createDoubleSpin(1.0, 100.0, 0.5, m_currentModel.tipBaseWidth, " px");
    grid->addWidget(m_spinTipBaseWidth, row, 1);

    row++;
    grid->addWidget(new QLabel(tr("█ Body Radius Range (Min~Max):")), row, 0);
    QHBoxLayout *bodyRBox = new QHBoxLayout();
    m_spinBodyMinR = createDoubleSpin(0.0, 1000.0, 1.0, m_currentModel.bodyMinRadius, " px");
    m_spinBodyMaxR = createDoubleSpin(0.0, 1000.0, 1.0, m_currentModel.bodyMaxRadius, " px");
    bodyRBox->addWidget(m_spinBodyMinR);
    bodyRBox->addWidget(m_spinBodyMaxR);
    grid->addLayout(bodyRBox, row, 1);

    row++;
    grid->addWidget(new QLabel(tr("█ Body Width (针身宽度):")), row, 0);
    m_spinBodyWidth = createDoubleSpin(1.0, 100.0, 0.5, m_currentModel.bodyWidth, " px");
    grid->addWidget(m_spinBodyWidth, row, 1);

    row++;
    grid->addWidget(new QLabel(tr("▄ Tail Radius Range (Min~Max):")), row, 0);
    QHBoxLayout *tailRBox = new QHBoxLayout();
    m_spinTailMinR = createDoubleSpin(0.0, 500.0, 1.0, m_currentModel.tailMinRadius, " px");
    m_spinTailMaxR = createDoubleSpin(0.0, 500.0, 1.0, m_currentModel.tailMaxRadius, " px");
    tailRBox->addWidget(m_spinTailMinR);
    tailRBox->addWidget(m_spinTailMaxR);
    grid->addLayout(tailRBox, row, 1);

    row++;
    grid->addWidget(new QLabel(tr("▄ Tail Width (粗针尾宽度):")), row, 0);
    m_spinTailWidth = createDoubleSpin(2.0, 150.0, 0.5, m_currentModel.tailWidth, " px");
    grid->addWidget(m_spinTailWidth, row, 1);

    row++;
    grid->addWidget(new QLabel(tr("⚖️ Region Weights (Tip/Body/Tail):")), row, 0);
    QHBoxLayout *wBox = new QHBoxLayout();
    m_spinWTip = createDoubleSpin(0.1, 10.0, 0.1, m_currentModel.wTip, "");
    m_spinWBody = createDoubleSpin(0.1, 10.0, 0.1, m_currentModel.wBody, "");
    m_spinWTail = createDoubleSpin(0.1, 10.0, 0.1, m_currentModel.wTail, "");
    wBox->addWidget(m_spinWTip);
    wBox->addWidget(m_spinWBody);
    wBox->addWidget(m_spinWTail);
    grid->addLayout(wBox, row, 1);

    row++;
    grid->addWidget(new QLabel(tr("🔍 Chamfer Step / DT Sigma:")), row, 0);
    QHBoxLayout *dtBox = new QHBoxLayout();
    m_spinRadialStep = createDoubleSpin(1.0, 20.0, 0.5, m_currentModel.radialStep, " px");
    m_spinDtSigma = createDoubleSpin(1.0, 20.0, 0.5, m_currentModel.dtSigma, " px");
    dtBox->addWidget(m_spinRadialStep);
    dtBox->addWidget(m_spinDtSigma);
    grid->addLayout(dtBox, row, 1);

    paramLayout->addLayout(grid);
    paramLayout->addStretch();
    contentLayout->addWidget(paramBox, 0);

    mainLayout->addLayout(contentLayout, 1);

    // Bottom Actions Bar
    QHBoxLayout *actionLayout = new QHBoxLayout();
    m_btnLoadImage = new QPushButton(tr("📂 打开新仪表照片"), this);
    m_btnSave = new QPushButton(tr("💾 确认并保存到 cameras.json"), this);
    m_btnCancel = new QPushButton(tr("取消 (Cancel)"), this);

    m_btnLoadImage->setStyleSheet("QPushButton { background-color: #232736; color: #00e5ff; border: 1px solid #363b50; font-weight: bold; padding: 8px 16px; border-radius: 4px; }");
    m_btnSave->setStyleSheet("QPushButton { background-color: #00897b; color: white; font-weight: bold; padding: 8px 20px; border-radius: 4px; font-size: 13px; }");
    m_btnCancel->setStyleSheet("QPushButton { background-color: #232736; color: #e0e0e0; border: 1px solid #363b50; padding: 8px 16px; border-radius: 4px; }");

    actionLayout->addWidget(m_btnLoadImage);
    actionLayout->addStretch();
    actionLayout->addWidget(m_btnSave);
    actionLayout->addWidget(m_btnCancel);

    mainLayout->addLayout(actionLayout);

    // Signals & Slots
    connect(m_canvasLabel, &InteractiveTemplateLabel::modelChanged, this, &PointerTemplateBuilderDialog::onModelUpdatedFromCanvas);
    connect(m_canvasLabel, &InteractiveTemplateLabel::stepChanged, this, &PointerTemplateBuilderDialog::onStepChangedFromCanvas);
    connect(m_canvasLabel, &InteractiveTemplateLabel::pointCountsChanged, this, &PointerTemplateBuilderDialog::onPointCountsUpdated);

    connect(m_btnStepCenter, &QPushButton::clicked, this, [this]() { m_canvasLabel->setBuilderStep(BuilderStep::SetCenter); });
    connect(m_btnStepTip, &QPushButton::clicked, this, [this]() { m_canvasLabel->setBuilderStep(BuilderStep::SetTip); });
    connect(m_btnStepBody, &QPushButton::clicked, this, [this]() { m_canvasLabel->setBuilderStep(BuilderStep::SetBody); });
    connect(m_btnStepTail, &QPushButton::clicked, this, [this]() { m_canvasLabel->setBuilderStep(BuilderStep::SetTail); });
    connect(m_btnCompleted, &QPushButton::clicked, this, [this]() { m_canvasLabel->setBuilderStep(BuilderStep::Completed); });

    connect(m_btnClearStep, &QPushButton::clicked, this, &PointerTemplateBuilderDialog::onClearCurrentStep);
    connect(m_btnUndoPoint, &QPushButton::clicked, this, &PointerTemplateBuilderDialog::onUndoLastPoint);
    connect(m_btnReset, &QPushButton::clicked, this, &PointerTemplateBuilderDialog::onResetCalibration);

    auto spinConnect = [this](QDoubleSpinBox *sp) {
        connect(sp, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &PointerTemplateBuilderDialog::onSpinBoxValueChanged);
    };

    spinConnect(m_spinCenterX); spinConnect(m_spinCenterY); spinConnect(m_spinCenterExclusion);
    spinConnect(m_spinTipMinR); spinConnect(m_spinTipMaxR); spinConnect(m_spinTipBaseWidth);
    spinConnect(m_spinBodyMinR); spinConnect(m_spinBodyMaxR); spinConnect(m_spinBodyWidth);
    spinConnect(m_spinTailMinR); spinConnect(m_spinTailMaxR); spinConnect(m_spinTailWidth);
    spinConnect(m_spinWTip); spinConnect(m_spinWBody); spinConnect(m_spinWTail);
    spinConnect(m_spinRadialStep); spinConnect(m_spinDtSigma);

    connect(m_btnLoadImage, &QPushButton::clicked, this, &PointerTemplateBuilderDialog::onLoadNewImage);
    connect(m_btnSave, &QPushButton::clicked, this, &PointerTemplateBuilderDialog::onSaveAndApply);
    connect(m_btnCancel, &QPushButton::clicked, this, &QDialog::reject);
}

void PointerTemplateBuilderDialog::updateStepButtonsState() {
    if (!m_canvasLabel) return;
    BuilderStep curStep = m_canvasLabel->getBuilderStep();

    auto setBtnStyle = [&](QPushButton* btn, BuilderStep step, const QString& activeColor, const QString& label) {
        int cnt = m_canvasLabel->getPointCount(step);
        QString text = QString("%1 (%2)").arg(label).arg(cnt);
        btn->setText(text);

        if (curStep == step) {
            btn->setStyleSheet(QString("QPushButton { background-color: #1e3a5f; color: %1; border: 2px solid %1; font-weight: bold; padding: 5px 10px; border-radius: 3px; }").arg(activeColor));
        } else {
            btn->setStyleSheet("QPushButton { background-color: #232736; color: #e0e0e0; border: 1px solid #363b50; font-weight: bold; padding: 5px 10px; border-radius: 3px; }"
                               "QPushButton:hover { background-color: #2f3448; }");
        }
    };

    setBtnStyle(m_btnStepCenter, BuilderStep::SetCenter, "#ffd700", tr("🎯 1.Center"));
    setBtnStyle(m_btnStepTip, BuilderStep::SetTip, "#ff00ff", tr("▲ 2.Tip 轮廓"));
    setBtnStyle(m_btnStepBody, BuilderStep::SetBody, "#00e5ff", tr("█ 3.Body 轮廓"));
    setBtnStyle(m_btnStepTail, BuilderStep::SetTail, "#ff8c00", tr("▄ 4.Tail 轮廓"));

    if (curStep == BuilderStep::Completed) {
        m_btnCompleted->setStyleSheet("QPushButton { background-color: #004d40; color: #64ffda; border: 2px solid #64ffda; font-weight: bold; padding: 5px 10px; border-radius: 3px; }");
    } else {
        m_btnCompleted->setStyleSheet("QPushButton { background-color: #232736; color: #e0e0e0; border: 1px solid #363b50; font-weight: bold; padding: 5px 10px; border-radius: 3px; }");
    }
}

void PointerTemplateBuilderDialog::updateStepGuidanceLabel(BuilderStep step) {
    if (!m_guidanceLabel || !m_canvasLabel) return;
    QString text;
    int cnt = m_canvasLabel->getPointCount(step);

    switch (step) {
        case BuilderStep::SetCenter:
            text = tr("💡 [步骤 1/4] 请点击 🎯 表盘轴心 (Dial Center Pin)...");
            break;
        case BuilderStep::SetTip:
            text = QString(tr("💡 [步骤 2/4 - 自定义针尖轮廓] 已点 %1 个点。请顺时针/逆时针沿针尖真实边缘依次点击，构建自定义针尖多边形轮廓...")).arg(cnt);
            break;
        case BuilderStep::SetBody:
            text = QString(tr("💡 [步骤 3/4 - 自定义针身轮廓] 已点 %1 个点。请沿针身两侧边缘依次点击点，构建自定义针身轮廓...")).arg(cnt);
            break;
        case BuilderStep::SetTail:
            text = QString(tr("💡 [步骤 4/4 - 自定义针尾轮廓] 已点 %1 个点。请沿粗针尾/配重块（圆盘、泪滴、多边形等）外轮廓依次点击，构建任意自定义针尾多边形...")).arg(cnt);
            break;
        case BuilderStep::Completed:
            text = tr("✅ [标定完成] 可以在画布上鼠标拖拽任意顶点，或在右侧面板微调参数，2D 模板点集与法向将实时跟随重绘。");
            break;
    }
    m_guidanceLabel->setText(text);
}

void PointerTemplateBuilderDialog::onStepChangedFromCanvas(BuilderStep newStep) {
    updateStepButtonsState();
    updateStepGuidanceLabel(newStep);
}

void PointerTemplateBuilderDialog::onPointCountsUpdated() {
    updateStepButtonsState();
    updateStepGuidanceLabel(m_canvasLabel->getBuilderStep());
}

void PointerTemplateBuilderDialog::onModelUpdatedFromCanvas(const PointerShapeModel& model) {
    m_currentModel = model;
    updateSpinBoxControlsFromModel(model);
}

void PointerTemplateBuilderDialog::updateSpinBoxControlsFromModel(const PointerShapeModel& model) {
    m_updatingSpinBoxes = true;

    m_spinCenterX->setValue(model.center.x);
    m_spinCenterY->setValue(model.center.y);
    m_spinCenterExclusion->setValue(model.centerExclusionRadius);

    m_spinTipMinR->setValue(model.tipMinRadius);
    m_spinTipMaxR->setValue(model.tipMaxRadius);
    m_spinTipBaseWidth->setValue(model.tipBaseWidth);

    m_spinBodyMinR->setValue(model.bodyMinRadius);
    m_spinBodyMaxR->setValue(model.bodyMaxRadius);
    m_spinBodyWidth->setValue(model.bodyWidth);

    m_spinTailMinR->setValue(model.tailMinRadius);
    m_spinTailMaxR->setValue(model.tailMaxRadius);
    m_spinTailWidth->setValue(model.tailWidth);

    m_spinWTip->setValue(model.wTip);
    m_spinWBody->setValue(model.wBody);
    m_spinWTail->setValue(model.wTail);

    m_spinRadialStep->setValue(model.radialStep);
    m_spinDtSigma->setValue(model.dtSigma);

    m_updatingSpinBoxes = false;
}

PointerShapeModel PointerTemplateBuilderDialog::getModelFromSpinBoxControls() const {
    PointerShapeModel m = m_currentModel;

    m.center = cv::Point2f(static_cast<float>(m_spinCenterX->value()), static_cast<float>(m_spinCenterY->value()));
    m.centerExclusionRadius = static_cast<float>(m_spinCenterExclusion->value());

    m.tipMinRadius = static_cast<float>(m_spinTipMinR->value());
    m.tipMaxRadius = static_cast<float>(m_spinTipMaxR->value());
    m.tipBaseWidth = static_cast<float>(m_spinTipBaseWidth->value());

    m.bodyMinRadius = static_cast<float>(m_spinBodyMinR->value());
    m.bodyMaxRadius = static_cast<float>(m_spinBodyMaxR->value());
    m.bodyWidth = static_cast<float>(m_spinBodyWidth->value());

    m.tailMinRadius = static_cast<float>(m_spinTailMinR->value());
    m.tailMaxRadius = static_cast<float>(m_spinTailMaxR->value());
    m.tailWidth = static_cast<float>(m_spinTailWidth->value());

    m.wTip = static_cast<float>(m_spinWTip->value());
    m.wBody = static_cast<float>(m_spinWBody->value());
    m.wTail = static_cast<float>(m_spinWTail->value());

    m.radialStep = static_cast<float>(m_spinRadialStep->value());
    m.dtSigma = static_cast<float>(m_spinDtSigma->value());

    return m;
}

void PointerTemplateBuilderDialog::onSpinBoxValueChanged() {
    if (m_updatingSpinBoxes) return;
    m_currentModel = getModelFromSpinBoxControls();
    if (m_canvasLabel) {
        m_canvasLabel->setModel(m_currentModel);
    }
}

void PointerTemplateBuilderDialog::onClearCurrentStep() {
    if (m_canvasLabel) {
        m_canvasLabel->clearCurrentStepPoints();
    }
}

void PointerTemplateBuilderDialog::onUndoLastPoint() {
    if (m_canvasLabel) {
        m_canvasLabel->undoLastPoint();
    }
}

void PointerTemplateBuilderDialog::onResetCalibration() {
    if (m_canvasLabel) {
        m_canvasLabel->clearAllPoints();
    }
}

void PointerTemplateBuilderDialog::onLoadNewImage() {
    QString fileName = QFileDialog::getOpenFileName(this, tr("选择仪表样品照片"), "test_images", tr("Image Files (*.png *.jpg *.bmp *.jpeg)"));
    if (!fileName.isEmpty()) {
        cv::Mat img = cv::imread(fileName.toStdString());
        if (img.empty()) {
            QMessageBox::warning(this, tr("载入错误"), tr("无法读取选中的图像文件！"));
            return;
        }
        m_frame = img;
        if (m_canvasLabel) {
            m_canvasLabel->setImageAndModel(m_frame, m_currentModel);
        }
    }
}

void PointerTemplateBuilderDialog::onSaveAndApply() {
    m_currentModel = getModelFromSpinBoxControls();

    // Update Profile parameters
    m_profile.center = m_currentModel.center;
    m_profile.pointerBaseRadius = m_currentModel.centerExclusionRadius;
    m_profile.radius = m_currentModel.tipMaxRadius;
    m_profile.customTipPoints = m_currentModel.customTipPoints;
    m_profile.customBodyPoints = m_currentModel.customBodyPoints;
    m_profile.customTailPoints = m_currentModel.customTailPoints;

    // Save profile to file
    std::string targetCam = !m_cameraId.isEmpty() ? m_cameraId.toStdString() : m_profile.gauge_id;
    if (m_profile.saveToFile(m_configPath.toStdString(), targetCam)) {
        QMessageBox::information(this, tr("保存成功"),
            QString(tr("📌 交互式自定义 2D 多边形指针模板已成功保存至:\n%1\n\n"
                       "► Center: (%.1f, %.1f) px\n"
                       "► Tip 自定义顶点数: %2 点\n"
                       "► Body 自定义轮廓数: %3 点\n"
                       "► Tail 自定义轮廓数: %4 点"))
                .arg(m_configPath)
                .arg(m_currentModel.center.x).arg(m_currentModel.center.y)
                .arg(m_currentModel.customTipPoints.size())
                .arg(m_currentModel.customBodyPoints.size())
                .arg(m_currentModel.customTailPoints.size()));
    } else {
        QMessageBox::warning(this, tr("保存警告"), tr("模板参数已在内存中生效，但保存到 JSON 配置文件失败。"));
    }
    accept();
}
