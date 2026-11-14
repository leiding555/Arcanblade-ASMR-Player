#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <functional>

namespace asmr {

// Stored ZIP entries with data descriptors, allowing large files to be sent in one pass.
class MobileOfflineZip {
public:
    using Sink = std::function<bool(QByteArrayView)>;
    explicit MobileOfflineZip(Sink sink);

    bool add(const QByteArray &name, quint64 size, const std::function<QByteArray(qint64)> &read);
    bool add(const QByteArray &name, const QByteArray &bytes);
    bool finish();

private:
    struct Entry {
        QByteArray name;
        quint64 size;
        quint64 offset;
        quint32 crc = 0;
    };
    bool send(QByteArrayView bytes);
    Sink m_sink;
    QList<Entry> m_entries;
    quint64 m_offset = 0;
};

} // namespace asmr
