#pragma once
#include "AutoScaleCalibration.hpp"
#include "LiquidSurface.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <atomic>
#include <memory>

namespace PaddleScaleOcr {
struct Recognition { QJsonArray tokens; QString error; bool valid() const {return error.isEmpty();} };
struct InputRegion {cv::Mat image;int top{0};};
inline InputRegion inputRegion(const cv::Mat& image,const LiquidSurface& surface) {
    if(!surface.found||image.empty())return {image,0};
    cv::Mat masked=image.clone();int top=image.rows;
    for(int x=0;x<image.cols;++x) {
        const double boundary=surface.yAt(x)+surface.margin;
        if(!std::isfinite(boundary))return {{},0};
        const int first=int(std::clamp(std::floor(boundary)+1,0.,double(image.rows)));
        top=std::min(top,first);
        if(first>0)masked(cv::Rect(x,0,1,first)).setTo(cv::Scalar::all(255));
    }
    if(top>=image.rows)return {{},top};
    return {masked(cv::Rect(0,top,image.cols,image.rows-top)).clone(),top};
}
inline QJsonArray restoreTokens(const QJsonArray& tokens,int top,const LiquidSurface& surface) {
    QJsonArray restored;
    for(const auto& entry:tokens) {
        auto token=entry.toObject();QJsonArray polygon;bool excluded=false;
        for(const auto& vertex:token["polygon"].toArray()) {
            const auto p=vertex.toArray();if(p.size()!=2){excluded=true;break;}
            polygon.append(QJsonArray{p[0].toDouble(),p[1].toDouble()+top});
        }
        if(polygon.size()!=4)continue;
        for(int i=0;i<4;++i) {
            const auto a=polygon[i].toArray(),b=polygon[(i+1)%4].toArray();
            for(int k=0;k<=8;++k) {
                const double t=k/8.;
                excluded|=surface.excludes(a[0].toDouble()*(1-t)+b[0].toDouble()*t,a[1].toDouble()*(1-t)+b[1].toDouble()*t);
            }
        }
        if(!excluded){token["polygon"]=polygon;restored.append(token);}
    }
    return restored;
}
inline Recognition recognize(const cv::Mat& image,const std::shared_ptr<std::atomic_bool>& cancelled,const LiquidSurface& surface={}) {
    Recognition result;
    try {
        if(surface.ambiguous){result.error=QStringLiteral(u"液面位置有歧义，未执行 OCR。");return result;}
        const auto region=inputRegion(image,surface);
        if(region.image.empty()){result.error=QStringLiteral(u"液面屏蔽后没有有效 OCR 区域。");return result;}
        const auto appDir=QCoreApplication::applicationDirPath();
        QString ocrDir=appDir+"/ocr";
        if(!QFile::exists(ocrDir+"/offline.json") && !QFile::exists(ocrDir+"/paddle_scale_ocr.py") && QFile::exists(appDir+"/../ocr/paddle_scale_ocr.py"))ocrDir=QDir(appDir+"/../ocr").absolutePath();
        const bool portable=QFile::exists(ocrDir+"/offline.json");
        const QString script=portable?ocrDir+"/paddle_scale_ocr.py":qEnvironmentVariable("GAUGE_OCR_SCRIPT",ocrDir+"/paddle_scale_ocr.py");
        QString python=portable?ocrDir+"/runtime/python.exe":qEnvironmentVariable("GAUGE_OCR_PYTHON");
        if(python.isEmpty()) {
            python=ocrDir+"/runtime/python.exe";
            if(!QFile::exists(python))python=ocrDir+"/venv/Scripts/python.exe";
            if(!QFile::exists(python)) {
                result.error=QStringLiteral(u"当前构建目录缺少 OCR 运行环境，请部署完整离线 ocr 目录，或设置 GAUGE_OCR_PYTHON 指向已安装 PaddleOCR 的 Python。");
                return result;
            }
        }
        if(portable&&!QFile::exists(python)) {result.error=QStringLiteral(u"离线 OCR 运行时缺失，请重新完整解压部署包（含 ocr/runtime）。");return result;}
        if(!QFile::exists(script)) {result.error=QStringLiteral(u"未找到 PaddleOCR 桥接脚本，请部署 ocr 目录或设置 GAUGE_OCR_SCRIPT。");return result;}
        QTemporaryDir temporary;
        if(!temporary.isValid()) {result.error=QStringLiteral(u"无法创建 OCR 临时目录。");return result;}
        const auto input=temporary.filePath("dial.png"),output=temporary.filePath("result.json");
        std::vector<uchar> encoded; cv::imencode(".png",region.image,encoded);
        QFile file(input);
        if(!file.open(QIODevice::WriteOnly)||file.write(reinterpret_cast<const char*>(encoded.data()),qint64(encoded.size()))!=qint64(encoded.size())) {result.error=QStringLiteral(u"无法写入 OCR 图像。");return result;}file.close();
        QProcess process;
        auto environment=QProcessEnvironment::systemEnvironment();
        for(const auto& name:{"PYTHONHOME","PYTHONPATH","PYTHONUSERBASE","VIRTUAL_ENV","__PYVENV_LAUNCHER__"})environment.remove(name);
        environment.insert("PYTHONDONTWRITEBYTECODE","1");
        process.setProcessEnvironment(environment);
        process.start(python,{"-B",script,"--image",input,"--output",output});
        if(!process.waitForStarted(5000)) {
            result.error=portable?QStringLiteral(u"离线 OCR 运行时启动失败，请完整解压部署包并检查运行库。"):
                QStringLiteral(u"无法启动 PaddleOCR Python，请按 SERVICE.md 安装 OCR 环境并配置 GAUGE_OCR_PYTHON。");return result;
        }
        QElapsedTimer timer;timer.start();QByteArray errors;
        while(process.state()!=QProcess::NotRunning) {
            process.waitForFinished(100);
            process.readAllStandardOutput();errors+=process.readAllStandardError();errors=errors.right(3000);
            if(cancelled->load()||timer.elapsed()>180000) {
                process.kill();process.waitForFinished(3000);
                result.error=cancelled->load()?QStringLiteral(u"OCR 已取消。"):QStringLiteral(u"PaddleOCR 超时（180秒），请检查本地模型配置与电脑性能。");return result;
            }
        }
        QFile response(output);
        if(!response.open(QIODevice::ReadOnly)) {result.error=QStringLiteral(u"PaddleOCR 未返回结果：%1").arg(QString::fromUtf8(errors.right(1200)));return result;}
        QJsonParseError parseError;
        const auto doc=QJsonDocument::fromJson(response.readAll(),&parseError);const auto data=doc.object();
        if(parseError.error!=QJsonParseError::NoError || !data["tokens"].isArray() || process.exitStatus()!=QProcess::NormalExit || process.exitCode()!=0) {
            result.error=QStringLiteral(u"PaddleOCR 识别失败：%1。使用的 Python：%2。请检查 OCR 环境及模型配置。")
                .arg(data["error"].toString(QString::fromUtf8(errors.right(1200))),python);return result;
        }
        result.tokens=surface.found?restoreTokens(data["tokens"].toArray(),region.top,surface):data["tokens"].toArray();return result;
    } catch(const std::exception& error) {result.error=QString::fromUtf8(error.what());return result;}
}
// Upright dial inspection accepts the built-in 0..2 vocabulary in steps of 0.1.
inline bool parseAllowedValues(QString text,std::vector<double>& values) {
    values.clear();text.replace(QChar(0xff0c),',');text.replace(QChar(0x3001),',');
    for(const auto& part:text.split(QRegularExpression("[,;\\s]+"),Qt::SkipEmptyParts)) {
        bool ok=false;const double value=part.toDouble(&ok);
        if(!ok||!std::isfinite(value))return false;
        values.push_back(value);
    }
    return true;
}
inline const std::vector<double>& builtInDialValues() {
    static const auto values=[] {std::vector<double> result;for(int i=0;i<=20;++i)result.push_back(i/10.0);return result;}();
    return values;
}
inline QJsonArray numericTokens(const QJsonArray& tokens,const std::vector<double>& allowedValues=builtInDialValues()) {
    const QRegularExpression numeric(QStringLiteral("^[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)$"));
    struct Candidate {QJsonObject token;cv::Point2f center;double extent;};
    std::vector<Candidate> candidates;
    for(const auto& entry:tokens) {
        auto token=entry.toObject();QString text=token["text"].toString().trimmed();
        text.replace(QChar(0xff0e),'.');text.replace(QChar(0x2212),'-');
        // Upright dial labels use decimal notation, never zero-padded integers.
        // Keep the raw OCR text while recovering an omitted dot in 01..09.
        if(text.size()==2&&text[0]=='0'&&text[1]>='1'&&text[1]<='9') {
            const QString rawText=token["text"].toString();text.insert(1,'.');
            if(!token.contains("raw_text"))token["raw_text"]=rawText;token["text"]=text;
        }
        const double score=token["score"].toDouble();const auto polygon=token["polygon"].toArray();
        if(!numeric.match(text).hasMatch()||!std::isfinite(score)||score<.65||polygon.size()!=4)continue;
        if(!allowedValues.empty()) {
            const double value=text.toDouble();
            if(std::none_of(allowedValues.begin(),allowedValues.end(),[&](double allowed){
                return std::abs(allowed-value)<=1e-6*std::max(1.0,std::abs(allowed));
            }))continue;
        }
        cv::Point2f center(0,0);std::vector<cv::Point2f> points;
        for(const auto& vertex:polygon) {
            const auto p=vertex.toArray();
            if(p.size()!=2||!p[0].isDouble()||!p[1].isDouble()||!std::isfinite(p[0].toDouble())||!std::isfinite(p[1].toDouble()))break;
            const cv::Point2f point{float(p[0].toDouble()),float(p[1].toDouble())};points.push_back(point);center+=point*.25f;
        }
        if(points.size()!=4)continue;
        const double extent=std::max(cv::norm(points[0]-points[1]),cv::norm(points[1]-points[2]));
        if(extent>1)candidates.push_back({token,center,extent});
    }
    std::sort(candidates.begin(),candidates.end(),[](const Candidate& a,const Candidate& b){
        const bool uprightA=a.token["orientation_preferred"].toBool(),uprightB=b.token["orientation_preferred"].toBool();
        if(uprightA!=uprightB)return uprightA;
        return a.token["score"].toDouble()>b.token["score"].toDouble();
    });
    std::vector<Candidate> selected;
    for(const auto& candidate:candidates) {
        const bool duplicate=std::any_of(selected.begin(),selected.end(),[&](const Candidate& existing){
            return cv::norm(candidate.center-existing.center)<std::min(candidate.extent,existing.extent)*.4;
        });
        if(!duplicate)selected.push_back(candidate);
    }
    QJsonArray output;for(const auto& candidate:selected)output.append(candidate.token);return output;
}
inline AutoScaleCalibration::Result run(const cv::Mat& image,cv::Point2f center,double radius,
        double minimum,double maximum,bool clockwise,const std::shared_ptr<std::atomic_bool>& cancelled) {
    const auto recognition=recognize(image,cancelled);
    if(!recognition.valid()) {AutoScaleCalibration::Result result;result.error=recognition.error;return result;}
    return AutoScaleCalibration::match(AutoScaleCalibration::numbers(recognition.tokens,minimum,maximum),
        AutoScaleCalibration::ticks(image,center,radius),center,radius,minimum,maximum,clockwise);
}
}
