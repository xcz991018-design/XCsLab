#pragma once
#include "AutoDialCalibration.hpp"
#include "CameraUndistortion.hpp"
#include "DeviceCalibration.hpp"
#include "OcrDialRange.hpp"

namespace UnattendedCalibration {
struct Candidate {
    QJsonObject geometry;
    QString error;
};

// A small image patch around each printed anchor checks placement without OCR.
// Coordinates and patch extent scale with the current camera stream resolution.
inline QByteArray patch(const cv::Mat& frame, cv::Point2f center, int radius) {
    const cv::Rect roi(cvRound(center.x)-radius,cvRound(center.y)-radius,2*radius+1,2*radius+1);
    if (radius<3 || (roi & cv::Rect(0,0,frame.cols,frame.rows))!=roi) return {};
    cv::Mat gray,small;
    if(frame.channels()==1) gray=frame(roi);
    else cv::cvtColor(frame(roi),gray,cv::COLOR_BGR2GRAY);
    cv::resize(gray,small,{24,24},0,0,cv::INTER_AREA);
    cv::Scalar mean,deviation;
    cv::meanStdDev(small,mean,deviation);
    if(deviation[0]<8) return {};
    return QByteArray(reinterpret_cast<const char*>(small.data),24*24);
}

inline bool matches(const QJsonObject& geometry,const cv::Mat& frame) {
    const double sx=double(frame.cols)/geometry["calibrated_width"].toInt(1);
    const double sy=double(frame.rows)/geometry["calibrated_height"].toInt(1);
    if(std::abs(sx/sy-1)>.01) return false;
    const auto anchors=geometry["scale_points"].toArray();
    if(anchors.size()<3) return false;
    for(const auto entry:anchors) {
        const auto p=entry.toObject();
        const auto reference=QByteArray::fromBase64(p["patch"].toString().toLatin1());
        const auto observed=patch(frame,{float(p["x"].toDouble()*sx),float(p["y"].toDouble()*sy)},
                                  cvRound(p["patch_radius"].toInt()*sx));
        if(reference.size()!=576 || observed.size()!=576) return false;
        cv::Mat correlation;
        cv::matchTemplate(cv::Mat(1,576,CV_8U,const_cast<char*>(observed.constData())),
                          cv::Mat(1,576,CV_8U,const_cast<char*>(reference.constData())),
                          correlation,cv::TM_CCOEFF_NORMED);
        if(correlation.at<float>(0)<.8f) return false;
    }
    return true;
}

inline Candidate solve(const cv::Mat& raw,const std::shared_ptr<std::atomic_bool>& cancel) {
    if(raw.empty()) return {{},QStringLiteral(u"图像为空")};
    const auto image=CameraUndistortion::image(raw);
    const auto dial=AutoDialCalibration::detect(image);
    if(!dial.valid()) return {{},dial.error};
    const bool verifiedRing=dial.structuralRectification || (dial.graduationSupport>=24 &&
        dial.graduationAspect>=.97 && dial.graduationResidual>=0 && dial.graduationResidual<=.025);
    const auto H=cv::getPerspectiveTransform(dial.points,std::vector<cv::Point2f>{{400,40},{760,400},{400,760},{40,400}});
    cv::Mat rectified;
    cv::warpPerspective(image,rectified,H,{800,800},cv::INTER_CUBIC);
    std::vector<cv::Point2f> origin{dial.structuralRectification?dial.graduationCenter:dial.hub};
    cv::perspectiveTransform(origin,origin,H);
    const auto surface=detectLiquidSurface(rectified,origin.front(),360);
    const auto ocr=PaddleScaleOcr::recognize(rectified,cancel,surface,PaddleScaleOcr::rectifiedDialBounds(rectified.size(),40));
    if(!ocr.valid()) return {{},ocr.error};
    const auto numbers=OcrDialRange::reliableNumbers(ocr.tokens,surface,{});
    const bool clockwise=OcrDialRange::detect(numbers,origin.front(),800,true,{}).valid;
    const auto scale=OcrDialRange::calibrate(rectified,ocr.tokens,origin.front(),360,clockwise,surface,{});
    if(!scale.valid()) return {{},scale.error};
    ScaleCalibration::Mapping mapping;
    if(!ScaleCalibration::build(scale.points,origin.front(),{},clockwise,mapping))
        return {{},QStringLiteral(u"刻度顺序不一致")};
    bool distributed=false;
    for(size_t i=1;i+1<mapping.ratios.size();++i) {
        const double ratio=mapping.ratios[i];
        if(std::abs(mapping.offsets[i]-ratio*mapping.offsets.back())>3)
            return {{},QStringLiteral(u"中间刻度与线性量程不一致")};
        distributed |= ratio>=.2 && ratio<=.8;
    }
    if(!verifiedRing) {
        // A nearly frontal dial may lack enough fine strokes for the structural
        // fit. Test the rim-derived transform against independent labeled majors.
        std::array<bool,4> bins{};
        std::vector<double> radii;
        for(const auto entry:scale.points) {
            const auto p=entry.toObject();
            const double ratio=p["ratio"].toDouble();
            radii.push_back(std::hypot(p["x"].toDouble()-origin.front().x,p["y"].toDouble()-origin.front().y));
            if(ratio>0 && ratio<1) bins[std::min(3,int(ratio*4))]=true;
        }
        const auto bounds=std::minmax_element(radii.begin(),radii.end());
        if(std::count(bins.begin(),bins.end(),true)<3 || *bounds.second-*bounds.first>360*.045)
            return {{},QStringLiteral(u"印刷刻度的覆盖范围或半径一致性不足以验证表盘平面")};
    }
    if(!distributed) return {{},QStringLiteral(u"缺少可验证量程中段的数字刻度")};
    const auto hub=CameraUndistortion::point(dial.hub,raw.size(),true);
    const auto handles=CameraUndistortion::points(dial.points,raw.size(),true);
    auto anchors=CameraUndistortion::scales(ScaleCalibration::transformed(scale.points,H.inv()),raw.size(),true);
    const int radius=cvRound(cv::norm(handles[0]-handles[2])*.04);
    for(int i=0;i<anchors.size();++i) {
        auto p=anchors[i].toObject();
        const auto pixels=patch(raw,{float(p["x"].toDouble()),float(p["y"].toDouble())},radius);
        if(pixels.isEmpty()) return {{},QStringLiteral(u"刻度局部纹理不足")};
        p["patch"]=QString::fromLatin1(pixels.toBase64());p["patch_radius"]=radius;anchors[i]=p;
    }
    // Preserve an explicitly recognized pressure unit only.
    QString unit;
    for(const auto token:ocr.tokens) {
        const auto t=token.toObject();
        const auto label=t["text"].toString().trimmed();
        if(t["score"].toDouble()<.8 || !QStringList{"MPa","kPa","Pa","bar","psi"}.contains(label)) continue;
        if(!unit.isEmpty() && unit!=label) return {{},QStringLiteral(u"表盘单位有歧义")};
        unit=label;
    }
    if(unit.isEmpty()) return {{},QStringLiteral(u"缺少可靠的压力单位")};
    QJsonObject range{{"min_val",scale.range.minimum},{"max_val",scale.range.maximum},{"unit",unit},
                      {"clockwise",clockwise},{"source","unattended_v1"}};
    QJsonObject cal{{"perspective_correction_enabled",true},{"calibrated_width",raw.cols},{"calibrated_height",raw.rows},
                    {"center_x",hub.x},{"center_y",hub.y},{"pointer_base_radius",50},{"meter_range",range},{"scale_points",anchors}};
    for(int i=0;i<4;++i) {
        cal[QString("pt_%1_x").arg(i)]=handles[i].x;
        cal[QString("pt_%1_y").arg(i)]=handles[i].y;
    }
    if(!validDeviceCalibration(cal)) return {{},QStringLiteral(u"映射到原图的标定点无效")};
    return {cal,{}};
}

inline bool agrees(const QJsonObject& first,const QJsonObject& second,const cv::Mat& frame) {
    if(first["meter_range"]!=second["meter_range"] || !matches(first,frame)) return false;
    const double sx=double(second["calibrated_width"].toInt())/first["calibrated_width"].toInt();
    const double sy=double(second["calibrated_height"].toInt())/first["calibrated_height"].toInt();
    const double tolerance=.015*std::hypot(second["pt_0_x"].toDouble()-second["pt_2_x"].toDouble(),
                                           second["pt_0_y"].toDouble()-second["pt_2_y"].toDouble());
    for(const auto& key:QStringList{"center","pt_0","pt_1","pt_2","pt_3"}) {
        if(std::hypot(first[key+"_x"].toDouble()*sx-second[key+"_x"].toDouble(),
                      first[key+"_y"].toDouble()*sy-second[key+"_y"].toDouble())>tolerance) return false;
    }
    const auto a=first["scale_points"].toArray(), b=second["scale_points"].toArray();
    int shared=0;
    for(const auto left:a) for(const auto right:b) {
        const auto p=left.toObject(),q=right.toObject();
        if(std::abs(p["ratio"].toDouble()-q["ratio"].toDouble())>1e-6) continue;
        if(std::hypot(p["x"].toDouble()*sx-q["x"].toDouble(),p["y"].toDouble()*sy-q["y"].toDouble())>tolerance) return false;
        ++shared;
    }
    return shared>=3;
}
}
