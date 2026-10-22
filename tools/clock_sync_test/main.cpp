#include "OnvifSoap.hpp"
#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QNetworkProxy>
#include <iostream>
#include <memory>
#include <stdexcept>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    QCoreApplication app(argc,argv);
    try {
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy,"127.0.0.1",9));
        QTcpServer camera;
        require(camera.listen(QHostAddress::LocalHost,0),"mock camera unavailable");
        QString mode="NTP"; qint64 offset=0; int writes=0; bool reject=false;
        QObject::connect(&camera,&QTcpServer::newConnection,&app,[&]{
            auto* socket=camera.nextPendingConnection();
            auto buffer=std::make_shared<QByteArray>();
            QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            QObject::connect(socket,&QTcpSocket::readyRead,socket,[&,socket,buffer]{
                buffer->append(socket->readAll());
                const int headerEnd=buffer->indexOf("\r\n\r\n");
                if (headerEnd<0) return;
                int length=0;
                for (const auto& line:buffer->left(headerEnd).split('\n'))
                    if (line.toLower().startsWith("content-length:")) length=line.mid(15).trimmed().toInt();
                if (buffer->size()<headerEnd+4+length) return;
                const auto body=buffer->mid(headerEnd+4,length);
                QByteArray response;
                if (body.contains("<tds:SetSystemDateAndTime>")) {
                    ++writes;
                    const auto requested=OnvifSoap::parseClock(body);
                    require(requested.zone=="UTC+08:00" && requested.daylight,"timezone and daylight must be preserved");
                    if (reject) response="<s:Envelope xmlns:s='http://www.w3.org/2003/05/soap-envelope'><s:Body><s:Fault/></s:Body></s:Envelope>";
                    else {
                        mode=requested.type;
                        offset=QDateTime::currentDateTimeUtc().msecsTo(requested.utc);
                        response="<SetSystemDateAndTimeResponse/>";
                    }
                } else {
                    const auto clock=QDateTime::currentDateTimeUtc().addMSecs(offset);
                    response=QString("<GetSystemDateAndTimeResponse><DateTimeType>%1</DateTimeType><DaylightSavings>true</DaylightSavings><TimeZone><TZ>UTC+08:00</TZ></TimeZone><UTCDateTime><Time><Hour>%2</Hour><Minute>%3</Minute><Second>%4</Second></Time><Date><Year>%5</Year><Month>%6</Month><Day>%7</Day></Date></UTCDateTime></GetSystemDateAndTimeResponse>")
                        .arg(mode).arg(clock.time().hour()).arg(clock.time().minute()).arg(clock.time().second())
                        .arg(clock.date().year()).arg(clock.date().month()).arg(clock.date().day()).toUtf8();
                }
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/soap+xml\r\nContent-Length: "+
                    QByteArray::number(response.size())+"\r\nConnection: close\r\n\r\n"+response);
                socket->disconnectFromHost(); buffer->clear();
            });
        });
        QUrl endpoint(QString("http://127.0.0.1:%1/onvif/device_service").arg(camera.serverPort()));
        auto result=OnvifSoap::synchronizeClockDetailed(endpoint,"admin","fixture");
        require(result.error.isEmpty() && !result.correctionAttempted && result.beforeKnown &&
            !result.afterKnown && result.logMessage("fixture").isEmpty() && writes==0 && mode=="NTP",
            "accurate NTP must not be changed or logged");
        offset=-4LL*86400*1000;
        result=OnvifSoap::synchronizeClockDetailed(endpoint,"admin","fixture");
        require(result.error.isEmpty() && result.correctionAttempted && result.beforeKnown && result.afterKnown &&
            result.beforeOffsetMs < -3LL*86400*1000 && std::llabs(result.afterOffsetMs)<=5000 &&
            writes==1 && mode=="Manual" && std::llabs(offset)<5000,
            "inaccurate NTP must report measured offsets before and after correction");
        const auto message=result.logMessage("fixture");
        require(message.contains(QString::number(result.beforeOffsetMs)) &&
            message.contains(QString::number(result.afterOffsetMs)) && message.contains("fixture"),
            "correction log must include camera and both measured offsets");
        offset=120000;
        result=OnvifSoap::synchronizeClockDetailed(endpoint,"admin","fixture");
        require(result.error.isEmpty() && result.correctionAttempted && result.beforeOffsetMs>110000 &&
            result.afterKnown && std::llabs(result.afterOffsetMs)<=5000 && writes==2,
            "manual clock drift must also be corrected and reported");
        mode="NTP"; offset=3600000; reject=true;
        result=OnvifSoap::synchronizeClockDetailed(endpoint,"admin","fixture");
        require(!result.error.isEmpty() && result.correctionAttempted && result.afterKnown &&
            result.afterOffsetMs>3500000 && result.logMessage("fixture").contains(result.error) && writes==3 && mode=="NTP",
            "failed write must preserve failure reason and uncorrected readback");
        offset=0; reject=false;
        result=OnvifSoap::synchronizeClockDetailed(endpoint,"admin","fixture");
        require(result.logMessage("fixture").isEmpty() && writes==3,
            "subsequent normal checks must stay silent");
        std::cout<<"PASS: automatic NTP/manual correction, accurate clock unchanged, timezone preserved, readback, failure reporting\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
