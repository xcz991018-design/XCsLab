#include "MySqlErrorText.hpp"
#include <QCoreApplication>
#include <QTextStream>
#include <stdexcept>

static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        const auto unicodeSuccess=QStringLiteral(u"\u8fde\u63a5\u6210\u529f");
        require(unicodeSuccess.size()==4 && unicodeSuccess[0].unicode()==0x8fde && unicodeSuccess[1].unicode()==0x63a5,
                "MSVC Unicode literal codepoints were corrupted");
        const auto utf8 = QStringLiteral(u"无法连接数据库，账号或密码不正确。");
        require(decodeMySqlError(utf8.toUtf8(), 936) == utf8, "UTF-8 error was decoded as local text");
        require(decodeMySqlError("Access denied for user 'test'") == "Access denied for user 'test'", "ASCII error changed");
        require(decodeMySqlError({}).isEmpty(), "Empty error changed");
#ifdef Q_OS_WIN
        const auto raw = QByteArray("Can't connect to MySQL server (10061: ")
            + QByteArray::fromHex("c1acbdd3b1bbbedcbef8a1a3") + ')';
        require(decodeMySqlError(raw, 936) == QStringLiteral(u"Can't connect to MySQL server (10061: 连接被拒绝。)"),
            "GBK Winsock error was corrupted");
        const auto local = QStringLiteral(u"由于目标计算机积极拒绝，无法连接。");
        require(decodeMySqlError(local.toLocal8Bit()) == local, "Windows system encoding error corrupted");
#endif
        QTextStream(stdout) << "MySQL error encoding passed: UTF-8, ASCII, Windows ACP and mixed GBK messages\n";
        return 0;
    } catch (const std::exception& e) { QTextStream(stderr) << e.what() << "\n"; return 1; }
}
