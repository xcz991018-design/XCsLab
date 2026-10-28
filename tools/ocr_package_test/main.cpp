#include "PaddleScaleOcr.hpp"
#include <QCommandLineParser>
#include <QSaveFile>
#include <iostream>

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    QCommandLineParser parser;parser.addHelpOption();
    parser.addOption({"output","Write validation report.","path"});parser.process(app);
    const auto root=QCoreApplication::applicationDirPath();
    QJsonObject report;int exitCode=1;
    try {
        if(!QFile::exists(root+"/ocr/offline.json"))throw std::runtime_error("Offline package marker missing");
        QFile fixture(root+"/ocr/validation/dial.png");
        if(!fixture.open(QIODevice::ReadOnly))throw std::runtime_error("Validation image missing");
        const auto bytes=fixture.readAll();
        const std::vector<uchar> data(bytes.begin(),bytes.end());
        const auto image=cv::imdecode(data,cv::IMREAD_COLOR);
        const auto cancelled=std::make_shared<std::atomic_bool>(false);
        QElapsedTimer timer;timer.start();
        const auto result=PaddleScaleOcr::run(image,{400,400},360,0,1,true,cancelled);
        report={{"passed",result.valid()&&result.points.size()==6},{"error",result.error},
            {"elapsed_ms",double(timer.elapsed())},{"points",result.points},{"package_root",root}};
        if(report["passed"].toBool())exitCode=0;
    }catch(const std::exception& error){report={{"passed",false},{"error",QString::fromUtf8(error.what())}};}
    if(parser.isSet("output")) {
        QSaveFile file(parser.value("output"));
        if(!file.open(QIODevice::WriteOnly)||file.write(QJsonDocument(report).toJson())<0||!file.commit())return 2;
    }
    std::cout<<QJsonDocument(report).toJson().constData();return exitCode;
}
