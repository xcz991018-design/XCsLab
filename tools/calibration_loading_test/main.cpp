#include "CardinalSelectorDialog.hpp"
#include "CameraInfoEditDialog.hpp"
#include "CameraUndistortion.hpp"
#include "GaugeReaderEngine.hpp"
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
        CameraInfoEditDialog archive("",nullptr,false,{},true);
        archive.setAttribute(Qt::WA_DontShowOnScreen);
        require(!archive.findChild<QComboBox*>("deviceGaugeTemplate"),"archive must no longer require template selection");
        auto* archiveSave=archive.findChild<QPushButton*>("saveDeviceArchiveButton");
        require(archiveSave&&archiveSave->isEnabled(),"archive save must be available without template loading");
        // Check the supplied camera model against independent forward projection,
        // then ensure resolution scaling and annotation persistence round-trip.
        const cv::Size nativeSize(2304,1296), size(576,324);
        const double x=.4, y=-.2, r2=x*x+y*y;
        const double radial=1-.303680902*r2+.096542819*r2*r2-.0141173611*r2*r2*r2;
        const cv::Point2f corrected(float(1000.25230*x+1168.05784),float(1000.48207*y+660.617147));
        const cv::Point2f expectedRaw(
            float(1000.25230*(x*radial+2*(-.000133464040)*x*y+.0000526209523*(r2+2*x*x))+1168.05784),
            float(1000.48207*(y*radial+(-.000133464040)*(r2+2*y*y)+2*.0000526209523*x*y)+660.617147));
        const auto raw=CameraUndistortion::point(corrected,nativeSize,true);
        require(cv::norm(raw-expectedRaw)<.001,"D ordering and forward distortion must match the supplied model");
        require(cv::norm(CameraUndistortion::point(raw,nativeSize)-corrected)<.001,"raw points must undistort accurately");
        require(cv::norm(CameraUndistortion::point(raw*.25f,size)-corrected*.25f)<.001,"K must scale with snapshot resolution");
        require(CameraUndistortion::point({-1,-1},size)==cv::Point2f(-1,-1),"unset hub must stay unset");
        require(CameraUndistortion::image({}).empty(),"empty images must remain empty");

        GaugeProfile lensProfile;
        lensProfile.calibrated_width=size.width; lensProfile.calibrated_height=size.height;
        lensProfile.perspective_pts={{288,92},{358,162},{288,232},{218,162}};
        lensProfile.center={288,162}; lensProfile.auto_detect_roi=false;
        lensProfile.scalePoints=QJsonArray{
            QJsonObject{{"x",358},{"y",162},{"ratio",0},{"origin_x",288},{"origin_y",158}},
            QJsonObject{{"x",288},{"y",232},{"ratio",1},{"origin_x",288},{"origin_y",158}}};
        const auto saved=lensProfile.toJson();
        cv::Mat lensFrame(size,CV_8UC3,cv::Scalar(210,210,210));
        cv::circle(lensFrame,{288,162},70,{20,20,20},2);
        cv::line(lensFrame,{288,162},{335,195},{10,10,10},3);
        const auto originalPixels=lensFrame.clone();
        GaugeReaderEngine engine;
        const auto result=engine.process(lensFrame,lensProfile,GaugeRunMode::Debug);
        require(!result.undistortedImage.empty() && cv::norm(result.undistortedImage,
            CameraUndistortion::image(lensFrame),cv::NORM_INF)==0,
            "recognition UI must receive the actual lens-corrected image");
        cv::Mat inverse;
        const auto expected=engine.rectifyPerspective(CameraUndistortion::image(lensFrame),
            CameraUndistortion::points(lensProfile.perspective_pts,size),inverse,
            lensProfile.rectified_size,lensProfile.rectified_margin);
        require(!result.step2_rectified.empty() && cv::norm(result.step2_rectified,expected,cv::NORM_INF)==0,
            "recognition must undistort before applying the four-point homography");
        require(cv::norm(result.step1_original,originalPixels,cv::NORM_INF)==0 &&
            cv::norm(lensFrame,originalPixels,cv::NORM_INF)==0,"evidence and input must preserve the raw snapshot");
        require(lensProfile.toJson()==saved,"recognition must not persist corrected coordinates");
        const auto repeated=engine.process(lensFrame,lensProfile,GaugeRunMode::Debug);
        require(cv::norm(repeated.step2_rectified,expected,cv::NORM_INF)==0,"repeated recognition must not double-correct");
        CardinalSelectorDialog lensDialog(lensFrame,lensProfile,{},nullptr,true);
        const auto displayed=lensDialog.findChild<InteractiveImageLabel*>("perspectiveImage")->getPoints();
        const auto correctedCorners=CameraUndistortion::points(lensProfile.perspective_pts,size);
        require(cv::norm(displayed[0]-correctedCorners[0])<.001,"calibration UI must show corrected annotations");
        lensDialog.reject();
        require(lensProfile.toJson()==saved,"cancelling calibration must preserve raw annotations");
        CardinalSelectorDialog saveDialog(lensFrame,lensProfile,QStringLiteral("lens-test"),nullptr,true);
        saveDialog.findChild<QDoubleSpinBox*>("ocrRangeMinimum")->setValue(-.2);
        saveDialog.findChild<QDoubleSpinBox*>("ocrRangeMaximum")->setValue(2.5);
        for (auto* box : saveDialog.findChildren<QCheckBox*>()) box->setChecked(true);
        require(QMetaObject::invokeMethod(&saveDialog,"onSaveAndApply",Qt::DirectConnection),"save slot missing");
        require(saveDialog.result()==QDialog::Accepted,"historical calibration must save without a database write");
        require(lensProfile.min_val==-.2&&lensProfile.max_val==2.5&&lensProfile.spec.range_max==2.5,
            "saving edited range updates effective profile and meter specification together");
        require(cv::norm(lensProfile.center-cv::Point2f(288,162))<.001 &&
            cv::norm(lensProfile.perspective_pts[0]-cv::Point2f(288,92))<.001,
            "saving corrected annotations must return them to raw snapshot coordinates");
        require(std::abs(lensProfile.scalePoints[0].toObject()["x"].toDouble()-358)<.001 &&
            lensProfile.scalePoints[1].toObject()["ratio"].toDouble()==1,
            "scale positions must return to raw coordinates while ratios remain intact");
        require(std::abs(lensProfile.scalePoints[0].toObject()["origin_y"].toDouble()-158)<.001,
            "calibration UI save must persist the printed plane origin separately from the hub");

        GaugeProfile profile;
        cv::Mat frame(600, 600, CV_8UC3, cv::Scalar(220,220,220));
        CardinalSelectorDialog dialog(frame, profile, {}, nullptr, true);
        dialog.setAttribute(Qt::WA_DontShowOnScreen);
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

        auto* autoScale=dialog.findChild<QPushButton*>("autoCalibrateScaleButton");
        require(autoScale && autoScale->isEnabled(),"OCR action must be available after center confirmation");
        const auto priorScales=preview->getScalePoints();
        const auto priorProfile=profile.toJson();
        const auto previousScript=qgetenv("GAUGE_OCR_SCRIPT");
        qputenv("GAUGE_OCR_SCRIPT","missing-test-ocr-adapter.py");
        autoScale->click();
        require(progress->isVisible() && !autoScale->isEnabled() && !save->isEnabled() && !preview->isEnabled(),
            "OCR must show busy feedback and freeze edits");
        heartbeats=0;timeout.start(15000);loop.exec();
        if(previousScript.isNull())qunsetenv("GAUGE_OCR_SCRIPT");else qputenv("GAUGE_OCR_SCRIPT",previousScript);
        require(!progress->isVisible() && autoScale->isEnabled() && preview->isEnabled() && heartbeats>0,
            "OCR failure must restore edits and keep event loop responsive");
        require(preview->getScalePoints()==priorScales && profile.toJson()==priorProfile,
            "failed OCR must preserve existing calibration and profile");
        require(dialog.findChild<QLabel*>("calibrationSaveStatus")->text().contains(QStringLiteral(u"数字识别失败")),
            "OCR errors must be actionable in the existing calibration UI");

        qputenv("GAUGE_OCR_SCRIPT",QByteArray(OCR_FIXTURE_SCRIPT));
        autoScale->click();heartbeats=0;timeout.start(15000);loop.exec();
        if(previousScript.isNull())qunsetenv("GAUGE_OCR_SCRIPT");else qputenv("GAUGE_OCR_SCRIPT",previousScript);
        require(!progress->isVisible() && preview->ocrNumbers().size()==7,
            "numeric inspection must succeed on a blank dial without tick matching");
        require(preview->getScalePoints()==priorScales && profile.toJson()==priorProfile,
            "successful numeric inspection must not generate calibration points or mutate profile");
        require(autoScale->text().contains(QStringLiteral(u"识别表盘数字")),"OCR button must describe numeric inspection");
        require(dialog.findChild<QLabel*>("calibrationSaveStatus")->text().contains(QStringLiteral(u"识别到 7 处数字")),
            "OCR must report numbers instead of a calibration result");
        if(!qEnvironmentVariable("GAUGE_TEST_PREVIEW_PATH").isEmpty())preview->grab().save(qEnvironmentVariable("GAUGE_TEST_PREVIEW_PATH"));
        require(!dialog.findChild<QLineEdit*>("ocrAllowedValues"),"candidate values must be built in without user input");
        qputenv("GAUGE_OCR_FIXTURE_MODE","range");
        qputenv("GAUGE_OCR_SCRIPT",QByteArray(OCR_FIXTURE_SCRIPT));
        autoScale->click();timeout.start(15000);loop.exec();
        qunsetenv("GAUGE_OCR_FIXTURE_MODE");
        if(previousScript.isNull())qunsetenv("GAUGE_OCR_SCRIPT");else qputenv("GAUGE_OCR_SCRIPT",previousScript);
        auto* rangeMin=dialog.findChild<QDoubleSpinBox*>("ocrRangeMinimum");
        auto* rangeMax=dialog.findChild<QDoubleSpinBox*>("ocrRangeMaximum");
        require(rangeMin&&rangeMax&&rangeMin->value()==0&&rangeMax->value()==1.9,"OCR lower endpoints populate editable range fields");
        require(profile.toJson()==priorProfile&&preview->getScalePoints()==priorScales,"range remains pending until calibration save");
        rangeMax->setValue(2.0);require(profile.toJson()==priorProfile,"manual range correction remains pending until save");
        // Integration: actual tick pixels plus subprocess OCR labels must update
        // the preview endpoints, preserving the middle point and pending profile.
        cv::Mat tickFrame(800,800,CV_8UC3,cv::Scalar(245,245,245));
        const auto dialPoint=[](double angle,double radius){return cv::Point2f(400+radius*std::cos(angle*CV_PI/180),400+radius*std::sin(angle*CV_PI/180));};
        for(double angle:{135.,45.}) {
            const auto rawLine=CameraUndistortion::points({dialPoint(angle,261),dialPoint(angle,299)},tickFrame.size(),true);
            cv::line(tickFrame,rawLine[0],rawLine[1],{10,10,10},5);
        }
        GaugeProfile tickProfile;
        tickProfile.perspective_pts=CameraUndistortion::points({{400,40},{760,400},{400,760},{40,400}},tickFrame.size(),true);
        tickProfile.center=CameraUndistortion::point({400,400},tickFrame.size(),true);
        tickProfile.scalePoints=CameraUndistortion::scales(QJsonArray{
            QJsonObject{{"x",dialPoint(140,280).x},{"y",dialPoint(140,280).y},{"ratio",0}},
            QJsonObject{{"x",400},{"y",120},{"ratio",.5}},
            QJsonObject{{"x",dialPoint(40,280).x},{"y",dialPoint(40,280).y},{"ratio",1}}},tickFrame.size(),true);
        const auto originalTickProfile=tickProfile.toJson();
        CardinalSelectorDialog tickDialog(tickFrame,tickProfile,"endpoint-test",nullptr,true);
        tickDialog.setAttribute(Qt::WA_DontShowOnScreen);tickDialog.show();app.processEvents();
        auto* tickPreview=tickDialog.findChild<InteractiveImageLabel*>("rectifiedCalibrationImage");
        const auto oldTickPoints=tickPreview->getScalePoints();
        qputenv("GAUGE_OCR_FIXTURE_MODE","range");qputenv("GAUGE_OCR_SCRIPT",QByteArray(OCR_FIXTURE_SCRIPT));
        tickDialog.findChild<QPushButton*>("autoCalibrateScaleButton")->click();
        QEventLoop tickLoop;QTimer tickPoll,tickTimeout;
        QObject::connect(&tickPoll,&QTimer::timeout,&tickLoop,[&]{if(!tickDialog.findChild<QProgressBar*>("calibrationLoadingProgress")->isVisible())tickLoop.quit();});
        QObject::connect(&tickTimeout,&QTimer::timeout,&tickLoop,&QEventLoop::quit);
        tickPoll.start(5);tickTimeout.start(15000);tickLoop.exec();
        qunsetenv("GAUGE_OCR_FIXTURE_MODE");
        if(previousScript.isNull())qunsetenv("GAUGE_OCR_SCRIPT");else qputenv("GAUGE_OCR_SCRIPT",previousScript);
        const auto newTickPoints=tickPreview->getScalePoints();
        require(newTickPoints.size()==3&&newTickPoints[1]==oldTickPoints[1]&&newTickPoints[0]!=oldTickPoints[0]&&newTickPoints[2]!=oldTickPoints[2],
            "GUI OCR action must replace actual endpoint ticks and retain middle calibration");
        require(tickProfile.toJson()==originalTickProfile,"automatic endpoint angles remain pending until save");
        require(tickDialog.findChild<QLabel*>("calibrationSaveStatus")->text().contains(QStringLiteral(u"刻度角度已自动标定")),"GUI must report endpoint auto calibration success");
        auto* tickOriginal=tickDialog.findChild<InteractiveImageLabel*>("perspectiveImage");
        auto adjustedCorners=tickOriginal->getPoints();adjustedCorners[0].y+=4;
        tickOriginal->setImageAndPoints(CameraUndistortion::image(tickFrame),adjustedCorners);
        QMetaObject::invokeMethod(&tickDialog,"onPointsUpdated",Qt::DirectConnection);
        require(tickDialog.findChild<QLabel*>("calibrationSaveStatus")->text().contains(QStringLiteral(u"旧自动端点"))&&
            !tickDialog.findChild<QPushButton*>("saveCalibrationButton")->isEnabled(),
            "changed perspective must invalidate old auto endpoints and require rerun or manual correction");
        QMetaObject::invokeMethod(&tickDialog,"onSaveAndApply",Qt::DirectConnection);
        require(tickDialog.result()!=QDialog::Accepted&&tickProfile.toJson()==originalTickProfile,
            "save slot must reject stale automatic endpoints even if invoked directly");
        tickDialog.reject();
        // Optional local-photo integration: run the production geometry worker,
        // then the OCR protocol fixture and save/reopen all coordinate metadata.
        const auto structuralImage=qEnvironmentVariable("GAUGE_TEST_STRUCTURAL_IMAGE");
        if(!structuralImage.isEmpty()) {
            const auto photo=cv::imread(structuralImage.toStdString());require(!photo.empty(),"structural photo fixture missing");
            GaugeProfile photoProfile;CardinalSelectorDialog photoDialog(photo,photoProfile,"structural-test",nullptr,true);
            photoDialog.setAttribute(Qt::WA_DontShowOnScreen);photoDialog.show();app.processEvents();
            QEventLoop photoLoop;QTimer photoPoll,photoTimeout;
            QObject::connect(&photoPoll,&QTimer::timeout,&photoLoop,[&]{if(!photoDialog.findChild<QProgressBar*>("calibrationLoadingProgress")->isVisible())photoLoop.quit();});
            QObject::connect(&photoTimeout,&QTimer::timeout,&photoLoop,&QEventLoop::quit);
            QMetaObject::invokeMethod(&photoDialog,"onAutoDetect",Qt::DirectConnection);
            photoPoll.start(5);photoTimeout.start(20000);photoLoop.exec();
            const auto localizationStatus=photoDialog.findChild<QLabel*>("calibrationSaveStatus")->text();
            if(qEnvironmentVariableIsSet("GAUGE_TEST_MIN_TOP_Y")) {
                const auto handles=photoDialog.findChild<InteractiveImageLabel*>("perspectiveImage")->getPoints();
                require(handles.size()==4&&handles[0].y>=qEnvironmentVariable("GAUGE_TEST_MIN_TOP_Y").toDouble(),"top handle must remain on the observed inner rim, not the image border");
            }
            if(qEnvironmentVariableIsSet("GAUGE_TEST_MAX_TOP_Y")) {
                const auto handles=photoDialog.findChild<InteractiveImageLabel*>("perspectiveImage")->getPoints();
                require(handles.size()==4&&handles[0].y<=qEnvironmentVariable("GAUGE_TEST_MAX_TOP_Y").toDouble(),"an inward printed/reflection boundary must not replace the actual top rim");
            }
            if(qEnvironmentVariableIsSet("GAUGE_TEST_ALLOW_RIM_FALLBACK"))
                require(!localizationStatus.contains(QStringLiteral(u"自动定位未通过"))&&photoDialog.findChild<InteractiveImageLabel*>("rectifiedCalibrationImage")->getCenterPoint().x>=0,"dark-lip photo must successfully localize and rectify in the GUI");
            else require(localizationStatus.contains(QStringLiteral(u"刻度的平面矫正")),"real-photo GUI must apply multi-feature geometry");
            qputenv("GAUGE_OCR_FIXTURE_MODE","range");qputenv("GAUGE_OCR_SCRIPT",QByteArray(OCR_FIXTURE_SCRIPT));
            const auto photoOcr=qgetenv("GAUGE_TEST_STRUCTURAL_OCR");if(!photoOcr.isEmpty())qputenv("GAUGE_OCR_FIXTURE_TOKENS",photoOcr);
            photoDialog.findChild<QPushButton*>("autoCalibrateScaleButton")->click();photoTimeout.start(20000);photoLoop.exec();
            qunsetenv("GAUGE_OCR_FIXTURE_MODE");
            qunsetenv("GAUGE_OCR_FIXTURE_TOKENS");
            if(previousScript.isNull())qunsetenv("GAUGE_OCR_SCRIPT");else qputenv("GAUGE_OCR_SCRIPT",previousScript);
            const auto photoPoints=photoDialog.findChild<InteractiveImageLabel*>("rectifiedCalibrationImage")->getScalePoints();
            require(photoPoints.size()>=2&&photoPoints.first().toObject().contains("origin_x"),"GUI OCR endpoints must carry the printed-plane origin");
            if(!photoOcr.isEmpty())require(photoPoints.size()==4&&std::abs(photoPoints[1].toObject()["ratio"].toDouble()-.2)<1e-6&&std::abs(photoPoints[2].toObject()["ratio"].toDouble()-.8)<1e-6,"real-photo OCR must auto-calibrate both lower intermediate values");
            if(!qEnvironmentVariable("GAUGE_TEST_MAJOR_PREVIEW").isEmpty())photoDialog.findChild<InteractiveImageLabel*>("rectifiedCalibrationImage")->grab().save(qEnvironmentVariable("GAUGE_TEST_MAJOR_PREVIEW"));
            for(auto* box:photoDialog.findChildren<QCheckBox*>())box->setChecked(true);
            QMetaObject::invokeMethod(&photoDialog,"onSaveAndApply",Qt::DirectConnection);
            require(photoDialog.result()==QDialog::Accepted&&photoProfile.scalePoints.size()==photoPoints.size(),"multi-feature calibration must save");
            const auto photoReading=engine.process(photo,photoProfile,GaugeRunMode::Debug);
            require(photoReading.success&&!photoReading.centerInconsistent,"saved multi-feature calibration must remain usable by the pointer-reading pipeline");
            CardinalSelectorDialog reopened(photo,photoProfile,"structural-test",nullptr,true);
            const auto restored=reopened.findChild<InteractiveImageLabel*>("rectifiedCalibrationImage")->getScalePoints();
            require(restored.size()==photoPoints.size()&&std::abs(restored.first().toObject()["origin_x"].toDouble()-photoPoints.first().toObject()["origin_x"].toDouble())<.01,
                "reopening saved calibration must preserve the printed origin");
            reopened.reject();std::cout<<"PASS: real-photo structural geometry, OCR endpoint protocol, save and reopen\n";
        }
        preview->setImageAndPoints(frame,{},preview->getCenterPoint());
        require(preview->ocrNumbers().isEmpty(),"changing rectification must clear stale numeric boxes");

        InteractiveImageLabel scaleImage;
        scaleImage.setAttribute(Qt::WA_DontShowOnScreen);
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
        closing->setAttribute(Qt::WA_DontShowOnScreen);
        closing->show();
        const bool initialFeedback=closing->findChild<QProgressBar*>("calibrationLoadingProgress")->isVisible();
        for(auto* button:closing->findChildren<QPushButton*>())
            require(button->isVisible()==(button->objectName()=="exportCalibrationOriginalButton"),"automatic result view must expose only original-image export");
        for(auto* spin:closing->findChildren<QDoubleSpinBox*>())
            require(!spin->isVisible(),"automatic result view must expose no manual numeric fields");
        require(!closing->findChild<InteractiveImageLabel*>("rectifiedCalibrationImage")->isVisible(),
            "intermediate or stale calibration must not appear as the final result");
        require(!closing->findChild<QLabel*>("calibrationLoadingStatus")->text().isEmpty(),
            "loading message must exist before the first event loop iteration");
        app.processEvents(); // Starts the automatic detection scheduled on opening.
        closing->reject();
        const bool cancelled=!closing->isVisible();
        delete closing;
        release.release();blocker.waitForFinished();pool->waitForDone();app.processEvents();
        require(cancelled, "users must be able to cancel while detection is pending");
        require(initialFeedback, "opening a calibration must immediately show loading feedback");
        const auto automaticImage=qEnvironmentVariable("GAUGE_TEST_AUTOMATIC_IMAGE");
        if(!automaticImage.isEmpty()) {
            qputenv("GAUGE_OCR_SCRIPT",QByteArray(OCR_FIXTURE_SCRIPT));
            qputenv("GAUGE_OCR_FIXTURE_TOKENS",qgetenv("GAUGE_TEST_AUTOMATIC_TOKENS"));
            qputenv("GAUGE_OCR_FIXTURE_FULL_HEIGHT","800");
            GaugeProfile automaticProfile;const auto initial=automaticProfile.toJson();
            CardinalSelectorDialog automatic(cv::imread(automaticImage.toStdString()),automaticProfile);
            automatic.setAttribute(Qt::WA_DontShowOnScreen);automatic.show();
            QEventLoop autoLoop;QTimer autoPoll,autoTimeout;autoTimeout.setSingleShot(true);
            autoPoll.setInterval(10);
            QObject::connect(&autoPoll,&QTimer::timeout,&autoLoop,[&]{
                if(automatic.findChild<QLabel*>("calibrationSaveStatus")->text().startsWith(QStringLiteral(u"未保存本次标定")))autoLoop.quit();
            });
            QObject::connect(&autoTimeout,&QTimer::timeout,&autoLoop,&QEventLoop::quit);
            autoPoll.start();autoTimeout.start(20000);autoLoop.exec();
            const auto status=automatic.findChild<QLabel*>("calibrationSaveStatus")->text();
            std::cout<<"Automatic status: "<<status.toStdString()<<'\n';
            require(status.contains(QStringLiteral(u"无法保存")),"automatic stages must reach save validation without any button clicks");
            const auto points=automatic.findChild<InteractiveImageLabel*>("rectifiedCalibrationImage")->getScalePoints();
            require(points.size()>15,"automatic chain must include major and minor calibration points");
            require(automaticProfile.toJson()==initial,"missing device identity must not persist or mutate the original profile");
            for(auto* button:automatic.findChildren<QPushButton*>())require(button->isVisible()==(button->objectName()=="exportCalibrationOriginalButton"),"automatic completion/failure must retain only original-image export");
            automatic.reject();
            std::cout<<"PASS: opening alone drives localization, OCR, major/minor ticks and save validation\n";
        }
        std::cout << "PASS: busy feedback, event loop responsiveness, failure recovery, cancellation and safe destruction\n";
        return 0;
    } catch(const std::exception& e) {std::cerr << e.what() << '\n';return 1;}
}
