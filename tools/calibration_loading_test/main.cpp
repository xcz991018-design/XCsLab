#include "CardinalSelectorDialog.hpp"
#include <QApplication>
#include <QEventLoop>
#include <QSemaphore>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent/QtConcurrent>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    try {
        GaugeProfile profile;
        cv::Mat frame(600, 600, CV_8UC3, cv::Scalar(220,220,220));
        CardinalSelectorDialog dialog(frame, profile, {}, nullptr, true);
        dialog.show();
        app.processEvents();
        auto* progress=dialog.findChild<QProgressBar*>("calibrationLoadingProgress");
        auto* manual=dialog.findChild<QPushButton*>("manualEditCalibrationButton");
        auto* save=dialog.findChild<QPushButton*>("saveCalibrationButton");
        require(progress && !progress->isVisible(), "historical sessions must wait for explicit auto detection");
        require(QMetaObject::invokeMethod(&dialog,"onAutoDetect",Qt::DirectConnection), "auto detection slot missing");
        require(progress->isVisible() && progress->maximum()==0 && !save->isEnabled() && !manual->isEnabled(),
            "auto detection must return to the event loop with visible busy feedback and edits locked");
        QEventLoop loop;
        QTimer poll, timeout;
        int heartbeats=0;
        QObject::connect(&poll,&QTimer::timeout,&loop,[&] {
            ++heartbeats;
            if(!progress->isVisible())loop.quit();
        });
        QObject::connect(&timeout,&QTimer::timeout,&loop,&QEventLoop::quit);
        poll.start(5);timeout.start(15000);loop.exec();
        require(!progress->isVisible() && heartbeats>0 && manual->isEnabled(), "failure must finish and restore controls");
        require(dialog.findChild<QLabel*>("calibrationSaveStatus")->text().contains(QStringLiteral(u"自动定位未通过")),
            "failed detection must provide an actionable explanation");

        QMetaObject::invokeMethod(&dialog,"onConfirmPerspective",Qt::DirectConnection);
        auto* original=dialog.findChild<InteractiveImageLabel*>("perspectiveImage");
        auto* preview=dialog.findChild<InteractiveImageLabel*>("rectifiedCalibrationImage");
        auto* confirm=dialog.findChild<QPushButton*>("confirmPerspectiveButton");
        require(original->isEnabled() && !confirm->isVisible(), "confirmed perspective must permit direct corner dragging");
        app.processEvents();
        const auto before=original->getPoints();
        const double scale=std::min(original->width()/600.0,original->height()/600.0);
        const QPointF from((original->width()-int(600*scale))/2+before[0].x*scale,
            (original->height()-int(600*scale))/2+before[0].y*scale);
        QMouseEvent press(QEvent::MouseButtonPress,from,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(original,&press);
        QMouseEvent move(QEvent::MouseMove,from+QPointF(12,8),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(original,&move);
        QMouseEvent releaseMouse(QEvent::MouseButtonRelease,from+QPointF(12,8),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(original,&releaseMouse);
        require(original->getPoints()[0]!=before[0] && original->isEnabled() && preview->isEnabled() && !confirm->isVisible(),
            "dragging a confirmed corner must update perspective immediately without unlocking via a button");
        auto* centerButton=dialog.findChild<QPushButton*>("pickCenterButton");
        auto* minimum=dialog.findChild<QPushButton*>("pickMinimumScaleButton");
        auto* maximum=dialog.findChild<QPushButton*>("pickMaximumScaleButton");
        auto* intermediate=dialog.findChild<QPushButton*>("pickIntermediateScaleButton");
        centerButton->click();
        require(centerButton->isChecked() && preview->pickingCenter(), "center button must reflect the active mouse tool");
        QMouseEvent pickCenter(QEvent::MouseButtonPress,QPointF(preview->width()/2.,preview->height()/2.),
            Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(preview,&pickCenter);
        require(!centerButton->isChecked() && !preview->pickingCenter(), "placing the center must clear the pending highlight");
        minimum->click();
        require(minimum->isChecked() && preview->pendingScaleRatio()==0, "minimum selection must be visible");
        maximum->click();
        require(maximum->isChecked() && !minimum->isChecked() && preview->pendingScaleRatio()==1,
            "switching tools must leave only the actual active tool highlighted");
        intermediate->click();
        require(intermediate->isChecked() && !maximum->isChecked() && preview->pendingScaleRatio()==.5 &&
            intermediate->text().contains("50%"), "intermediate tool must display its selected ratio");
        QApplication::sendEvent(preview,&pickCenter);
        require(!intermediate->isChecked() && preview->pendingScaleRatio()<0, "placing a scale point must clear its highlight");

        InteractiveImageLabel scaleImage;
        scaleImage.setRectifiedMode(true);scaleImage.resize(600,600);
        scaleImage.setImageAndPoints(frame,{},cv::Point2f(300,300));
        const QJsonArray scales{
            QJsonObject{{"x",100},{"y",300},{"ratio",0}},
            QJsonObject{{"x",300},{"y",100},{"ratio",.5}},
            QJsonObject{{"x",500},{"y",300},{"ratio",1}}};
        scaleImage.setScalePoints(scales);scaleImage.show();app.processEvents();
        QMouseEvent selectLine(QEvent::MouseButtonPress,QPointF(300,200),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(&scaleImage,&selectLine);
        require(scaleImage.hasSelectedIntermediateScale(), "clicking the middle of an intermediate ray must select it");
        QKeyEvent removeLine(QEvent::KeyPress,Qt::Key_Delete,Qt::NoModifier);
        QApplication::sendEvent(&scaleImage,&removeLine);
        require(scaleImage.getScalePoints().size()==2 && !scaleImage.hasSelectedIntermediateScale() &&
            scaleImage.getScalePoints()[0].toObject()["ratio"].toDouble()==0 &&
            scaleImage.getScalePoints()[1].toObject()["ratio"].toDouble()==1,
            "Delete must remove only the selected intermediate point and preserve endpoints");
        QMouseEvent selectMinimum(QEvent::MouseButtonPress,QPointF(100,300),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(&scaleImage,&selectMinimum);
        QApplication::sendEvent(&scaleImage,&removeLine);
        require(scaleImage.getScalePoints().size()==2, "range endpoints must be protected from individual deletion");
        scaleImage.setScalePoints(scales);
        QMouseEvent selectPoint(QEvent::MouseButtonPress,QPointF(300,100),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(&scaleImage,&selectPoint);
        require(scaleImage.hasSelectedIntermediateScale(), "clicking an intermediate endpoint must also select it");
        scaleImage.deleteSelectedIntermediateScale();
        require(scaleImage.getScalePoints().size()==2, "button action must delete the selected intermediate point");

        // Occupy the worker pool so cancellation definitely precedes completion.
        auto* pool=QThreadPool::globalInstance();pool->waitForDone();pool->setMaxThreadCount(1);
        QSemaphore entered, release;
        auto blocker=QtConcurrent::run([&] {entered.release();release.acquire();});
        entered.acquire();
        auto* closing=new CardinalSelectorDialog(frame, profile);
        closing->show();
        const bool initialFeedback=closing->findChild<QProgressBar*>("calibrationLoadingProgress")->isVisible();
        app.processEvents(); // Starts the automatic detection scheduled on opening.
        closing->reject();
        const bool cancelled=!closing->isVisible();
        delete closing;
        release.release();blocker.waitForFinished();pool->waitForDone();app.processEvents();
        require(cancelled, "users must be able to cancel while detection is pending");
        require(initialFeedback, "opening a calibration must immediately show loading feedback");
        std::cout << "PASS: busy feedback, event loop responsiveness, failure recovery, cancellation and safe destruction\n";
        return 0;
    } catch(const std::exception& e) {std::cerr << e.what() << '\n';return 1;}
}
