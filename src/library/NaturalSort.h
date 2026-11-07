#pragma once
#include <QString>
namespace asmr {
// Cached locale-independent comparison key; never a file identity.
class NaturalSortKey final {
public:
    explicit NaturalSortKey(QString name);
    [[nodiscard]] int compare(const NaturalSortKey &other) const;
private:
    QString m_original;
    QString m_folded;
};
} // namespace asmr