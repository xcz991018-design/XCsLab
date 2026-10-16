#include "CsvReport.hpp"
#include <QCoreApplication>
#include <QBuffer>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc,argv);
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    QFile file(directory.path() + "/report.csv");
    if (!file.open(QIODevice::WriteOnly)) return 1;
    const QList<QStringList> rows{
        {QStringLiteral("设备编号"),QStringLiteral("测点名称"),QStringLiteral("最新读数"),QStringLiteral("巡检状态")},
        {"CAM_UUID_123",QStringLiteral("压力表,\"一号\""),"0.500",QStringLiteral("异常：超出安全工作范围")},
        {"CAM_2",QStringLiteral("第一行\n第二行"),"1.250",QStringLiteral("正常")}};
    if (!CsvReport::write(file,rows)) return 1;
    file.close();
    if (!file.open(QIODevice::ReadOnly)) return 1;
    const auto bytes = file.readAll();
    if (!bytes.startsWith(QByteArray::fromHex("efbbbf"))) { std::cerr << "Missing UTF-8 BOM"; return 1; }
    if (bytes.mid(3).contains(QByteArray::fromHex("efbbbf"))) return 1;
    const auto text = QString::fromUtf8(bytes.mid(3));
    const auto expected = QStringLiteral("设备编号,测点名称,最新读数,巡检状态\r\nCAM_UUID_123,\"压力表,\"\"一号\"\"\",0.500,异常：超出安全工作范围\r\nCAM_2,\"第一行\n第二行\",1.250,正常\r\n");
    if (text != expected) { std::cerr << "CSV text, quoting or final flush incorrect"; return 1; }
    std::cout << "Excel UTF-8 BOM, Chinese roundtrip, quoting, CRLF and final-row flush passed\n";
    return 0;
}
