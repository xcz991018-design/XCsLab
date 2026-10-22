#include "EvidenceArchive.hpp"
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static void write(const QString& path, const QByteArray& data) {
    QFile f(path);
    require(f.open(QIODevice::WriteOnly) && f.write(data) == data.size(), "fixture write failed");
}
int main() {
    try {
        QTemporaryDir root;
        require(root.isValid(), "temporary directory unavailable");
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QDir folder(QDir(root.path()).filePath(id)); require(folder.mkpath("."), "fixture directory failed");
        write(folder.filePath("result.json"), QJsonDocument(QJsonObject{{"task_id",id}}).toJson());
        write(folder.filePath("original.jpg"), QByteArray(1234,'a'));
        write(folder.filePath("rectified.jpg"), QByteArray(5678,'b'));
        write(folder.filePath("unrelated.jpg"), "keep");
        EvidenceArchive::CleanupStats stats;
        if (!EvidenceArchive::expireLocalImages(root.path(),id,QDateTime::currentDateTimeUtc(),&stats)) {
            std::cerr << "Deleted " << stats.imageCount << " images / " << stats.imageBytes << " bytes; temporary path " << root.path().toStdString() << "\n";
            throw std::runtime_error("cleanup failed");
        }
        require(stats.imageCount == 2 && stats.imageBytes == 6912, "must count actual removed files and bytes");
        require(QFileInfo::exists(folder.filePath("result.json")) && QFileInfo::exists(folder.filePath("unrelated.jpg")),
                "metadata and unknown files must remain");
        require(EvidenceArchive::expireLocalImages(root.path(),id,QDateTime::currentDateTimeUtc(),&stats) &&
                stats.imageCount == 2 && stats.imageBytes == 6912, "repeated maintenance must not count twice");
        write(folder.filePath("original.jpg"), QByteArray(20,'c'));
        require(folder.mkdir("rectified.jpg"), "failure fixture unavailable");
        require(!EvidenceArchive::expireLocalImages(root.path(),id,QDateTime::currentDateTimeUtc(),&stats) &&
                stats.imageCount == 3 && stats.imageBytes == 6932, "partial failures must retain counts of actual deletions");
        std::cout << "PASS: exact deletion counts/bytes, no double counting, partial failures, preserved metadata\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
