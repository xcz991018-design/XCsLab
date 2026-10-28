#include "MySqlRepository.hpp"
#include "DeviceCalibration.hpp"
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrent>
#include "CardinalSelectorDialog.hpp"
#include "GaugeReaderEngine.hpp"
#include "CameraUndistortion.hpp"
#include "DeviceConfigManager.hpp"
#include <QPainter>
#include <QMessageBox>
#include <QDebug>
#include <QJsonDocument>
#include <QSignalBlocker>
#include <QFileDialog>
#include <QFile>
#include <QDateTime>
#include <QTimer>
#include <cmath>
#include "ScaleCalibration.hpp"
#include "AutoDialCalibration.hpp"
#include "PaddleScaleOcr.hpp"
#include "OcrDialRange.hpp"
#include <algorithm>

namespace {
std::vector<cv::Point2f> editableCardinalPlaceholders(const cv::Mat& frame) {
    if(frame.empty())return {};
    const float x=frame.cols*.5f,y=frame.rows*.5f,r=std::min(frame.cols,frame.rows)*.35f;
    return {{x,y-r},{x+r,y},{x,y+r},{x-r,y}};
}
}

InteractiveImageLabel::InteractiveImageLabel(QWidget *parent) : QLabel(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumSize(540, 480);
    setStyleSheet("background-color: #1a1a22; border: 1px solid #333344; border-radius: 4px;");
}

void InteractiveImageLabel::setImageAndPoints(const cv::Mat& mat, const std::vector<cv::Point2f>& pts, const cv::Point2f& centerPt) {
    m_origImage = mat.clone();
    m_ocrNumbers={};
    m_pts = pts;
    if (!m_rectifiedMode && m_pts.size() != 4 && !mat.empty()) {
        m_pts = editableCardinalPlaceholders(mat);
    }

    m_centerPt = m_rectifiedMode ? centerPt : cv::Point2f(-1,-1);
    m_selectedIdx=-1;
    clearScaleSelection();
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
    if(m_showLiquidMask && m_liquidSurface.found) {
        painter.save();
        painter.setClipRect(QRectF(mapToCanvas({0,0}),mapToCanvas({float(m_origImage.cols),float(m_origImage.rows)})));
        QPolygonF excluded; excluded<<mapToCanvas({0,0})<<mapToCanvas({float(m_origImage.cols),0});
        QPolygonF boundary;
        for(int x=m_origImage.cols;x>=0;x-=4) {
            const float y=float(m_liquidSurface.yAt(x)+m_liquidSurface.margin);
            excluded<<mapToCanvas({float(x),y});boundary<<mapToCanvas({float(x),y});
        }
        excluded<<mapToCanvas({0,float(m_liquidSurface.yAt(0)+m_liquidSurface.margin)});
        painter.setPen(Qt::NoPen);painter.setBrush(QColor(255,150,50,60));painter.drawPolygon(excluded);
        painter.setPen(QPen(QColor(255,185,70),2,Qt::DashLine));painter.drawPolyline(boundary);
        painter.setPen(QColor(255,220,150));painter.drawText(mapToCanvas({15,28}),tr("液面及以上：影响区域"));
        painter.restore();
    }
    painter.setPen(QPen(QColor(0,220,255),2));painter.setBrush(Qt::NoBrush);
    for(auto point:m_liquidClicks)painter.drawEllipse(mapToCanvas(point),5,5);
    for (int index=0;index<m_scalePoints.size();++index) {
        const auto value=m_scalePoints[index];
        const auto p=value.toObject(); const QPointF cp=mapToCanvas({float(p["x"].toDouble()),float(p["y"].toDouble())});
        const bool selected=index==m_selectedScale;
        const QColor color=selected?QColor("#ffb74d"):QColor(100,255,100);
        painter.setPen(QPen(color,selected?3:1.5)); painter.setBrush(color);
        const double ratio=p["ratio"].toDouble();
        const bool labelled=m_scalePoints.size()<=20 || std::abs(ratio*10-std::round(ratio*10))<1e-7;
        if(labelled||selected) painter.drawLine(mapToCanvas(ScaleCalibration::origin(p,m_centerPt)),cp);
        painter.drawEllipse(cp,selected?5:3,selected?5:3);
        if(labelled||selected) painter.drawText(cp+QPointF(7,-7),(m_hasScaleRange?
            tr("%1 刻度%2").arg(m_rangeMinimum+ratio*(m_rangeMaximum-m_rangeMinimum),0,'g',6):
            tr("%1% 刻度%2").arg(ratio*100,0,'f',1)).arg(selected?tr("（已选中，可删除）"):QString()));
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
    // OCR is an inspection overlay, independent of calibration geometry and hit testing.
    painter.save();painter.setClipRect(QRect(m_offsetX,m_offsetY,dispW,dispH));
    QFont ocrFont=painter.font();ocrFont.setPixelSize(14);ocrFont.setBold(true);painter.setFont(ocrFont);
    for(const auto& entry:m_ocrNumbers) {
        const auto number=entry.toObject();QPolygonF polygon;
        for(const auto& vertex:number["polygon"].toArray()) {
            const auto point=vertex.toArray();polygon<<mapToCanvas({float(point[0].toDouble()),float(point[1].toDouble())});
        }
        if(polygon.size()!=4)continue;
        painter.setPen(QPen(QColor("#00e5ff"),2));painter.setBrush(Qt::NoBrush);painter.drawPolygon(polygon);
        const auto text=tr("%1  %2%").arg(number["text"].toString()).arg(number["score"].toDouble()*100,0,'f',0);
        const auto bounds=polygon.boundingRect();
        const auto metrics=painter.fontMetrics();
        const double width=metrics.horizontalAdvance(text)+10,height=metrics.height()+6;
        const double x=std::max(double(m_offsetX),std::min(bounds.left(),m_offsetX+dispW-width));
        const double y=std::max(double(m_offsetY),bounds.top()-height-1);
        const QRectF caption(x,y,width,height);
        painter.fillRect(caption,QColor(0,25,35,225));painter.drawText(QPointF(x+5,y+3+metrics.ascent()),text);
    }
    painter.restore();
}

void InteractiveImageLabel::mousePressEvent(QMouseEvent *event) {
    if (!isEnabled() || m_origImage.empty() || event->button()!=Qt::LeftButton) return;
    setFocus(Qt::MouseFocusReason);
    clearScaleSelection();
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
    if(m_pickLiquidSurface) {
        const auto point=mapToImage(pos);
        if(!m_liquidClicks.empty() && std::abs(point.x-m_liquidClicks.front().x)<m_origImage.cols*.2) return;
        m_liquidClicks.push_back(point);update();
        if(m_liquidClicks.size()==2) {
            const auto a=m_liquidClicks[0],b=m_liquidClicks[1];
            LiquidSurface surface;surface.found=true;surface.centerX=m_origImage.cols*.5;surface.radius=m_origImage.cols*.5;
            surface.slope=(b.y-a.y)/(b.x-a.x)*surface.radius;
            surface.y0=a.y+(b.y-a.y)/(b.x-a.x)*(surface.centerX-a.x);
            surface.margin=std::max(6.0,surface.radius*.025);
            m_liquidSurface=surface;m_liquidClicks.clear();m_pickLiquidSurface=false;unsetCursor();
            emit liquidSurfaceChanged();
        }
        return;
    }
    if (m_pickCenter) {
        m_centerPt=mapToImage(pos); m_pickCenter=false; unsetCursor();
        emit pickModeChanged();
        update(); emit centerClicked(); emit pointsChanged(); return;
    }
    if (m_pendingRatio>=0) {
        auto pt=mapToImage(pos);
        std::vector<QJsonObject> points;
        for (const auto& value : m_scalePoints) if (value.toObject()["ratio"].toDouble()!=m_pendingRatio) points.push_back(value.toObject());
        points.push_back(QJsonObject{{"x",pt.x},{"y",pt.y},{"ratio",m_pendingRatio}});
        std::sort(points.begin(),points.end(),[](const QJsonObject& a,const QJsonObject& b){return a["ratio"].toDouble()<b["ratio"].toDouble();});
        m_scalePoints={}; for (const auto& p:points) m_scalePoints.append(p);
        m_pendingRatio=-1; unsetCursor(); emit pickModeChanged(); update(); emit pointsChanged(); return;
    }
    for (int i=0;i<m_scalePoints.size();++i) {
        auto p=m_scalePoints[i].toObject(); auto cp=mapToCanvas({float(p["x"].toDouble()),float(p["y"].toDouble())});
        if (std::hypot(pos.x()-cp.x(),pos.y()-cp.y())<12) {
            m_selectedIdx=5+i;
            if(p["ratio"].toDouble()>0&&p["ratio"].toDouble()<1)m_selectedScale=i;
            emit scaleSelectionChanged();update();return;
        }
    }
    // Select the nearest intermediate ray, avoiding the crowded area at the hub.
    double nearest=7;
    if(m_centerPt.x>=0&&m_centerPt.y>=0)for(int i=0;i<m_scalePoints.size();++i) {
        const auto point=m_scalePoints[i].toObject();
        const QPointF center=mapToCanvas(ScaleCalibration::origin(point,m_centerPt));
        const double ratio=point["ratio"].toDouble();
        if(ratio<=0||ratio>=1)continue;
        const QPointF end=mapToCanvas({float(point["x"].toDouble()),float(point["y"].toDouble())});
        const QPointF ray=end-center,delta=pos-center;
        const double length=QPointF::dotProduct(ray,ray);
        if(length<=0)continue;
        const double t=QPointF::dotProduct(delta,ray)/length;
        if(t<.18||t>1)continue;
        const auto offset=pos-(center+ray*t);
        const double distance=std::hypot(offset.x(),offset.y());
        if(distance<nearest){nearest=distance;m_selectedScale=i;}
    }
    emit scaleSelectionChanged();update();
}

void InteractiveImageLabel::deleteSelectedIntermediateScale() {
    if(!isEnabled()||!hasSelectedIntermediateScale())return;
    m_scalePoints.removeAt(m_selectedScale);m_selectedIdx=-1;
    clearScaleSelection();emit pointsChanged();
}

void InteractiveImageLabel::keyPressEvent(QKeyEvent* event) {
    if(event->key()==Qt::Key_Delete&&hasSelectedIntermediateScale()) {
        deleteSelectedIntermediateScale();event->accept();return;
    }
    QLabel::keyPressEvent(event);
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
    : QDialog(parent), m_rawFrame(frame.clone()), m_frame(CameraUndistortion::image(frame)), m_profile(profile), m_cameraId(cameraId) {
    m_historicalSession = historicalSession;
    m_automaticReviewMode=!historicalSession;
    m_sourceCenter=CameraUndistortion::point(profile.center, m_frame.size());
    m_sourceScalePoints=CameraUndistortion::scales(profile.scalePoints, m_frame.size());
    const auto correctedPoints=CameraUndistortion::points(profile.perspective_pts, m_frame.size());
    setupUi();
    m_rangeConfirmed=m_historicalSession||!m_profile.parent_model_path.empty();
    if(!m_cameraId.isEmpty()) {
        const auto device=DeviceConfigManager::instance().getDevice(m_cameraId);
        m_rangeConfirmed=m_rangeConfirmed||OcrDialRange::valid(device.meterRange)||!device.defaultProfileId.isEmpty();
    }

    bool ptsValid = (correctedPoints.size() == 4);
    if (ptsValid) {
        for (const auto& p : correctedPoints) {
            if (p.x <= 0 || p.x >= m_frame.cols || p.y <= 0 || p.y >= m_frame.rows) {
                ptsValid = false;
                break;
            }
        }
    }

    if (ptsValid) {
        m_currentPts = correctedPoints;
    } else {
        m_currentPts=editableCardinalPlaceholders(m_frame);
    }

    m_interactiveLabel->setImageAndPoints(m_frame, m_currentPts);
    m_interactiveLabel->setObjectName("perspectiveImage");
    m_interactiveLabel->setPointerBaseRadius(m_profile.pointerBaseRadius);
    if (m_baseRadiusSpinBox) {
        m_baseRadiusSpinBox->setValue(m_profile.pointerBaseRadius);
    }
    updatePreview();
    if (ptsValid && cv::isContourConvex(m_currentPts) &&
        ScaleCalibration::valid(m_sourceScalePoints,m_frame.cols,m_frame.rows,m_sourceCenter))
        onConfirmPerspective(); // Existing completed calibration is editable immediately.
    if(!m_historicalSession) {
        setManualEditing(false);
        m_saveBtn->setEnabled(false);m_manualEditBtn->setEnabled(false);
        m_saveStatus->setText(tr("正在自动执行视场矫正、液面检测及表盘中心检测…"));
        QTimer::singleShot(0,this,&CardinalSelectorDialog::onAutoDetect);
    }
}

void CardinalSelectorDialog::setupUi() {
    setWindowTitle(tr("表计标定（透视、中心点与刻度）"));
    if (m_historicalSession) setWindowTitle(tr("历史原图标定（仅用于本次分析）"));
    resize(1200, 780);
    setStyleSheet("QDialog { background-color: #121216; color: #e0e0e0; font-family: Segoe UI, sans-serif; }"
        "QLabel, QCheckBox { color: #e0e0e0; }"
        "QPushButton { background-color: #272a3a; color: #ffffff; border: 1px solid #3a3f58; border-radius: 4px; padding: 5px 10px; }"
        "QPushButton:hover { background-color: #35394e; border-color: #00e5ff; }"
        "QPushButton:disabled { background-color: #15161f; color: #9299ab; }"
        "QSpinBox, QDoubleSpinBox { background-color: #1f2230; color: #ffffff; border: 1px solid #4b526b; padding: 4px; }");

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    m_autoProgressLabel = new QLabel(tr("正在自动定位表盘、校正视场并检测液面，请稍候…可点击“取消”关闭。"), this);
    m_autoProgressLabel->setObjectName("calibrationLoadingStatus");
    m_autoProgressLabel->setWordWrap(true);
    m_autoProgressLabel->setStyleSheet("color: #ffffff; background-color: #203547; padding: 12px; font-size: 15px; font-weight: bold;");
    mainLayout->addWidget(m_autoProgressLabel);
    m_autoProgress = new QProgressBar(this);
    m_autoProgress->setObjectName("calibrationLoadingProgress");
    m_autoProgress->setRange(0, 0); // Detection has no measurable percentage.
    m_autoProgress->setTextVisible(false);
    m_autoProgress->setStyleSheet("QProgressBar { background-color: #252936; border: 1px solid #4b526b; border-radius: 3px; min-height: 8px; }"
        "QProgressBar::chunk { background-color: #4dd0e1; }");
    mainLayout->addWidget(m_autoProgress);
    m_autoProgressLabel->setVisible(!m_historicalSession);
    m_autoProgress->setVisible(!m_historicalSession);

    QHBoxLayout *contentLayout = new QHBoxLayout();

    // Left: Interactive Selector
    QVBoxLayout *leftLayout = new QVBoxLayout();
    auto* originalHeader=new QHBoxLayout();
    originalHeader->addWidget(new QLabel(tr("<b>表盘多特征定位与透视矫正（支持手动修正）</b>"), this));
    originalHeader->addStretch();
    auto* exportOriginal=new QPushButton(tr("保存标定原图"),this);
    exportOriginal->setObjectName("exportCalibrationOriginalButton");
    exportOriginal->setToolTip(tr("保存本窗口实际使用的冻结原图，不含缩放、矫正或标记。"));
    originalHeader->addWidget(exportOriginal);leftLayout->addLayout(originalHeader);
    connect(exportOriginal,&QPushButton::clicked,this,[this]{
        const auto path=QFileDialog::getSaveFileName(this,tr("保存本次标定实际原图"),
            QStringLiteral("calibration_%1.png").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),tr("PNG 图像 (*.png)"));
        if(path.isEmpty())return;
        std::vector<uchar> encoded;
        try {
            if(!cv::imencode(".png",m_rawFrame,encoded)) {m_saveStatus->setText(tr("标定原图编码失败。"));return;}
        } catch(const cv::Exception&) {m_saveStatus->setText(tr("标定原图编码失败。"));return;}
        QFile output(path);
        if(!output.open(QIODevice::WriteOnly)||output.write(reinterpret_cast<const char*>(encoded.data()),qint64(encoded.size()))!=qint64(encoded.size())) {
            m_saveStatus->setText(tr("标定原图保存失败：%1").arg(output.errorString()));return;
        }
        const auto saved=tr("已保存本次标定实际原图（%1×%2）：%3").arg(m_frame.cols).arg(m_frame.rows).arg(path);
        m_saveStatus->setText((m_automaticAttemptFailed?tr("本次自动定位未通过，当前四点为保留的旧标定，并非本次自动结果。\n"):QString())+saved);
    });
    m_interactiveLabel = new InteractiveImageLabel(this);
    leftLayout->addWidget(m_interactiveLabel, 1);
    contentLayout->addLayout(leftLayout, 1);

    // Right: Live Rectified Preview
    QVBoxLayout *rightLayout = new QVBoxLayout();
    rightLayout->addWidget(new QLabel(tr("<b>矫正预览、轴心与刻度标定</b>"), this));
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

    auto* autoScaleLayout=new QHBoxLayout;
    m_autoScaleBtn=new QPushButton(tr("识别表盘数字（PaddleOCR）"),this);
    m_autoScaleBtn->setObjectName("autoCalibrateScaleButton");
    m_autoScaleBtn->setToolTip(tr("识别左下、右下数字设置量程，并在数字外侧寻找真实大刻度线，自动标定起止角度；结果可手动修正。"));
    autoScaleLayout->addWidget(m_autoScaleBtn);
    auto* scaleHint=new QLabel(tr("自动匹配各数字对应的长刻度；结果可手动修正"),this);
    scaleHint->setWordWrap(true);
    autoScaleLayout->addWidget(scaleHint);autoScaleLayout->addStretch();
    mainLayout->addLayout(autoScaleLayout);
    auto* rangeLayout=new QHBoxLayout;
    rangeLayout->addWidget(new QLabel(tr("OCR 量程（左下 → 右下）"),this));
    m_rangeMinimum=new QDoubleSpinBox(this);m_rangeMaximum=new QDoubleSpinBox(this);
    m_rangeMinimum->setObjectName("ocrRangeMinimum");m_rangeMaximum->setObjectName("ocrRangeMaximum");
    for(auto* spin:{m_rangeMinimum,m_rangeMaximum}) {spin->setDecimals(6);spin->setRange(-1e9,1e9);rangeLayout->addWidget(spin);}
    m_rangeMinimum->setValue(m_profile.min_val);m_rangeMaximum->setValue(m_profile.max_val);
    const auto updateRangeLabels=[this]{m_previewLabel->setScaleRange(m_rangeMinimum->value(),m_rangeMaximum->value());};
    for(auto* spin:{m_rangeMinimum,m_rangeMaximum})connect(spin,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[updateRangeLabels](double){updateRangeLabels();});
    updateRangeLabels();
    rangeLayout->addWidget(new QLabel(QString::fromStdString(m_profile.unit)+tr("；识别后可修正，保存标定时生效"),this));
    mainLayout->addLayout(rangeLayout);
    connect(m_autoScaleBtn,&QPushButton::clicked,this,&CardinalSelectorDialog::onAutoScaleCalibration);

    m_scaleStatus = new QLabel(this);
    m_scaleStatus->setWordWrap(true);
    m_scaleStatus->setStyleSheet("color: #ffd700;");
    mainLayout->addWidget(m_scaleStatus);

    m_scaleControls = new QWidget(this);
    auto* calibrationControlsLayout = new QVBoxLayout(m_scaleControls);
    calibrationControlsLayout->setContentsMargins(0,0,0,0);
    auto* centerLayout = new QHBoxLayout;
    auto* scaleLayout = new QHBoxLayout;
    calibrationControlsLayout->addLayout(centerLayout);
    calibrationControlsLayout->addLayout(scaleLayout);
    m_centerBtn = new QPushButton(tr("点击标定轴心"),this);
    m_centerBtn->setObjectName("pickCenterButton");
    auto* minBtn = new QPushButton(tr("标定最小刻度"),this);
    auto* maxBtn = new QPushButton(tr("标定最大刻度"),this);
    minBtn->setObjectName("pickMinimumScaleButton");
    maxBtn->setObjectName("pickMaximumScaleButton");
    auto* ratioSpin = new QDoubleSpinBox(this);
    ratioSpin->setRange(0.1,99.9); ratioSpin->setValue(50); ratioSpin->setSuffix(tr("% 量程"));
    auto* addBtn = new QPushButton(tr("标定中间刻度（可选）"),this);
    addBtn->setObjectName("pickIntermediateScaleButton");
    for(auto* button : {m_centerBtn,minBtn,maxBtn,addBtn}) {
        button->setCheckable(true);
        button->setStyleSheet("QPushButton:checked { background-color: #80deea; color: #102027; border: 2px solid #ffffff; font-weight: bold; }"
            "QPushButton:checked:hover { background-color: #b2ebf2; color: #102027; }");
    }
    connect(m_previewLabel,&InteractiveImageLabel::pickModeChanged,this,[this,minBtn,maxBtn,addBtn] {
        const double ratio=m_previewLabel->pendingScaleRatio();
        m_centerBtn->setChecked(m_previewLabel->pickingCenter());
        minBtn->setChecked(ratio==0);
        maxBtn->setChecked(ratio==1);
        addBtn->setChecked(ratio>0&&ratio<1);
        m_centerBtn->setText(m_centerBtn->isChecked()?tr("正在标定：轴心"):tr("点击标定轴心"));
        minBtn->setText(minBtn->isChecked()?tr("正在标定：最小刻度"):tr("标定最小刻度"));
        maxBtn->setText(maxBtn->isChecked()?tr("正在标定：最大刻度"):tr("标定最大刻度"));
        addBtn->setText(addBtn->isChecked()?tr("正在标定：%1% 刻度").arg(ratio*100):tr("标定中间刻度（可选）"));
    });
    auto* clearBtn = new QPushButton(tr("删除全部刻度点"),this);
    auto* deleteSelected=new QPushButton(tr("删除选中刻度点"),this);
    deleteSelected->setObjectName("deleteSelectedIntermediateScaleButton");
    deleteSelected->setEnabled(false);
    deleteSelected->setToolTip(tr("点击右图的中间刻度线或刻度点选中，再点击此按钮或按 Delete 删除。"));
    m_previewLabel->setToolTip(tr("点击中间刻度线或刻度点可选中，按 Delete 删除；最小、最大刻度不可单独删除。"));
    connect(m_previewLabel,&InteractiveImageLabel::scaleSelectionChanged,this,[this,deleteSelected] {
        deleteSelected->setEnabled(m_previewLabel->hasSelectedIntermediateScale());
    });
    connect(m_previewLabel,&InteractiveImageLabel::pickModeChanged,m_previewLabel,&InteractiveImageLabel::clearScaleSelection);
    connect(deleteSelected,&QPushButton::clicked,m_previewLabel,&InteractiveImageLabel::deleteSelectedIntermediateScale);
    clearBtn->setObjectName("clearScalePointsButton");
    centerLayout->addWidget(m_centerBtn);
    scaleLayout->addWidget(minBtn); scaleLayout->addWidget(maxBtn); scaleLayout->addWidget(ratioSpin);
    scaleLayout->addWidget(addBtn); scaleLayout->addWidget(deleteSelected); scaleLayout->addWidget(clearBtn);
    mainLayout->addWidget(m_scaleControls);
    auto arm = [this](double ratio) {
        if (!m_perspectiveConfirmed || !m_centerConfirmed) {
            m_previewLabel->selectCenter();
            m_scaleStatus->setText(tr("请先在矫正图点击指针轴心。")); return;
        }
        m_previewLabel->selectScaleRatio(ratio);
        m_scaleStatus->setText(tr("请在右侧矫正图点击 %1% 量程对应的真实刻度线（不要点击数字）；已有点可拖动。").arg(ratio*100));
    };
    connect(m_centerBtn,&QPushButton::clicked,this,[this]{m_previewLabel->selectCenter(); m_scaleStatus->setText(tr("请在右侧矫正图点击指针旋转轴心；重新选择轴心也使用点击。"));});
    connect(minBtn,&QPushButton::clicked,this,[arm]{arm(0);});
    connect(maxBtn,&QPushButton::clicked,this,[arm]{arm(1);});
    connect(addBtn,&QPushButton::clicked,this,[arm,ratioSpin]{arm(ratioSpin->value()/100);});
    connect(clearBtn,&QPushButton::clicked,this,[this]{
        m_previewLabel->setScalePoints({}); m_previewLabel->selectScaleRatio(-1);
        updatePreview();
    });
    m_liquidControls=new QWidget(this);
    auto* liquidContainerLayout=new QVBoxLayout(m_liquidControls);
    liquidContainerLayout->setContentsMargins(0,0,0,0);
    auto* liquidLayout=new QHBoxLayout();liquidContainerLayout->addLayout(liquidLayout);
    m_liquidMaskEnabled=new QCheckBox(tr("识别并显示液面影响区域（充液表盘）"),this);
    m_liquidMaskEnabled->setObjectName("liquidSurfaceMaskEnabled");m_liquidMaskEnabled->setChecked(true);
    auto* detectLiquid=new QPushButton(tr("重新检测液面"),this);detectLiquid->setObjectName("detectLiquidSurfaceButton");
    auto* manualLiquid=new QPushButton(tr("手动调整液面"),this);manualLiquid->setObjectName("manualLiquidSurfaceButton");
    liquidLayout->addWidget(m_liquidMaskEnabled);liquidLayout->addWidget(detectLiquid);liquidLayout->addWidget(manualLiquid);liquidLayout->addStretch();
    connect(detectLiquid,&QPushButton::clicked,this,[this]{m_manualLiquidSurface=false;m_liquidMaskEnabled->setChecked(true);onDetectLiquidSurface();});
    connect(manualLiquid,&QPushButton::clicked,this,[this]{
        m_liquidMaskEnabled->setChecked(true);m_previewLabel->selectLiquidSurface();
        m_liquidNote=tr("请在右侧图中点击液面左、右两端；完成后继续手动标定刻度。");updatePreview();
    });
    connect(m_liquidMaskEnabled,&QCheckBox::toggled,this,[this](bool enabled){
        if(m_autoEndpointCalibration){m_autoEndpointCalibration=false;m_endpointReviewRequired=true;}
        m_previewLabel->showLiquidMask(enabled);
        m_liquidNote=enabled?tr("将显示液面及以上影响区域；无可靠液面时可手动指定。"):tr("已关闭液面屏蔽；请确认该表盘无需屏蔽液面。");updatePreview();
        if(m_endpointReviewRequired)m_saveStatus->setText(tr("液面屏蔽范围已改变，请重新识别表盘数字，或手动修正刻度后保存。"));
    });
    connect(m_previewLabel,&InteractiveImageLabel::liquidSurfaceChanged,this,[this]{
        if(m_autoEndpointCalibration){m_autoEndpointCalibration=false;m_endpointReviewRequired=true;}
        m_manualLiquidSurface=true;
        m_liquidNote=tr("已手动设置液面，上方含缓冲区为影响区域，请据此选择可靠刻度。");updatePreview();
        if(m_endpointReviewRequired)m_saveStatus->setText(tr("液面位置已改变，请重新识别表盘数字，或手动修正刻度后保存。"));
    });
    mainLayout->addWidget(m_liquidControls);
    m_liquidControls->setEnabled(false);
    m_geometryStatus=new QLabel(this); m_geometryStatus->setWordWrap(true);
    m_geometryStatus->setObjectName("calibrationGeometryStatus");
    mainLayout->addWidget(m_geometryStatus);
    m_geometryAcknowledged=new QCheckBox(tr("已复核上述几何提示，确认使用这些标定点"),this);
    m_geometryAcknowledged->setObjectName("acknowledgeCalibrationGeometry");
    m_geometryAcknowledged->setVisible(false);
    mainLayout->addWidget(m_geometryAcknowledged);
    connect(m_geometryAcknowledged,&QCheckBox::toggled,this,[this]{updatePreview();});
    // Bottom Action Bar
    QHBoxLayout *btnLayout = new QHBoxLayout();
    m_autoBtn = new QPushButton(tr("自动检测切点"), this);
    m_autoBtn->setObjectName("autoCalibrateDialButton");
    m_saveBtn = new QPushButton(m_historicalSession ? tr("确认标定并分析历史原图") : tr("💾 确认并保存到数据库"), this);
    m_cancelBtn = new QPushButton(tr("取消"), this);
    m_manualEditBtn=new QPushButton(tr("手动修改"),this);
    m_manualEditBtn->setObjectName("manualEditCalibrationButton");
    m_manualEditBtn->setVisible(!m_historicalSession);
    connect(m_manualEditBtn,&QPushButton::clicked,this,[this]{setManualEditing(m_automaticReviewMode);});

    m_saveBtn->setObjectName("saveCalibrationButton");
    m_saveBtn->setStyleSheet("QPushButton { background-color: #00897b; color: white; font-weight: bold; padding: 8px 16px; border-radius: 4px; }"
        "QPushButton:disabled { background-color: #243b38; color: #899f99; border: 1px solid #425b55; }");
    m_cancelBtn->setStyleSheet("background-color: #2a2a38; border: 1px solid #3a3a4c; padding: 8px 16px; border-radius: 4px;");

    QLabel *radiusLabel = new QLabel(tr("中心轴帽避让半径："), this);

    m_baseRadiusSpinBox = new QDoubleSpinBox(this);
    m_baseRadiusSpinBox->setRange(5.0, 250.0);
    m_baseRadiusSpinBox->setSingleStep(1.0);
    m_baseRadiusSpinBox->setDecimals(1);
    m_baseRadiusSpinBox->setSuffix(" px");
    m_baseRadiusSpinBox->setValue(m_profile.pointerBaseRadius > 0 ? m_profile.pointerBaseRadius : 50.0);
    centerLayout->insertWidget(0,m_autoBtn);
    centerLayout->addWidget(radiusLabel);
    centerLayout->addWidget(m_baseRadiusSpinBox);
    centerLayout->addStretch();

    m_confirmPerspectiveBtn=new QPushButton(tr("确认四切点，继续标定"),this);
    m_confirmPerspectiveBtn->setObjectName("confirmPerspectiveButton");
    btnLayout->addWidget(m_confirmPerspectiveBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(m_manualEditBtn);
    btnLayout->addWidget(m_saveBtn);
    btnLayout->addWidget(m_cancelBtn);

    mainLayout->addLayout(btnLayout);
    m_manualWidgets={m_scaleControls,m_liquidControls,m_geometryAcknowledged,
        m_confirmPerspectiveBtn,m_autoBtn,radiusLabel,m_baseRadiusSpinBox};
    m_scaleControls->setEnabled(false);
    m_baseRadiusSpinBox->setEnabled(false);
    m_saveBtn->setEnabled(false);
    m_saveStatus = new QLabel(tr("设备: %1；快照已冻结并完成镜头去畸变，请先确认左图四切点，再在透视矫正图完成后续标定。").arg(m_cameraId), this);
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
    connect(m_previewLabel,&InteractiveImageLabel::pointsChanged,this,[this]{
        // An explicit manual annotation is the user's correction of the result.
        m_autoEndpointCalibration=false;m_endpointReviewRequired=false;updatePreview();
    });
    connect(m_previewLabel,&InteractiveImageLabel::centerClicked,this,[this]{
        m_centerConfirmed=true;

        m_manualLiquidSurface=false;m_liquidNote.clear();m_previewLabel->setLiquidSurface({});
    });
    connect(m_autoBtn, &QPushButton::clicked, this, [this]{startAutoDetection(true);});
    connect(m_saveBtn, &QPushButton::clicked, this, &CardinalSelectorDialog::onSaveAndApply);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
}

void CardinalSelectorDialog::setManualEditing(bool enabled) {
    if(m_saving || m_autoDetectRunning)return;
    if(enabled)m_automaticAttemptFailed=false;
    m_automaticReviewMode=!enabled;
    if(!enabled)m_previewLabel->selectScaleRatio(-1);
    for(auto* widget:m_manualWidgets)widget->setVisible(enabled);
    m_manualEditBtn->setText(enabled?tr("查看标定预览"):tr("手动修改"));
    m_saveBtn->setText(enabled?tr("确认并保存标定结果"):tr("确认自动标定结果"));
    m_interactiveLabel->setMinimumSize(enabled?540:400,enabled?480:320);
    m_previewLabel->setMinimumSize(400,enabled?400:320);
    m_interactiveLabel->setEnabled(enabled&&!m_saving);
    m_previewLabel->setEnabled(enabled&&m_perspectiveConfirmed&&!m_saving);
    if(enabled && (m_saveStatus->text().startsWith(tr("自动校正与液面检测已完成")) ||
                   m_saveStatus->text().startsWith(tr("自动校正及中心检测完成"))))
        m_saveStatus->clear();
    updatePreview();
}

void CardinalSelectorDialog::onPointsUpdated() {
    if(m_saving || m_autoDetectRunning)return;
    m_graduationSourceCenter={-1,-1};
    if(m_autoEndpointCalibration) {m_endpointReviewRequired=true;m_autoEndpointCalibration=false;}
    // Preserve annotations in source pixels before replacing the homography.
    if(m_perspectiveConfirmed && !m_inverseHomography.empty()) {
        const auto annotations=sourceAnnotations();
        m_sourceCenter=annotations.first;m_sourceScalePoints=annotations.second;
    }
    const bool centerWasConfirmed=m_centerConfirmed;
    m_perspectiveConfirmed=false;
    m_autoDialReviewPending=false;
    m_manualLiquidSurface=false;m_liquidNote.clear();m_previewLabel->setLiquidSurface({});
    m_currentPts=m_interactiveLabel->getPoints();
    updatePreview();
    if(!m_inverseHomography.empty()) {
        onConfirmPerspective();
        const auto center=m_previewLabel->getCenterPoint();
        if(centerWasConfirmed && center.x>=0 && center.y>=0 &&
            center.x<m_profile.rectified_size && center.y<m_profile.rectified_size) {
            m_centerConfirmed=true;
            m_previewLabel->selectScaleRatio(-1);
        }
        updatePreview();
        m_saveStatus->setText(tr("切点已调整，右侧矫正预览已同步更新。请复核轴心、刻度及液面后保存。"));
        if(m_endpointReviewRequired)m_saveStatus->setText(tr("四切点已改变，旧自动端点不再作为有效结果。请重新识别表盘数字，或手动修正刻度后保存。"));
    } else {
        m_scaleControls->setEnabled(false);m_liquidControls->setEnabled(false);
        m_previewLabel->setEnabled(false);m_baseRadiusSpinBox->setEnabled(false);
        m_confirmPerspectiveBtn->setVisible(!m_automaticReviewMode);
        m_saveStatus->setText(tr("四切点无效，请继续拖动左图切点，使其构成有效凸四边形。"));
    }
}

void CardinalSelectorDialog::onConfirmPerspective() {
    if (m_saving) return;
    if (m_perspectiveConfirmed) {

        // Freeze current edits in source pixels before changing the homography.
        const auto annotations=sourceAnnotations();
        m_sourceCenter=annotations.first; m_sourceScalePoints=annotations.second;
        m_perspectiveConfirmed=false; m_centerConfirmed=false;
        m_previewLabel->selectScaleRatio(-1);
        m_previewLabel->setEnabled(false); m_interactiveLabel->setEnabled(true);
        m_scaleControls->setEnabled(false); m_baseRadiusSpinBox->setEnabled(false);
        m_liquidControls->setEnabled(false);

        m_manualLiquidSurface=false;m_liquidNote.clear();m_previewLabel->setLiquidSurface({});
        m_saveBtn->setEnabled(false); m_autoBtn->setEnabled(true);
        m_confirmPerspectiveBtn->setVisible(!m_automaticReviewMode);
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
    m_perspectiveConfirmed=true;
    ScaleCalibration::Mapping existingMapping;
    const auto existingCenter=m_previewLabel->getCenterPoint();
    const auto existingScales=m_previewLabel->getScalePoints();
    m_centerConfirmed=
        ScaleCalibration::valid(existingScales,m_profile.rectified_size,m_profile.rectified_size,existingCenter) &&
        ScaleCalibration::build(existingScales,existingCenter,{},m_profile.clockwise,existingMapping);
    m_interactiveLabel->setEnabled(!m_automaticReviewMode); m_previewLabel->setEnabled(!m_automaticReviewMode);
    m_scaleControls->setEnabled(true); m_baseRadiusSpinBox->setEnabled(true);
    m_liquidControls->setEnabled(true);
    m_autoBtn->setEnabled(true);
    m_confirmPerspectiveBtn->hide();
    if (!m_centerConfirmed) {
        m_previewLabel->selectCenter();
        m_scaleStatus->setText(tr("四切点已确认，请在右侧矫正图点击指针旋转轴心。"));
    } else {
        m_previewLabel->selectScaleRatio(-1);
        updatePreview();
    }
    m_saveStatus->setText(m_centerConfirmed
        ? tr("轴心和刻度点已映射到当前矫正图，请检查并按需调整后保存。")
        : tr("第二步：在矫正图完成轴心和刻度标定后保存。"));
}

void CardinalSelectorDialog::updatePreview() {
    m_autoScaleBtn->setEnabled(!m_saving&&!m_autoDetectRunning&&m_perspectiveConfirmed);
    m_confirmPerspectiveBtn->setVisible(!m_automaticReviewMode&&!m_perspectiveConfirmed);
    m_scaleStatus->setVisible(!m_automaticReviewMode);
    m_geometryStatus->setVisible(!m_automaticReviewMode);
    if (!m_perspectiveConfirmed) {
        m_inverseHomography.release();
        m_geometryStatus->clear(); m_geometryAcknowledged->setVisible(false);
        m_geometryReviewSignature.clear();
        if (m_frame.empty() || m_currentPts.size()!=4 || !cv::isContourConvex(m_currentPts) || std::abs(cv::contourArea(m_currentPts))<=1) {
            m_saveBtn->setEnabled(false);
            m_previewLabel->setImageAndPoints(cv::Mat(),{});
            m_scaleStatus->setText(tr("四切点无效，请在原图调整后确认。")); return;
        }
        GaugeReaderEngine engine;
        auto rectified=engine.rectifyPerspective(m_frame,m_currentPts,m_inverseHomography,m_profile.rectified_size,m_profile.rectified_margin);
        cv::Point2f center(-1,-1);
        QJsonArray scales;
        if (m_sourceCenter.x>=0 && m_sourceCenter.y>=0) {
            std::vector<cv::Point2f> c{m_sourceCenter};
            cv::perspectiveTransform(c,c,m_inverseHomography.inv()); center=c[0];
            scales=ScaleCalibration::transformed(m_sourceScalePoints,m_inverseHomography.inv());
        }
        m_previewLabel->setImageAndPoints(rectified,{},center);
        m_previewLabel->setScalePoints(scales);
        m_previewLabel->setPointerBaseRadius(m_baseRadiusSpinBox->value());
        m_scaleStatus->setText(tr("请调整并确认左侧四切点；已有轴心和刻度点会同步映射到右侧矫正图，请检查后按需调整。"));
        return;
    }
    auto center=m_previewLabel->getCenterPoint();
    auto scales=m_previewLabel->getScalePoints();
    ScaleCalibration::Mapping mapping;
    const bool valid=m_centerConfirmed && ScaleCalibration::valid(scales,m_profile.rectified_size,m_profile.rectified_size,center) &&
        ScaleCalibration::build(scales,center,{},m_profile.clockwise,mapping);
    QStringList issues;
    if(valid) {
        issues=ScaleCalibration::geometryIssues(scales,center,m_profile.clockwise,
            m_profile.rectified_size*.5-m_profile.rectified_margin);
        if(m_autoDialReviewPending)issues<<tr("表盘及轴心为自动定位结果，请复核边界、轴帽中心和刻度；液面局部变形仍需屏蔽。");
        QString text=issues.isEmpty()?tr("几何检查：未发现明显的刻度排列或局部跨度异常；请复核刻度数值及图中位置。")
            :tr("几何检查需复核：%1").arg(issues.join(tr("；")));
        if(!m_liquidNote.isEmpty()&&!m_liquidNote.startsWith(tr("已检测液面：")))
            text+=QStringLiteral(u"\n")+m_liquidNote;
        m_geometryStatus->setText(text);
        m_geometryStatus->setStyleSheet(issues.isEmpty()?"color: #9adbb0;":"color: #ffca75;");
    } else {
        m_geometryStatus->setText(m_liquidNote.startsWith(tr("已检测液面："))?QString():m_liquidNote);
        m_geometryStatus->setStyleSheet("color: #ffca75;");
    }
    m_geometryStatus->setVisible(!m_automaticReviewMode&&!m_geometryStatus->text().isEmpty());
    const QByteArray signature=QJsonDocument(QJsonObject{{"points",scales},{"center_x",center.x},{"center_y",center.y},
        {"clockwise",m_profile.clockwise},{"auto_dial",m_autoDialReviewPending},
        {"liquid_enabled",m_liquidMaskEnabled->isChecked()},{"liquid_y",m_previewLabel->liquidSurface().y0},
        {"liquid_slope",m_previewLabel->liquidSurface().slope},{"liquid_curve",m_previewLabel->liquidSurface().curve}}).toJson(QJsonDocument::Compact);
    if(signature!=m_geometryReviewSignature) {
        const QSignalBlocker blocker(m_geometryAcknowledged);
        m_geometryAcknowledged->setChecked(false); m_geometryReviewSignature=signature;
    }
    m_geometryAcknowledged->setVisible(!m_automaticReviewMode && valid && !issues.isEmpty());
    m_saveBtn->setEnabled(valid && !m_endpointReviewRequired && !m_saving && !m_autoDetectRunning && (!m_automaticReviewMode || !m_automaticAttemptFailed) &&
        (m_automaticReviewMode || issues.isEmpty() || m_geometryAcknowledged->isChecked()));
    m_scaleStatus->setText(valid ? tr("已标定 %1 个刻度点；量程 %2～%3 %4，方向：%5。请检查矫正图射线后保存。")
        .arg(scales.size()).arg(m_rangeMinimum->value()).arg(m_rangeMaximum->value()).arg(QString::fromStdString(m_profile.unit))
        .arg(m_profile.clockwise?tr("顺时针"):tr("逆时针")) : m_centerConfirmed
            ?tr("轴心已确认，可识别表盘数字，或手动标定刻度。")
            :tr("可先识别表盘数字；手动标定最小、最大及中间刻度前，请在矫正图确认轴心。"));
}

void CardinalSelectorDialog::onDetectLiquidSurface() {
    if(m_saving || !m_perspectiveConfirmed)return;
    if(!m_centerConfirmed){m_scaleStatus->setText(tr("请先在右侧图中确认指针轴心。"));return;}
    if(m_autoEndpointCalibration){m_autoEndpointCalibration=false;m_endpointReviewRequired=true;}

    GaugeReaderEngine engine;cv::Mat inverse;
    const auto rectified=engine.rectifyPerspective(m_frame,m_currentPts,inverse,m_profile.rectified_size,m_profile.rectified_margin);
    const auto surface=detectLiquidSurface(rectified,m_previewLabel->getCenterPoint(),m_profile.rectified_size*.5-m_profile.rectified_margin);
    m_previewLabel->setLiquidSurface(surface);m_previewLabel->showLiquidMask(m_liquidMaskEnabled->isChecked());

    m_liquidNote=surface.found?tr("已检测液面：橙色为液面影响区域（含边界缓冲），仅匹配下方数字与长刻度。"):
        surface.ambiguous?tr("存在多条可能的液面，请手动调整液面，或确认无液面后关闭显示。"):
        tr("未检测到可靠液面，请手动指定液面；无液体表盘可关闭液面显示。");
    updatePreview();
    if(m_endpointReviewRequired)m_saveStatus->setText(tr("液面已重新检测，请重新识别表盘数字，或手动修正刻度后保存。"));
}

void CardinalSelectorDialog::setAutoDetectionBusy(bool busy) {
    m_autoDetectRunning=busy;
    m_autoProgress->setVisible(busy);
    m_autoProgressLabel->setVisible(busy);
    m_autoBtn->setEnabled(!busy);
    m_autoScaleBtn->setEnabled(!busy&&m_perspectiveConfirmed);
    m_rangeMinimum->setEnabled(!busy);m_rangeMaximum->setEnabled(!busy);
    m_confirmPerspectiveBtn->setEnabled(!busy);
    m_manualEditBtn->setEnabled(!busy);
    m_scaleControls->setEnabled(!busy&&m_perspectiveConfirmed);
    m_liquidControls->setEnabled(!busy&&m_perspectiveConfirmed);
    m_baseRadiusSpinBox->setEnabled(!busy&&m_perspectiveConfirmed);
    m_geometryAcknowledged->setEnabled(!busy);
    m_interactiveLabel->setEnabled(!busy&&!m_automaticReviewMode);
    m_previewLabel->setEnabled(!busy&&!m_automaticReviewMode&&m_perspectiveConfirmed);
    m_saveBtn->setEnabled(false);
}

void CardinalSelectorDialog::onAutoDetect() {
    startAutoDetection(false);
}

void CardinalSelectorDialog::onAutoScaleCalibration() {
    if(m_saving||m_autoDetectRunning||!m_perspectiveConfirmed)return;
    cv::Mat rectified;
    try {
        GaugeReaderEngine engine;cv::Mat inverse;
        rectified=engine.rectifyPerspective(m_frame,m_currentPts,inverse,m_profile.rectified_size,m_profile.rectified_margin).clone();
    } catch(const cv::Exception& error) {m_saveStatus->setText(QString::fromUtf8(error.what()));return;}
    if(rectified.empty())return;
    auto cancelled=std::make_shared<std::atomic_bool>(false);
    // Closing the dialog interrupts the worker process; no GUI objects enter the worker.
    connect(this,&QDialog::finished,this,[cancelled]{cancelled->store(true);});
    connect(this,&QObject::destroyed,[cancelled]{cancelled->store(true);});
    m_previewLabel->selectScaleRatio(-1);
    m_previewLabel->setOcrNumbers({});
    setAutoDetectionBusy(true);
    m_autoProgressLabel->setText(tr("正在识别数字并匹配各数值的长刻度线…可点击取消关闭。"));
    m_saveStatus->setText(tr("正在识别数字，完成后在右图显示数字框和置信度。"));
    auto center=m_previewLabel->getCenterPoint();
    if(center.x<0||center.y<0)center=cv::Point2f(rectified.cols*.5f,rectified.rows*.5f);
    auto scaleCenter=center;
    if(m_graduationSourceCenter.x>=0&&!m_inverseHomography.empty()) {
        std::vector<cv::Point2f> origins{m_graduationSourceCenter};
        cv::perspectiveTransform(origins,origins,m_inverseHomography.inv());scaleCenter=origins[0];
    } else {
        const auto existing=m_previewLabel->getScalePoints();
        if(!existing.isEmpty())scaleCenter=ScaleCalibration::origin(existing.first().toObject(),center);
    }
    const auto surface=m_liquidMaskEnabled->isChecked()?m_previewLabel->liquidSurface():LiquidSurface{};
    struct Analysis {PaddleScaleOcr::Recognition ocr;OcrDialRange::Calibration calibration;};
    auto* watcher=new QFutureWatcher<Analysis>(this);
    connect(watcher,&QFutureWatcher<Analysis>::finished,this,[this,watcher,center,size=rectified.cols,liquidExcluded=surface.found] {
        const auto analysis=watcher->result();const auto& result=analysis.ocr;watcher->deleteLater();
        setAutoDetectionBusy(false);
        if(!result.valid()) {
            updatePreview();
            m_saveStatus->setText(tr("数字识别失败：%1 已保留原标定。").arg(result.error));return;
        }
        const auto numbers=PaddleScaleOcr::numericTokens(result.tokens);
        const auto& range=analysis.calibration.range;
        if(range.valid) {
            m_rangeMinimum->setValue(range.minimum);m_rangeMaximum->setValue(range.maximum);m_rangeConfirmed=true;
        }
        m_previewLabel->setOcrNumbers(numbers);
        QJsonArray merged;
        const bool calibrated=OcrDialRange::mergeCalibration(m_previewLabel->getScalePoints(),analysis.calibration,center,size,m_profile.clockwise,merged);
        if(calibrated) {
            m_previewLabel->setScalePoints(merged);m_autoEndpointCalibration=true;m_endpointReviewRequired=false;
        } else if(range.valid&&m_centerConfirmed) {
            m_endpointReviewRequired=true;m_autoEndpointCalibration=false;
        }
        updatePreview();
        m_saveStatus->setText(numbers.isEmpty()?tr("未识别到符合候选值的可靠数字，请检查候选列表和右图清晰度；已有标定点保留。"):
            tr("识别到 %1 处数字：右图青色框显示识别结果及置信度，已有刻度点保持不变。").arg(numbers.size()));
        m_saveStatus->setText(m_saveStatus->text()+(range.valid?
            tr(" 已设置量程 %1 ～ %2 %3，可修正后保存。").arg(range.minimum).arg(range.maximum).arg(QString::fromStdString(m_profile.unit)):
            tr(" 左下或右下端点缺失或有歧义，量程未修改，请检查识别结果。")));
        if(calibrated) {
            m_saveStatus->setText(tr("量程 %1 ～ %2 %3；%4 处刻度角度已自动标定（含起止和 %5 处中间数值，绿色射线）。可确认保存，也可手动修正。")
                .arg(range.minimum).arg(range.maximum).arg(QString::fromStdString(m_profile.unit)).arg(analysis.calibration.points.size()).arg(analysis.calibration.points.size()-2));
            if(liquidExcluded)m_saveStatus->setText(m_saveStatus->text()+tr(" 液面上方、边界缓冲区及跨界刻度已排除。"));
            if(analysis.calibration.skipped)m_saveStatus->setText(m_saveStatus->text()+tr(" 另有 %1 处数字未找到可靠长刻度，未添加标定点。").arg(analysis.calibration.skipped));
        }
        else if(range.valid)m_saveStatus->setText(m_saveStatus->text()+
            (analysis.calibration.valid()?tr(" 已有中间刻度与新匹配排列冲突，原刻度点保留，请手动修正。"):
                tr(" 刻度角度未修改：%1").arg(analysis.calibration.error)));
        else if(!range.valid&&!analysis.calibration.error.isEmpty())m_saveStatus->setText(m_saveStatus->text()+analysis.calibration.error);
    });
    const auto radius=m_profile.rectified_size*.5-m_profile.rectified_margin;
    watcher->setFuture(QtConcurrent::run([rectified,cancelled,scaleCenter,surface,radius,clockwise=m_profile.clockwise,confirmed=m_centerConfirmed] {
        Analysis analysis;analysis.ocr=PaddleScaleOcr::recognize(rectified,cancelled);
        if(!analysis.ocr.valid()||cancelled->load())return analysis;
        if(confirmed) {
            try {
                analysis.calibration=OcrDialRange::calibrate(rectified,analysis.ocr.tokens,scaleCenter,radius,clockwise,surface);
            }
            catch(const cv::Exception& error) {analysis.calibration.error=QString::fromUtf8(error.what());}
        }
        else analysis.calibration.error=QStringLiteral(u"请先确认轴心并识别两个端点数字。");
        return analysis;
    }));
}

void CardinalSelectorDialog::startAutoDetection(bool cornersOnly) {
    if(m_frame.empty()||m_saving||m_autoDetectRunning)return;
    m_automaticAttemptFailed=false;
    setAutoDetectionBusy(true);
    m_autoProgressLabel->setText(cornersOnly?tr("正在自动检测切点，请稍候…可点击“取消”关闭。"):
        tr("正在自动定位表盘、校正视场并检测液面，请稍候…可点击“取消”关闭。"));
    m_saveStatus->setText(cornersOnly?tr("正在自动检测切点，请稍候…"):
        tr("正在自动识别，请稍候；识别完成后将显示标定预览。"));
    struct Detection {
        AutoDialCalibration::Result dial;
        LiquidSurface surface;
    };
    auto* watcher=new QFutureWatcher<Detection>(this);
    connect(watcher,&QFutureWatcher<Detection>::finished,this,[this,watcher,cornersOnly] {
        const auto detection=watcher->result();
        watcher->deleteLater();
        setAutoDetectionBusy(false);
        const auto& result=detection.dial;
        if(!result.valid()) {
            m_automaticAttemptFailed=true;
            setManualEditing(!m_automaticReviewMode);
            m_saveStatus->setText(tr("自动定位未通过：%1 当前四点为保留的旧标定，并非本次自动结果；可点击“手动修改”调整。").arg(result.error));
            return;
        }
        // Apply only on the GUI thread; the worker owns a frozen frame and settings.
        if(m_autoEndpointCalibration) {m_endpointReviewRequired=true;m_autoEndpointCalibration=false;}
        const bool centerWasConfirmed=m_centerConfirmed;
        if(m_perspectiveConfirmed)onConfirmPerspective();
        if(!cornersOnly)m_sourceCenter=result.hub;
        m_currentPts=result.points;
        m_graduationSourceCenter=result.graduationCenter;
        m_interactiveLabel->setImageAndPoints(m_frame,m_currentPts);
        updatePreview();onConfirmPerspective();
        if(!m_perspectiveConfirmed) {
            m_automaticAttemptFailed=true;
            setManualEditing(!m_automaticReviewMode);
            m_saveStatus->setText(tr("自动视场校正未通过，请手动调整四切点后重试。"));
            return;
        }
        const auto center=m_previewLabel->getCenterPoint();
        m_centerConfirmed=!cornersOnly || (centerWasConfirmed && center.x>=0 && center.y>=0 &&
            center.x<m_profile.rectified_size && center.y<m_profile.rectified_size);
        if(m_centerConfirmed)m_previewLabel->selectScaleRatio(-1);
        m_autoDialReviewPending=true;
        if(!cornersOnly&&m_liquidMaskEnabled->isChecked()) {
            const auto& surface=detection.surface;
            m_previewLabel->setLiquidSurface(surface);
            m_previewLabel->showLiquidMask(true);
            m_liquidNote=surface.found?tr("已检测液面：橙色为液面影响区域（含边界缓冲），仅匹配下方数字与长刻度。"):
                surface.ambiguous?tr("存在多条可能的液面，请手动调整液面，或确认无液面后关闭显示。"):
                tr("未检测到可靠液面，请手动指定液面；无液体表盘可关闭液面显示。");
        }
        m_saveStatus->setText(tr("自动校正及中心检测完成：轮廓覆盖 %1%。请识别表盘数字，自动标定起止刻度后保存。")
            .arg(result.rimCoverage*100,0,'f',0));
        if(result.graduationSupport>=24&&!result.structuralRectification)m_saveStatus->setText(m_saveStatus->text()+
            tr(" 已结合 %1 组外围刻度边缘精修四切点。").arg(result.graduationSupport));
        setManualEditing(!m_automaticReviewMode);
        if(cornersOnly)
            m_saveStatus->setText(tr("切点检测完成，请复核轴心和刻度；需要检测液面时点击“重新检测液面”。"));
        else if(m_automaticReviewMode&&!m_saveBtn->isEnabled())
            m_saveStatus->setText(tr("自动校正与液面检测已完成；请点击“识别表盘数字”自动标定最小、最大刻度后确认。"));
        if(result.graduationLiquidExcluded)m_saveStatus->setText(m_saveStatus->text()+
            (result.graduationSupport>=24?tr(" 刻度环辅助精修已排除液面周边及液面以上区域。"):
                tr(" 液面影响区域已排除；下方可靠刻度不足，保留表圈边缘定位结果。")));
        else if(result.graduationLiquidAmbiguous)m_saveStatus->setText(m_saveStatus->text()+
            tr(" 液面位置有歧义，已跳过刻度环辅助精修。"));
        if(result.structuralRectification)m_saveStatus->setText(m_saveStatus->text()+
            tr(" 已采用 %1 条刻度的平面矫正，并通过左右对称与270°跨度检查；左侧四点为可手动调整的映射控制点。").arg(result.structuralSupport));
    });
    const auto frame=m_frame.clone();
    const int size=m_profile.rectified_size;
    const auto margin=m_profile.rectified_margin;
    const bool detectLiquid=!cornersOnly&&m_liquidMaskEnabled->isChecked();
    watcher->setFuture(QtConcurrent::run([frame,size,margin,detectLiquid] {
        Detection detection;
        try {
            detection.dial=AutoDialCalibration::detect(frame);
            if(detection.dial.valid()&&detectLiquid) {
                GaugeReaderEngine engine;cv::Mat inverse;
                const auto rectified=engine.rectifyPerspective(frame,detection.dial.points,inverse,size,margin);
                std::vector<cv::Point2f> center{detection.dial.hub};
                cv::perspectiveTransform(center,center,inverse.inv());
                detection.surface=detectLiquidSurface(rectified,center[0],size*.5-margin);
            }
        } catch(const std::exception& error) {
            detection.dial.error=QString::fromUtf8(error.what());
        } catch(...) {
            detection.dial.error=QStringLiteral(u"自动识别发生异常，请重试或手动标定。");
        }
        return detection;
    }));
}

void CardinalSelectorDialog::reject() {
    if (!m_saving) QDialog::reject();
}

std::pair<cv::Point2f,QJsonArray> CardinalSelectorDialog::sourceAnnotations() const {
    const auto rectifiedCenter=m_previewLabel->getCenterPoint();
    const auto rectifiedScales=m_previewLabel->getScalePoints();
    cv::Point2f center(-1,-1);
    if(rectifiedCenter.x>=0 && rectifiedCenter.y>=0) {
        std::vector<cv::Point2f> transformedCenter{rectifiedCenter};
        cv::perspectiveTransform(transformedCenter,transformedCenter,m_inverseHomography);
        center=transformedCenter[0];
        if(m_sourceCenter.x>=0 && m_sourceCenter.y>=0) {
            std::vector<cv::Point2f> originalCenter{m_sourceCenter};
            cv::perspectiveTransform(originalCenter,originalCenter,m_inverseHomography.inv());
            if(originalCenter[0]==rectifiedCenter) center=m_sourceCenter;
        }
    }
    auto scales=ScaleCalibration::transformed(rectifiedScales,m_inverseHomography);
    const auto originals=ScaleCalibration::transformed(m_sourceScalePoints,m_inverseHomography.inv());
    for(int i=0;i<scales.size();++i)
        for(int j=0;j<originals.size();++j)
            if(rectifiedScales[i]==originals[j]) { scales[i]=m_sourceScalePoints[j]; break; }
    return {center,scales};
}

void CardinalSelectorDialog::onSaveAndApply() {
    if (m_saving || m_autoDetectRunning) return;
    if(m_endpointReviewRequired) {
        m_saveStatus->setText(tr("自动刻度尚未通过检查。请确认透视、轴心及液面后重新识别，或手动修正刻度，再保存标定。"));return;
    }
    if(m_automaticReviewMode&&m_automaticAttemptFailed) {
        m_saveStatus->setText(tr("本次自动定位未通过，请先点击“手动修改”完成标定。"));return;
    }
    if (!m_perspectiveConfirmed || !m_centerConfirmed || m_inverseHomography.empty()) {
        m_saveStatus->setText(tr("请先确认四切点，并在矫正图点击轴心后完成刻度标定。")); return;
    }
    const auto points=CameraUndistortion::points(m_interactiveLabel->getPoints(),m_frame.size(),true);
    const auto rectifiedCenter=m_previewLabel->getCenterPoint();
    const auto rectifiedScales=m_previewLabel->getScalePoints();
    const auto radius=m_previewLabel->getPointerBaseRadius();
    ScaleCalibration::Mapping mapping;
    if (!ScaleCalibration::valid(rectifiedScales,m_profile.rectified_size,m_profile.rectified_size,rectifiedCenter) ||
        !ScaleCalibration::build(rectifiedScales,rectifiedCenter,{},m_profile.clockwise,mapping)) {
        m_saveStatus->setText(tr("无法保存：请检查矫正图的轴心、刻度点及增长方向。")); return;
    }
    auto geometryIssues=ScaleCalibration::geometryIssues(rectifiedScales,rectifiedCenter,m_profile.clockwise,
        m_profile.rectified_size*.5-m_profile.rectified_margin);
    if(m_autoDialReviewPending)geometryIssues<<tr("自动表盘定位及轴心需要复核。");
    // In the initial read-only preview, the explicit confirmation button is
    // the review acknowledgement. Manual mode retains its detailed checkbox.
    if(m_automaticReviewMode)m_geometryAcknowledged->setChecked(true);
    if(!geometryIssues.isEmpty() && !m_geometryAcknowledged->isChecked()) {
        m_saveStatus->setText(tr("请先复核几何检查提示，并勾选确认使用这些标定点。"));return;
    }
    const auto annotations=sourceAnnotations();
    const auto center=CameraUndistortion::point(annotations.first,m_frame.size(),true);
    const auto scales=CameraUndistortion::scales(annotations.second,m_frame.size(),true);
    QJsonObject calibration{{"perspective_correction_enabled", true},
        {"calibrated_width", m_frame.cols}, {"calibrated_height", m_frame.rows},
        {"center_x", center.x}, {"center_y", center.y}, {"pointer_base_radius", radius}};
    for (int i = 0; i < int(points.size()); ++i) {
        calibration[QString("pt_%1_x").arg(i)] = points[i].x;
        calibration[QString("pt_%1_y").arg(i)] = points[i].y;
    }
    calibration["scale_points"] = scales;
    if(!m_rangeConfirmed||m_rangeMinimum->value()>=m_rangeMaximum->value()) {
        m_saveStatus->setText(tr("请先识别左下和右下端点数字，检查最小值小于最大值后保存。"));return;
    }
    const QJsonObject meterRange{{"min_val",m_rangeMinimum->value()},{"max_val",m_rangeMaximum->value()},
        {"unit",QString::fromStdString(m_profile.unit)},{"source","ocr_endpoints"}};
    calibration["meter_range"]=meterRange;
    if (!validDeviceCalibration(calibration) || m_cameraId.isEmpty()) {
        m_saveStatus->setText(tr("无法保存：请将四个切点按 12、3、6、9 点钟顺序放在图像内，并设置有效中心点。"));
        return;
    }
    if (m_historicalSession) {
        m_profile.min_val=meterRange["min_val"].toDouble();m_profile.max_val=meterRange["max_val"].toDouble();
        m_profile.spec.range_min=m_profile.min_val;m_profile.spec.range_max=m_profile.max_val;
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
    m_rangeMinimum->setEnabled(false);m_rangeMaximum->setEnabled(false);
    m_autoScaleBtn->setEnabled(false);
    m_manualEditBtn->setEnabled(false);
    m_liquidControls->setEnabled(false);
    m_scaleControls->setEnabled(false);
    m_saveStatus->setText(tr("正在保存设备标定到数据库…"));
    m_previewLabel->setEnabled(false);
    m_confirmPerspectiveBtn->setEnabled(false);
    m_interactiveLabel->setEnabled(false);
    m_baseRadiusSpinBox->setEnabled(false);
    m_autoBtn->setEnabled(false);
    m_saveBtn->setEnabled(false); m_cancelBtn->setEnabled(false);
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, points, center, radius, scales, meterRange]() {
        const auto error = watcher->result(); watcher->deleteLater();
        m_saving = false;
        m_rangeMinimum->setEnabled(true);m_rangeMaximum->setEnabled(true);
        m_manualEditBtn->setEnabled(true);
        if (!error.isEmpty()) {
            m_saveStatus->setText(tr("数据库保存失败：%1；标定尚未保存，可重试。").arg(error));
            m_scaleControls->setEnabled(true);
            m_liquidControls->setEnabled(true);
            m_previewLabel->setEnabled(true); m_baseRadiusSpinBox->setEnabled(true);
            m_confirmPerspectiveBtn->setEnabled(true);m_autoBtn->setEnabled(true);
            m_saveBtn->setEnabled(true); m_cancelBtn->setEnabled(true);
            if(!m_historicalSession)setManualEditing(!m_automaticReviewMode);
            return;
        }
        DeviceConfigManager::instance().updateDeviceCalibration(m_cameraId, true, m_frame.cols, m_frame.rows,
            points[0].x, points[0].y, points[1].x, points[1].y,
            points[2].x, points[2].y, points[3].x, points[3].y, center.x, center.y, radius, false, scales, meterRange);
        m_profile.min_val=meterRange["min_val"].toDouble();m_profile.max_val=meterRange["max_val"].toDouble();
        m_profile.spec.range_min=m_profile.min_val;m_profile.spec.range_max=m_profile.max_val;
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
