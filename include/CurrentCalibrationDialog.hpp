#pragma once
#include "CardinalSelectorDialog.hpp"
#include "CameraUndistortion.hpp"
#include "InspectionCalibration.hpp"
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QTableWidget>

// A viewer of persisted annotations. It has no calibration or save actions.
class CurrentCalibrationDialog : public QDialog {
public:
    explicit CurrentCalibrationDialog(const QString& cameraId,QWidget* parent=nullptr):QDialog(parent) {
        setWindowTitle(tr("查看当前标定 — %1").arg(cameraId));resize(1120,820);
        auto* layout=new QVBoxLayout(this);
        m_status=new QLabel(tr("正在读取设备已保存的标定…"),this);m_status->setWordWrap(true);layout->addWidget(m_status);
        auto* images=new QHBoxLayout;
        for(int i=0;i<2;++i) {
            auto* column=new QVBoxLayout;
            column->addWidget(new QLabel(i?tr("透视校正与刻度分布"):tr("原图坐标：切点、轴心与刻度"),this));
            auto* view=new InteractiveImageLabel(this);view->setMinimumSize(320,300);
            view->setAttribute(Qt::WA_TransparentForMouseEvents);view->setFocusPolicy(Qt::NoFocus);
            column->addWidget(view,1);images->addLayout(column,1);
            if(i)m_rectified=view;else m_original=view;
        }
        layout->addLayout(images,1);
        m_table=new QTableWidget(0,4,this);m_table->setHorizontalHeaderLabels({tr("标定项"),tr("数值 / 类型"),tr("原图 X"),tr("原图 Y")});
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        m_table->setMaximumHeight(240);layout->addWidget(m_table);
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Close,this);connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);layout->addWidget(buttons);
    }
    bool setDevice(const DiscoveredCameraItem& device) {
        m_device=device;
        m_table->setRowCount(0);
        if(!canRecognizeDevice(device)) {m_status->setText(tr("该设备尚未保存有效标定，请通过“表计标定”完成标定。"));return false;}
        m_summary=tr("已保存标定（只读）｜量程 %1～%2 %3｜原图 %4×%5｜刻度点 %6 个。")
            .arg(device.meterRange["min_val"].toDouble()).arg(device.meterRange["max_val"].toDouble()).arg(device.meterRange["unit"].toString())
            .arg(device.calibratedWidth).arg(device.calibratedHeight).arg(device.scalePoints.size());
        m_status->setText(m_summary+tr("正在读取上次标定保存的原图。"));
        auto row=[&](QString name,QString value,double x,double y) {
            const int r=m_table->rowCount();m_table->insertRow(r);
            const QStringList cells{name,value,QString::number(x,'f',2),QString::number(y,'f',2)};
            for(int i=0;i<4;++i)m_table->setItem(r,i,new QTableWidgetItem(cells[i]));
        };
        const auto points=corners();
        for(int i=0;i<4;++i)row(tr("透视切点 %1").arg(QStringList{"12H","3H","6H","9H"}[i]),{},points[i].x,points[i].y);
        row(tr("轴心"),tr("轴帽屏蔽半径 %1（800 像素基准）").arg(device.pointerBaseRadius),device.centerX,device.centerY);
        for(const auto& value:device.scalePoints) {
            const auto p=value.toObject();const double lo=device.meterRange["min_val"].toDouble(),hi=device.meterRange["max_val"].toDouble();
            row(tr("刻度 %1").arg(MinorTickPreview::displayValue(lo+(hi-lo)*p["ratio"].toDouble())),p["source"].toString()=="minor_tick_inferred"?tr("小刻度（指针遮挡推算）"):(p["source"].toString()=="minor_tick"?tr("小刻度"):tr("大刻度 / 锚点")),p["x"].toDouble(),p["y"].toDouble());
        }
        render({});return true;
    }
    void setFrame(const cv::Mat& frame,const QString& error) {
        if(!frame.empty()&&std::abs(double(frame.cols)*m_device.calibratedHeight/(double(frame.rows)*m_device.calibratedWidth)-1)>.01) {
            setFrame({},tr("归档原图宽高比与标定参数不一致"));return;
        }
        try {render(frame);m_status->setText(m_summary+
            (frame.empty()?tr("%1。当前仅显示标定坐标示意图；未连接摄像头。").arg(error):tr("底图为上次成功标定时保存的原图。%1").arg(error)));}
        catch(const cv::Exception& e){m_status->setText(tr("标定底图显示失败：%1").arg(QString::fromUtf8(e.what())));}
    }
    void showError(const QString& error){m_status->setText(tr("无法读取当前标定：%1").arg(error));}
private:
    std::vector<cv::Point2f> corners() const {return {{float(m_device.pt0_x),float(m_device.pt0_y)},{float(m_device.pt1_x),float(m_device.pt1_y)},{float(m_device.pt2_x),float(m_device.pt2_y)},{float(m_device.pt3_x),float(m_device.pt3_y)}};}
    void render(const cv::Mat& frame) {
        // Cap the viewer canvas; saved coordinates and the parameter table stay unchanged.
        const double scale=std::min(1.,1200./std::max(m_device.calibratedWidth,m_device.calibratedHeight));
        const cv::Size size(std::max(1,cvRound(m_device.calibratedWidth*scale)),std::max(1,cvRound(m_device.calibratedHeight*scale)));
        cv::Mat image(size,CV_8UC3,cv::Scalar(235,235,235));if(!frame.empty())cv::resize(frame,image,size);
        auto points=corners();for(auto& p:points)p*=float(scale);
        const cv::Point2f center(float(m_device.centerX*scale),float(m_device.centerY*scale));
        auto scales=ScaleCalibration::scaled(m_device.scalePoints,scale,scale);
        const double lo=m_device.meterRange["min_val"].toDouble(),hi=m_device.meterRange["max_val"].toDouble();
        m_original->showSourceAnnotations(true);m_original->setImageAndPoints(image,points,center);m_original->setScaleRange(lo,hi);m_original->setScalePoints(scales);
        m_original->setPointerBaseRadius(0);
        auto corrected=CameraUndistortion::image(image);points=CameraUndistortion::points(points,size);
        scales=CameraUndistortion::scales(scales,size);const auto hub=CameraUndistortion::point(center,size);
        const auto H=cv::getPerspectiveTransform(points,std::vector<cv::Point2f>{{400,40},{760,400},{400,760},{40,400}});
        cv::Mat rectified;cv::warpPerspective(corrected,rectified,H,{800,800},cv::INTER_CUBIC);
        std::vector<cv::Point2f> centers{hub};cv::perspectiveTransform(centers,centers,H);
        m_rectified->setRectifiedMode(true);m_rectified->setImageAndPoints(rectified,{},centers.front());
        m_rectified->setPointerBaseRadius(m_device.pointerBaseRadius);m_rectified->setScaleRange(lo,hi);
        m_rectified->setScalePoints(ScaleCalibration::transformed(scales,H));
    }
    DiscoveredCameraItem m_device;QString m_summary;QLabel* m_status;InteractiveImageLabel *m_original,*m_rectified;QTableWidget* m_table;
};
