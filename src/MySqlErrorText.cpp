#include "MySqlErrorText.hpp"
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#endif

QString decodeMySqlError(const QByteArray& bytes, unsigned int localCodePage) {
    if (bytes.isEmpty()) return {};
#ifdef Q_OS_WIN
    // Validate before decoding, so local-encoded text is never irreversibly
    // replaced with U+FFFD by QString::fromUtf8.
    const int utf8Size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        bytes.constData(), static_cast<int>(bytes.size()), nullptr, 0);
    if (utf8Size > 0) return QString::fromUtf8(bytes);
    const UINT codePage = localCodePage == 0 ? CP_ACP : localCodePage;
    const int size = MultiByteToWideChar(codePage, 0, bytes.constData(),
        static_cast<int>(bytes.size()), nullptr, 0);
    if (size > 0) {
        std::wstring text(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(codePage, 0, bytes.constData(), static_cast<int>(bytes.size()), &text[0], size);
        return QString::fromWCharArray(text.data(), size);
    }
    return QStringLiteral("无法解码原始错误消息（字节: %1）").arg(QString::fromLatin1(bytes.toHex()));
#else
    Q_UNUSED(localCodePage);
    return QString::fromUtf8(bytes);
#endif
}
