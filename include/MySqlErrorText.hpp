#pragma once
#include <QByteArray>
#include <QString>

// Server messages normally use UTF-8; Windows client/Winsock messages may use ACP.
QString decodeMySqlError(const QByteArray& bytes, unsigned int localCodePage = 0);
