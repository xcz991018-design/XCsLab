#include "MySqlRepository.hpp"
#include "DeviceCalibration.hpp"
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrent>
#include "CardinalSelectorDialog.hpp"
#include "GaugeReaderEngine.hpp"
#include "DeviceConfigManager.hpp"
#include <QPainter>
#include <QMessageBox>
#include <QDebug>
#include <cmath>
#include "ScaleCalibration.hpp"
#include <algorithm>

InteractiveImageLabel::InteractiveImageLabel(QWidget *parent) : QLabel(parent) {
    setMouseTracking(true);
    setMinimumSize(540, 480);
    setStyleSheet("background-color: #1a1a22; border: 1px solid #333344; border-radius: 4px;");
}

void InteractiveImageLabel::setImageAndPoints(const cv::Mat& mat, const std::vector<cv::Point2f>& pts, const cv::Point2f& centerPt) {
    m_origImage = mat.clone();
    m_pts = pts;
    if (!m_rectifiedMode && m_pts.size() != 4 && !mat.empty()) {
        m_pts = GaugeReaderEngine::autoDetectCardinalPoints(mat);
    }

    m_centerPt = m_rectifiedMode ? centerPt : cv::Point2f(-1,-1);
    m_selectedIdx=-1;
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

    QFont labelFont = painter.font();
    labelFont.setBold(true); labelFont.setPixelSize(11);
    if (!m_rectifiedMode) {
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

    painter.setFont(labelFont);

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

    }
    if (!m_rectifiedMode) return;
    for (const auto& value : m_scalePoints) {
        const auto p=value.toObject(); const QPointF cp=mapToCanvas({float(p["x"].toDouble()),float(p["y"].toDouble())});
        painter.setPen(QPen(QColor(100,255,100),1.5)); painter.setBrush(QColor(100,255,100));
        painter.drawLine(mapToCanvas(m_centerPt),cp); painter.drawEllipse(cp,5,5);
        painter.drawText(cp+QPointF(7,-7),tr("%1% 刻度").arg(p["ratio"].toDouble()*100,0,'f',1));
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
    if (!isEnabled() || m_origImage.empty() || event->button()!=Qt::LeftButton) return;
    updateTransformParams();
    const QPointF pos=event->localPos();
    m_selectedIdx=-1;
    if (pos.x()<m_offsetX || pos.y()<m_offsetY || pos.x()>=m_offsetX+m_origImage.cols*m_scale || pos.y()>=m_offsetY+m_origImage.rows*m_scale) return;
    if (!m_rectifiedMode) {
        for (int i=0;i<int(m_pts.size());++i) {
            auto cp=mapToCanvas(m_pts[i]);
            if (std::hypot(pos.x()-cp.x(),pos.y()-cp.y())<20) { m_selectedIdx=i; return; }
        }
        return;
    }
    if (m_pickCenter) {
        m_centerPt=mapToImage(pos); m_pickCenter=false; unsetCursor();
        update(); emit centerClicked(); emit pointsChanged(); return;
    }
    if (m_pendingRatio>=0) {
        auto pt=mapToImage(pos);
        std::vector<QJsonObject> points;
        for (const auto& value : m_scalePoints) if (value.toObject()["ratio"].toDouble()!=m_pendingRatio) points.push_back(value.toObject());
        points.push_back(QJsonObject{{"x",pt.x},{"y",pt.y},{"ratio",m_pendingRatio}});
        std::sort(points.begin(),points.end(),[](const QJsonObject& a,const QJsonObject& b){return a["ratio"].toDouble()<b["ratio"].toDouble();});
        m_scalePoints={}; for (const auto& p:points) m_scalePoints.append(p);
        m_pendingRatio=-1; update(); emit pointsChanged(); return;
    }
    for (int i=0;i<m_scalePoints.size();++i) {
        auto p=m_scalePoints[i].toObject(); auto cp=mapToCanvas({float(p["x"].toDouble()),float(p["y"].toDouble())});
        if (std::hypot(pos.x()-cp.x(),pos.y()-cp.y())<12) { m_selectedIdx=5+i; return; }
    }
}

void InteractiveImageLabel::mouseMoveEvent(QMouseEvent *event) {
    if (!isEnabled() || !(event->buttons() & Qt::LeftButton)) return;
    if (m_selectedIdx>=5 && m_selectedIdx-5<m_scalePoints.size()) {
        auto p=m_scalePoints[m_selectedIdx-5].toObject(); auto pt=mapToImage(event->localPos());
        p["x"]=pt.x; p["y"]=pt.y; m_scalePoints[m_selectedIdx-5]=p; update(); emit pointsChanged();
    } else if (m_selectedIdx >= 0 && m_selectedIdx < 4) {
        m_pts[m_selectedIdx] = mapToImage(event->localPos());
        update();
        emit pointsChanged();
    }
}

void InteractiveImageLabel::mouseReleaseEvent(QMouseEvent *event) {
    Q_UNUSED(event);
    m_selectedIdx = -1;
}

// ---------------- CardinalSelectorDialog ----------------

CardinalSelectorDialog::CardinalSelectorDialog(const cv::Mat& frame, GaugeProfile& profile, const QString& cameraId, QWidget *parent, bool historicalSession)
    : QDialog(parent), m_frame(frame.clone()), m_profile(profile), m_cameraId(cameraId) {
    m_historicalSession = historicalSession;
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

    m_interactiveLabel->setImageAndPoints(m_frame, m_currentPts);
    m_interactiveLabel->setObjectName("perspectiveImage");
    m_interactiveLabel->setPointerBaseRadius(m_profile.pointerBaseRadius);
    if (m_baseRadiusSpinBox) {
        m_baseRadiusSpinBox->setValue(m_profile.pointerBaseRadius);
    }
    updatePreview();
}

void CardinalSelectorDialog::setupUi() {
    setWindowTitle(tr("表计标定（透视、中心点与刻度）"));
    if (m_historicalSession) setWindowTitle(tr("历史原图手动标定（仅用于本次分析）"));
    resize(1200, 780);
    setStyleSheet("QDialog { background-color: #121216; color: #e0e0e0; font-family: Segoe UI, sans-serif; }");

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    QLabel *tipLabel = new QLabel(tr("第一步：在左侧原图调整四个透视切点并确认。第二步：在右侧矫正图点击指针轴心、最小和最大刻度，再调节轴帽避让半径。"), this);
    tipLabel->setWordWrap(true);
    tipLabel->setStyleSheet("font-weight: bold; color: #00e5ff; padding: 6px; font-size: 13px;");
    mainLayout->addWidget(tipLabel);

    QHBoxLayout *contentLayout = new QHBoxLayout();

    // Left: Interactive Selector
    QVBoxLayout *leftLayout = new QVBoxLayout();
    leftLayout->addWidget(new QLabel(tr("<b>第一步：原图四切点标定</b>"), this));
    m_interactiveLabel = new InteractiveImageLabel(this);
    leftLayout->addWidget(m_interactiveLabel, 1);
    contentLayout->addLayout(leftLayout, 1);

    // Right: Live Rectified Preview
    QVBoxLayout *rightLayout = new QVBoxLayout();
    rightLayout->addWidget(new QLabel(tr("<b>第二步：矫正图轴心与刻度标定</b>"), this));
    m_previewLabel = new InteractiveImageLabel(this);
    m_previewLabel->setObjectName("rectifiedCalibrationImage");
    m_previewLabel->setRectifiedMode(true);
    m_previewLabel->setEnabled(false);
    m_previewLabel->setMinimumSize(400, 400);
    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewLabel->setStyleSheet("background-color: #1a1a22; border: 1px solid #333344; border-radius: 4px;");
    rightLayout->addWidget(m_previewLabel, 1);
    contentLayout->addLayout(rightLayout, 1);

    mainLayout->addLayout(contentLayout, 1);

    m_scaleControls = new QWidget(this);
    auto* scaleLayout = new QHBoxLayout(m_scaleControls);
    m_centerBtn = new QPushButton(tr("点击标定轴心"),this);
    m_centerBtn->setObjectName("pickCenterButton");
    auto* minBtn = new QPushButton(tr("标定最小刻度"),this);
    auto* maxBtn = new QPushButton(tr("标定最大刻度"),this);
    auto* ratioSpin = new QDoubleSpinBox(this);
    ratioSpin->setRange(0.1,99.9); ratioSpin->setValue(50); ratioSpin->setSuffix(tr("% 量程"));
    auto* addBtn = new QPushButton(tr("标定中间刻度（可选）"),this);
    auto* clearBtn = new QPushButton(tr("清除刻度点"),this);
    scaleLayout->addWidget(m_centerBtn);
    scaleLayout->addWidget(minBtn); scaleLayout->addWidget(maxBtn); scaleLayout->addWidget(ratioSpin);
    scaleLayout->addWidget(addBtn); scaleLayout->addWidget(clearBtn);
    mainLayout->addWidget(m_scaleControls);
    m_scaleStatus = new QLabel(this); m_scaleStatus->setWordWrap(true); mainLayout->addWidget(m_scaleStatus);
    auto arm = [this](double ratio) {
        if (!m_perspectiveConfirmed || !m_centerConfirmed) { m_scaleStatus->setText(tr("请先在矫正图点击指针轴心。")); return; }
        m_previewLabel->selectScaleRatio(ratio);
        m_scaleStatus->setText(tr("请在右侧矫正图点击 %1% 量程对应的真实刻度线（不要点击数字）；已有点可拖动。").arg(ratio*100));
    };
    connect(m_centerBtn,&QPushButton::clicked,this,[this]{m_previewLabel->selectCenter(); m_scaleStatus->setText(tr("请在右侧矫正图点击指针旋转轴心；重新选择轴心也使用点击。"));});
    connect(minBtn,&QPushButton::clicked,this,[arm]{arm(0);});
    connect(maxBtn,&QPushButton::clicked,this,[arm]{arm(1);});
    connect(addBtn,&QPushButton::clicked,this,[arm,ratioSpin]{arm(ratioSpin->value()/100);});
    connect(clearBtn,&QPushButton::clicked,this,[this]{m_previewLabel->setScalePoints({}); m_previewLabel->selectScaleRatio(-1); updatePreview();});
    // Bottom Action Bar
    QHBoxLayout *btnLayout = new QHBoxLayout();
    m_autoBtn = new QPushButton(tr("⚡ 自动检测切点"), this);
    m_resetBtn = new QPushButton(tr("🔄 重置切点"), this);
    m_saveBtn = new QPushButton(m_historicalSession ? tr("确认标定并分析历史原图") : tr("💾 确认并保存到数据库"), this);
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

    m_confirmPerspectiveBtn=new QPushButton(tr("确认四切点，继续标定"),this);
    m_confirmPerspectiveBtn->setObjectName("confirmPerspectiveButton");
    btnLayout->addWidget(m_confirmPerspectiveBtn);
    btnLayout->addWidget(m_autoBtn);
    btnLayout->addWidget(m_resetBtn);
    btnLayout->addSpacing(15);
    btnLayout->addWidget(radiusLabel);
    btnLayout->addWidget(m_baseRadiusSpinBox);
    btnLayout->addStretch();
    btnLayout->addWidget(m_saveBtn);
    btnLayout->addWidget(m_cancelBtn);

    mainLayout->addLayout(btnLayout);
    m_scaleControls->setEnabled(false);
    m_baseRadiusSpinBox->setEnabled(false);
    m_saveBtn->setEnabled(false);
    m_saveStatus = new QLabel(tr("设备: %1；底图已冻结，请先确认原图四切点，再在矫正图完成后续标定。").arg(m_cameraId), this);
    m_saveStatus->setObjectName("calibrationSaveStatus");
    if (m_historicalSession) m_saveStatus->setText(tr("请根据这张历史原图重新标定；仅用于本次分析，不更新相机档案。取消后不进行识别。"));
    m_saveStatus->setWordWrap(true);
    mainLayout->addWidget(m_saveStatus);

    // Signals & Slots
    connect(m_interactiveLabel, &InteractiveImageLabel::pointsChanged, this, &CardinalSelectorDialog::onPointsUpdated);
    connect(m_baseRadiusSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double val) {
        m_previewLabel->setPointerBaseRadius(val);
        updatePreview();
    });
    connect(m_confirmPerspectiveBtn,&QPushButton::clicked,this,&CardinalSelectorDialog::onConfirmPerspective);
    connect(m_previewLabel,&InteractiveImageLabel::pointsChanged,this,&CardinalSelectorDialog::updatePreview);
    connect(m_previewLabel,&InteractiveImageLabel::centerClicked,this,[this]{m_centerConfirmed=true;});
    connect(m_autoBtn, &QPushButton::clicked, this, &CardinalSelectorDialog::onAutoDetect);
    connect(m_resetBtn, &QPushButton::clicked, this, &CardinalSelectorDialog::onReset);
    connect(m_saveBtn, &QPushButton::clicked, this, &CardinalSelectorDialog::onSaveAndApply);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
}

void CardinalSelectorDialog::onPointsUpdated() {
    m_currentPts=m_interactiveLabel->getPoints();
    updatePreview();
}

void CardinalSelectorDialog::onConfirmPerspective() {
    if (m_saving) return;
    if (m_perspectiveConfirmed) {
        // Changing the warp invalidates every downstream annotation.
        m_perspectiveConfirmed=false; m_centerConfirmed=false; m_loadExistingPoints=false;
        m_previewLabel->setScalePoints({});
        m_previewLabel->selectScaleRatio(-1);
        m_previewLabel->setEnabled(false); m_interactiveLabel->setEnabled(true);
        m_scaleControls->setEnabled(false); m_baseRadiusSpinBox->setEnabled(false);
        m_saveBtn->setEnabled(false); m_autoBtn->setEnabled(true); m_resetBtn->setEnabled(true);
        m_confirmPerspectiveBtn->setText(tr("确认四切点，继续标定"));
        updatePreview(); return;
    }
    m_currentPts=m_interactiveLabel->getPoints();
    if (m_currentPts.size()!=4 || !cv::isContourConvex(m_currentPts) || std::abs(cv::contourArea(m_currentPts))<=1) {
        m_saveStatus->setText(tr("请先调整四个切点，使其构成有效凸四边形。")); return;
    }
    for (auto pt:m_currentPts) if (!std::isfinite(pt.x) || !std::isfinite(pt.y) || pt.x<0 || pt.y<0 || pt.x>=m_frame.cols || pt.y>=m_frame.rows) return;
    updatePreview();
    if (m_inverseHomography.empty()) return;
    m_perspectiveConfirmed=true; m_centerConfirmed=false;
    m_interactiveLabel->setEnabled(false); m_previewLabel->setEnabled(true);
    m_scaleControls->setEnabled(true); m_baseRadiusSpinBox->setEnabled(true);
    m_autoBtn->setEnabled(false); m_resetBtn->setEnabled(false);
    m_confirmPerspectiveBtn->setText(tr("返回修改四切点"));
    m_previewLabel->selectCenter();
    m_scaleStatus->setText(tr("四切点已确认，请在右侧矫正图点击指针旋转轴心。"));
    m_saveStatus->setText(tr("第二步：在矫正图完成轴心和刻度标定后保存。"));
}

void CardinalSelectorDialog::updatePreview() {
    if (!m_perspectiveConfirmed) {
        m_inverseHomography.release();
        if (m_frame.empty() || m_currentPts.size()!=4 || !cv::isContourConvex(m_currentPts) || std::abs(cv::contourArea(m_currentPts))<=1) {
            m_previewLabel->setImageAndPoints(cv::Mat(),{});
            m_scaleStatus->setText(tr("四切点无效，请在原图调整后确认。")); return;
        }
        GaugeReaderEngine engine;
        auto rectified=engine.rectifyPerspective(m_frame,m_currentPts,m_inverseHomography,m_profile.rectified_size,m_profile.rectified_margin);
        cv::Point2f center(-1,-1);
        QJsonArray scales;
        if (m_loadExistingPoints && m_profile.center.x>=0 && m_profile.center.y>=0) {
            std::vector<cv::Point2f> c{m_profile.center};
            cv::perspectiveTransform(c,c,m_inverseHomography.inv()); center=c[0];
            scales=ScaleCalibration::transformed(m_profile.scalePoints,m_inverseHomography.inv());
        }
        m_previewLabel->setImageAndPoints(rectified,{},center);
        m_previewLabel->setScalePoints(scales);
        m_previewLabel->setPointerBaseRadius(m_baseRadiusSpinBox->value());
        m_scaleStatus->setText(tr("请先确认左侧四切点，再在右侧矫正图标定轴心和刻度。"));
        return;
    }
    auto center=m_previewLabel->getCenterPoint();
    auto scales=m_previewLabel->getScalePoints();
    ScaleCalibration::Mapping mapping;
    const bool valid=m_centerConfirmed && ScaleCalibration::valid(scales,m_profile.rectified_size,m_profile.rectified_size,center) &&
        ScaleCalibration::build(scales,center,{},m_profile.clockwise,mapping);
    m_saveBtn->setEnabled(valid && !m_saving);
    m_scaleStatus->setText(valid ? tr("已标定 %1 个刻度点；量程 %2～%3 %4，方向：%5。请检查矫正图射线后保存。")
        .arg(scales.size()).arg(m_profile.min_val).arg(m_profile.max_val).arg(QString::fromStdString(m_profile.unit))
        .arg(m_profile.clockwise?tr("顺时针"):tr("逆时针")) : tr("请在矫正图点击轴心，再标定最小、最大刻度；中间点须按数值增长方向排列。"));
}

void CardinalSelectorDialog::onAutoDetect() {
    if (m_frame.empty() || m_perspectiveConfirmed || m_saving) return;
    m_currentPts = GaugeReaderEngine::autoDetectCardinalPoints(m_frame);
    m_interactiveLabel->setImageAndPoints(m_frame, m_currentPts);
    updatePreview();
}

void CardinalSelectorDialog::onReset() {
    m_loadExistingPoints=false;
    onAutoDetect();
}

void CardinalSelectorDialog::reject() {
    if (!m_saving) QDialog::reject();
}

void CardinalSelectorDialog::onSaveAndApply() {
    if (m_saving) return;
    if (!m_perspectiveConfirmed || !m_centerConfirmed || m_inverseHomography.empty()) {
        m_saveStatus->setText(tr("请先确认四切点，并在矫正图点击轴心后完成刻度标定。")); return;
    }
    const auto points=m_interactiveLabel->getPoints();
    const auto rectifiedCenter=m_previewLabel->getCenterPoint();
    const auto rectifiedScales=m_previewLabel->getScalePoints();
    const auto radius=m_previewLabel->getPointerBaseRadius();
    ScaleCalibration::Mapping mapping;
    if (!ScaleCalibration::valid(rectifiedScales,m_profile.rectified_size,m_profile.rectified_size,rectifiedCenter) ||
        !ScaleCalibration::build(rectifiedScales,rectifiedCenter,{},m_profile.clockwise,mapping)) {
        m_saveStatus->setText(tr("无法保存：请检查矫正图的轴心、刻度点及增长方向。")); return;
    }
    // Persist inverse-mapped source points to retain the existing device and
    // historical calibration format. Runtime reapplies this same homography.
    std::vector<cv::Point2f> sourceCenter{rectifiedCenter};
    cv::perspectiveTransform(sourceCenter,sourceCenter,m_inverseHomography);
    const auto center=sourceCenter[0];
    const auto scales=ScaleCalibration::transformed(rectifiedScales,m_inverseHomography);
    QJsonObject calibration{{"perspective_correction_enabled", true},
        {"calibrated_width", m_frame.cols}, {"calibrated_height", m_frame.rows},
        {"center_x", center.x}, {"center_y", center.y}, {"pointer_base_radius", radius}};
    for (int i = 0; i < int(points.size()); ++i) {
        calibration[QString("pt_%1_x").arg(i)] = points[i].x;
        calibration[QString("pt_%1_y").arg(i)] = points[i].y;
    }
    calibration["scale_points"] = scales;
    if (!validDeviceCalibration(calibration) || m_cameraId.isEmpty()) {
        m_saveStatus->setText(tr("无法保存：请将四个切点按 12、3、6、9 点钟顺序放在图像内，并设置有效中心点。"));
        return;
    }
    if (m_historicalSession) {
        m_profile.scalePoints = scales;
        m_profile.perspective_pts = points;
        m_profile.perspective_correction_enabled = true;
        m_profile.center = center; m_profile.pointerBaseRadius = radius;
        m_profile.auto_detect_roi = false;
        m_profile.calibrated_width = m_frame.cols; m_profile.calibrated_height = m_frame.rows;
        accept();
        return;
    }
    m_saving = true;
    m_scaleControls->setEnabled(false);
    m_saveStatus->setText(tr("正在保存设备标定到数据库…"));
    m_previewLabel->setEnabled(false);
    m_confirmPerspectiveBtn->setEnabled(false);
    m_interactiveLabel->setEnabled(false);
    m_baseRadiusSpinBox->setEnabled(false);
    m_autoBtn->setEnabled(false); m_resetBtn->setEnabled(false);
    m_saveBtn->setEnabled(false); m_cancelBtn->setEnabled(false);
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, points, center, radius, scales]() {
        const auto error = watcher->result(); watcher->deleteLater();
        m_saving = false;
        if (!error.isEmpty()) {
            m_saveStatus->setText(tr("数据库保存失败：%1；标定尚未保存，可重试。").arg(error));
            m_scaleControls->setEnabled(true);
            m_previewLabel->setEnabled(true); m_baseRadiusSpinBox->setEnabled(true);
            m_confirmPerspectiveBtn->setEnabled(true);
            m_saveBtn->setEnabled(true); m_cancelBtn->setEnabled(true);
            return;
        }
        DeviceConfigManager::instance().updateDeviceCalibration(m_cameraId, true, m_frame.cols, m_frame.rows,
            points[0].x, points[0].y, points[1].x, points[1].y,
            points[2].x, points[2].y, points[3].x, points[3].y, center.x, center.y, radius, false, scales);
        m_profile.scalePoints=scales;
        m_profile.perspective_pts = points;
        m_profile.perspective_correction_enabled = true;
        m_profile.center = center; m_profile.pointerBaseRadius = radius;
        m_profile.auto_detect_roi = false;
        m_profile.calibrated_width = m_frame.cols; m_profile.calibrated_height = m_frame.rows;
        accept();
    });
    const auto cameraId = m_cameraId;
    watcher->setFuture(QtConcurrent::run([cameraId, calibration]() -> QString {
        try {
            DeviceConfigManager::instance().waitForDatabaseWrites();
            const auto syncError = MySqlRepository::retryPending(QString());
            if (!syncError.isEmpty()) return syncError;
            MySqlRepository().saveCalibration(cameraId, calibration);
            return {};
        } catch (const std::exception& error) { return QString::fromUtf8(error.what()); }
    }));
}
