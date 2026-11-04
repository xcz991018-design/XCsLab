#include "CameraUndistortion.hpp"
#include "OcrWorker.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <iostream>
#include <future>

static void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        for(auto size:{cv::Size(576,324),cv::Size(2304,1296),cv::Size(3840,2160),cv::Size(576,324)}) {
            cv::Mat input(size,CV_8UC3),expected;cv::randu(input,0,256);
            auto K=CameraUndistortion::matrix(size);
            QElapsedTimer timer;timer.start();
            cv::undistort(input,expected,K,CameraUndistortion::coefficients(),K);
            const auto baseline=timer.nsecsElapsed();
            CameraUndistortion::image(input);timer.restart();
            auto actual=CameraUndistortion::image(input);
            const auto cached=timer.nsecsElapsed();
            const double difference=cv::norm(expected,actual,cv::NORM_INF);
            std::cout<<size<<" max_pixel_difference="<<difference<<" baseline_ms="<<baseline/1e6<<" cached_ms="<<cached/1e6<<std::endl;
            require(difference==0,"cached undistortion must preserve every pixel");
        }
        if(argc!=3)throw std::runtime_error("usage: performance_test python worker_fixture.py");
        QTemporaryDir dir;require(dir.isValid(),"temporary directory");
        const auto cancel=std::make_shared<std::atomic_bool>(false);
        auto environment=QProcessEnvironment::systemEnvironment();
        auto run=[&](QString name,QString image="ok") {
            const auto output=dir.filePath(name);
            const auto error=OcrWorker::instance().run(argv[1],argv[2],environment,image,output,cancel);
            require(error.isEmpty(),qPrintable(error));
            QFile file(output);require(file.open(QIODevice::ReadOnly),"response exists");
            return QJsonDocument::fromJson(file.readAll()).object()["pid"].toInt();
        };
        int first=run("first.json"),second=run("second.json");
        require(first==second,"process must be reused");
        auto parallel=std::async(std::launch::async,[&]{return run("parallel.json");});
        require(run("concurrent.json")==first && parallel.get()==first,"concurrent callers share one worker");
        auto pending=std::async(std::launch::async,[&]{return OcrWorker::instance().run(argv[1],argv[2],environment,"sleep",dir.filePath("cancel.json"),cancel);});
        QThread::msleep(200);cancel->store(true);
        require(!pending.get().isEmpty(),"in-flight cancellation");cancel->store(false);
        require(run("restart.json")!=first,"cancelled process must restart");
        const auto crash=OcrWorker::instance().run(argv[1],argv[2],environment,"crash",dir.filePath("crash.json"),cancel);
        require(!crash.isEmpty(),"worker crash must be reported");
        run("recovered.json");
        std::cout<<"PASS cache, reuse, concurrency, cancellation and recovery"<<std::endl;
    }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
}
