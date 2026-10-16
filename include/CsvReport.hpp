#pragma once
#include <QIODevice>
#include <QTextStream>
#include <QStringList>
#include <QList>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QStringConverter>
#endif

namespace CsvReport {
inline bool write(QIODevice& device, const QList<QStringList>& rows) {
    QTextStream out(&device);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    out.setEncoding(QStringConverter::Utf8);
#else
    out.setCodec("UTF-8");
#endif
    // Let the encoder emit actual EF BB BF bytes, never encode those bytes as text.
    out.setGenerateByteOrderMark(true);
    for (const auto& row : rows) {
        for (int column = 0; column < row.size(); ++column) {
            if (column) out << ',';
            QString field = row[column];
            if (field.contains(',') || field.contains('"') || field.contains('\r') || field.contains('\n')) {
                field.replace("\"", "\"\"");
                out << '"' << field << '"';
            } else out << field;
        }
        out << "\r\n";
    }
    out.flush();
    return out.status() == QTextStream::Ok;
}
}
