#include "mobile/MobileOfflineZip.h"

#include <QtEndian>
#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace asmr {
namespace {
constexpr quint64 ZipLimit = 0xffffffff;
constexpr quint16 Descriptor = 8;

template<class T> void append(QByteArray &bytes, T value)
{
    const auto little = qToLittleEndian(value);
    bytes.append(reinterpret_cast<const char *>(&little), sizeof(little));
}

void entrySizes(QByteArray &bytes, quint64 size)
{
    const auto field = static_cast<quint32>(std::min(size, ZipLimit));
    append(bytes, field);
    append(bytes, field);
}

QByteArray zip64Extra(quint64 size, quint64 offset, bool includeOffset)
{
    const bool includeSize = size >= ZipLimit;
    if (!includeSize && !includeOffset) return {};
    QByteArray extra;
    append<quint16>(extra, 1);
    append<quint16>(extra, (includeSize ? 16 : 0) + (includeOffset ? 8 : 0));
    if (includeSize) {
        append(extra, size);
        append(extra, size);
    }
    if (includeOffset) append(extra, offset);
    return extra;
}

quint32 updateCrc(quint32 crc, QByteArrayView bytes)
{
    static constexpr auto table = [] {
        std::array<quint32, 256> values{};
        for (quint32 i = 0; i < values.size(); ++i) {
            quint32 value = i;
            for (int bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ ((value & 1) ? 0xedb88320U : 0U);
            values[i] = value;
        }
        return values;
    }();
    for (unsigned char byte : bytes) crc = (crc >> 8) ^ table[(crc ^ byte) & 255];
    return crc;
}
} // namespace

MobileOfflineZip::MobileOfflineZip(Sink sink) : m_sink(std::move(sink)) {}

bool MobileOfflineZip::send(QByteArrayView bytes)
{
    if (bytes.isEmpty()) return true;
    if (!m_sink(bytes)) return false;
    m_offset += static_cast<quint64>(bytes.size());
    return true;
}

bool MobileOfflineZip::add(const QByteArray &name, quint64 size, const std::function<QByteArray(qint64)> &read)
{
    if (name.isEmpty() || name.size() > std::numeric_limits<quint16>::max()) return false;
    Entry entry{name, size, m_offset};
    const bool large = size >= ZipLimit;
    const auto extra = zip64Extra(size, 0, false);
    QByteArray header;
    append<quint32>(header, 0x04034b50);
    append<quint16>(header, large ? 45 : 20);
    append(header, Descriptor);
    append<quint16>(header, 0); // Stored.
    append<quint32>(header, 0x00210000); // 1980-01-01 00:00, DOS date/time.
    append<quint32>(header, 0);
    entrySizes(header, large ? size : 0);
    append<quint16>(header, static_cast<quint16>(name.size()));
    append<quint16>(header, static_cast<quint16>(extra.size()));
    if (!send(header + name + extra)) return false;

    quint32 crc = 0xffffffff;
    for (quint64 remaining = size; remaining;) {
        const auto count = static_cast<qint64>(std::min<quint64>(remaining, 64 * 1024));
        const auto bytes = read(count);
        if (bytes.isEmpty() || bytes.size() > count || !send(bytes)) return false;
        crc = updateCrc(crc, bytes);
        remaining -= static_cast<quint64>(bytes.size());
    }
    entry.crc = crc ^ 0xffffffff;
    QByteArray footer;
    append<quint32>(footer, 0x08074b50);
    append(footer, entry.crc);
    if (large) {
        append(footer, size);
        append(footer, size);
    } else {
        entrySizes(footer, size);
    }
    if (!send(footer)) return false;
    m_entries.append(std::move(entry));
    return true;
}

bool MobileOfflineZip::add(const QByteArray &name, const QByteArray &bytes)
{
    qsizetype offset = 0;
    return add(name, static_cast<quint64>(bytes.size()), [&](qint64 count) {
        const auto chunk = bytes.mid(offset, count);
        offset += chunk.size();
        return chunk;
    });
}

bool MobileOfflineZip::finish()
{
    const auto directory = m_offset;
    for (const auto &entry : m_entries) {
        const auto extra = zip64Extra(entry.size, entry.offset, entry.offset >= ZipLimit);
        const quint16 version = extra.isEmpty() ? 20 : 45;
        QByteArray header;
        append<quint32>(header, 0x02014b50);
        append(header, version);
        append(header, version);
        append(header, Descriptor);
        append<quint16>(header, 0);
        append<quint32>(header, 0x00210000);
        append(header, entry.crc);
        entrySizes(header, entry.size);
        append<quint16>(header, static_cast<quint16>(entry.name.size()));
        append<quint16>(header, static_cast<quint16>(extra.size()));
        header.append(10, '\0'); // Comment length, disk and attributes.
        append<quint32>(header, static_cast<quint32>(std::min(entry.offset, ZipLimit)));
        if (!send(header + entry.name + extra)) return false;
    }
    const auto size = m_offset - directory;
    const auto count = static_cast<quint64>(m_entries.size());
    const bool large = directory >= ZipLimit || size >= ZipLimit || count >= 0xffff;
    QByteArray footer;
    if (large) {
        append<quint32>(footer, 0x06064b50);
        append<quint64>(footer, 44);
        append<quint16>(footer, 45);
        append<quint16>(footer, 45);
        append<quint64>(footer, 0);
        append(footer, count);
        append(footer, count);
        append(footer, size);
        append(footer, directory);
        append<quint32>(footer, 0x07064b50);
        append<quint32>(footer, 0);
        append(footer, m_offset);
        append<quint32>(footer, 1);
    }
    append<quint32>(footer, 0x06054b50);
    append<quint32>(footer, 0);
    append<quint16>(footer, static_cast<quint16>(std::min<quint64>(count, 0xffff)));
    append<quint16>(footer, static_cast<quint16>(std::min<quint64>(count, 0xffff)));
    append<quint32>(footer, static_cast<quint32>(std::min(size, ZipLimit)));
    append<quint32>(footer, static_cast<quint32>(std::min(directory, ZipLimit)));
    append<quint16>(footer, 0);
    return send(footer);
}

} // namespace asmr
