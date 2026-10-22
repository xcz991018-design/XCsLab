#include "PointerAlgorithmSettings.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <iostream>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static QJsonObject read(const QString& path) {
    QFile f(path); require(f.open(QIODevice::ReadOnly),"cannot read test config");
    return QJsonDocument::fromJson(f.readAll()).object();
}
static void write(const QString& path, const QJsonObject& value) {
    QSaveFile f(path); const auto bytes=QJsonDocument(value).toJson();
    require(f.open(QIODevice::WriteOnly) && f.write(bytes)==bytes.size() && f.commit(),"cannot write test config");
}
int main(int argc, char** argv) {
    QCoreApplication app(argc,argv);
    try {
        QTemporaryDir directory(QDir::current().filePath("build/algorithm-settings-test-XXXXXX"));
        require(directory.isValid(),"test directory unavailable");
        const auto filename=directory.filePath("parameters.json");
        qputenv("GAUGE_ALGORITHM_CONFIG_PATH",filename.toUtf8());
        QJsonObject profile{{"gauge_id","fixture"},{"unit","MPa"},{"min_val",0},{"max_val",1.6},
            {"pointer_detector_global_config",QJsonObject{{"pointerMinWidth",4.5},{"pointerMaxWidth",22}}}};
        const auto frozen=PointerAlgorithmSettings::resolve(profile);
        require(frozen["pointerMinWidth"].toDouble()==4.5 && frozen["pointerMaxWidth"].toDouble()==22,
            "migration must preserve effective legacy parameters");
        auto root=read(filename); auto profiles=root["profiles"].toObject();
        auto parameters=profiles["fixture"].toObject();
        require(!parameters.contains("min_val") && !parameters.contains("unit"),"JSON must exclude user-facing range fields");
        parameters["pointerMaxWidth"]=30; profiles["fixture"]=parameters; root["profiles"]=profiles; write(filename,root);
        profile["max_val"]=9;
        const auto updated=PointerAlgorithmSettings::resolve(profile);
        require(updated["pointerMaxWidth"].toDouble()==30 && updated["max_val"].toDouble()==9,
            "file controls hidden parameters while database controls range");
        GaugeProfile historical;
        require(historical.loadFromJson(frozen) && historical.pointerMaxWidth==22,
            "historical frozen parameters must not be overridden by current JSON");
        const auto database=PointerAlgorithmSettings::withoutAlgorithmFields(updated);
        require(!database.contains("pointerMaxWidth") && database["max_val"].toDouble()==9,
            "database range edits must not duplicate hidden parameters");
        auto copied=updated; copied["gauge_id"]="copied";
        require(PointerAlgorithmSettings::resolve(copied)["pointerMaxWidth"].toDouble()==30,
            "template copies must retain source algorithm parameters independently");
        root=read(filename); profiles=root["profiles"].toObject(); parameters=profiles["fixture"].toObject();
        parameters["pointerMinWidth"]=999; profiles["fixture"]=parameters; root["profiles"]=profiles; write(filename,root);
        bool blocked=false;
        try { PointerAlgorithmSettings::resolve(profile); } catch(const std::exception&) { blocked=true; }
        require(blocked && read(filename)==root,"invalid parameters must fail without overwriting user JSON");
        { QFile f(filename); require(f.open(QIODevice::WriteOnly),"invalid fixture unavailable"); f.write("invalid"); }
        blocked=false;
        try { PointerAlgorithmSettings::resolve(profile); } catch(const std::exception&) { blocked=true; }
        require(blocked,"malformed JSON must not silently fall back to defaults");
        require(QFile::remove(filename),"cannot remove fixture config");
        profile["_algorithm_parameters_template_id"]="fixture";
        blocked=false;
        try { PointerAlgorithmSettings::resolve(profile); } catch(const std::exception&) { blocked=true; }
        require(blocked,"lost separated parameters must not reset to defaults");
        std::cout<<"PASS: legacy migration, independent JSON, range edits, copies, historical snapshots, validation\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
