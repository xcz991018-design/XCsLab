#pragma once
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>
#include <QFile>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <atomic>
#include <memory>

// A single dedicated thread owns the process, including during concurrent calls.
// Results use unique per-request files and atomic publication by the Python worker.
class OcrWorker {
    QThread thread;
    QObject context;
    QProcess* process{nullptr};
    QString executable, scriptPath;
    void stop() {
        if (!process) return;
        process->kill(); process->waitForFinished(3000);
        delete process; process=nullptr;
    }
public:
    OcrWorker() { context.moveToThread(&thread); thread.start(); }
    ~OcrWorker() {
        auto* owner=QThread::currentThread();
        QMetaObject::invokeMethod(&context,[&]{stop();context.moveToThread(owner);},Qt::BlockingQueuedConnection);
        thread.quit();thread.wait();
    }
    static OcrWorker& instance() { static OcrWorker worker;return worker; }
    QString run(const QString& python,const QString& script,const QProcessEnvironment& environment,
                const QString& input,const QString& output,const std::shared_ptr<std::atomic_bool>& cancelled) {
        QString error;
        QMetaObject::invokeMethod(&context,[&]{
            if(cancelled && cancelled->load()) {error=QStringLiteral(u"OCR 已取消。");return;}
            if(process && (executable!=python || scriptPath!=script || process->processEnvironment()!=environment || process->state()==QProcess::NotRunning))stop();
            if(!process) {
                process=new QProcess;
                executable=python;scriptPath=script;
                process->setProcessEnvironment(environment);
                process->setStandardOutputFile(QProcess::nullDevice());
                process->start(python,{"-B",script,"--worker"});
                if(!process->waitForStarted(5000)){error=QStringLiteral(u"无法启动 OCR 常驻进程。");stop();return;}
            }
            const QByteArray request=QJsonDocument(QJsonObject{{"image",input},{"output",output}}).toJson(QJsonDocument::Compact)+'\n';
            if(process->write(request)!=request.size()){error=QStringLiteral(u"无法提交 OCR 请求。");stop();return;}
            QElapsedTimer timer;timer.start();QByteArray errors;
            while(!QFile::exists(output)) {
                process->waitForFinished(50);
                errors+=process->readAllStandardError();errors=errors.right(3000);
                if((cancelled && cancelled->load()) || timer.elapsed()>180000) {
                    error=cancelled && cancelled->load()?QStringLiteral(u"OCR 已取消。"):QStringLiteral(u"OCR 超时（180秒）。");stop();return;
                }
                if(process->state()==QProcess::NotRunning && !QFile::exists(output)) {
                    error=QStringLiteral(u"OCR 进程退出：%1").arg(QString::fromUtf8(errors));stop();return;
                }
            }
            process->readAllStandardError();
        },Qt::BlockingQueuedConnection);
        return error;
    }
};
