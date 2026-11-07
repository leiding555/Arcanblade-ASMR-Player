#include "library/NaturalSort.h"
#include <utility>

namespace asmr {
namespace {
bool digit(QChar ch) { return ch >= QLatin1Char('0') && ch <= QLatin1Char('9'); }
int order(qsizetype a, qsizetype b) { return a < b ? -1 : a > b ? 1 : 0; }
}
NaturalSortKey::NaturalSortKey(QString name)
    : m_original(std::move(name)),
      m_folded(m_original.normalized(QString::NormalizationForm_C).toCaseFolded()
                   .normalized(QString::NormalizationForm_C)) {}
int NaturalSortKey::compare(const NaturalSortKey &other) const
{
    qsizetype left = 0, right = 0;
    while (left < m_folded.size() && right < other.m_folded.size()) {
        if (digit(m_folded[left]) && digit(other.m_folded[right])) {
            const qsizetype leftBegin = left, rightBegin = right;
            while (left < m_folded.size() && digit(m_folded[left])) ++left;
            while (right < other.m_folded.size() && digit(other.m_folded[right])) ++right;
            qsizetype a = leftBegin, b = rightBegin;
            while (a < left && m_folded[a] == QLatin1Char('0')) ++a;
            while (b < right && other.m_folded[b] == QLatin1Char('0')) ++b;
            // Compare significant digits without converting to bounded integers.
            if (const int length = order(left - a, right - b))
                return length;
            while (a < left) {
                if (const int value = order(m_folded[a].unicode(), other.m_folded[b].unicode()))
                    return value;
                ++a; ++b;
            }
            // Equal values: fewer leading zeros sort first.
            if (const int padding = order(left - leftBegin, right - rightBegin))
                return padding;
        } else {
            if (const int value = order(m_folded[left].unicode(), other.m_folded[right].unicode()))
                return value;
            ++left; ++right;
        }
    }
    if (const int remaining = order(m_folded.size() - left, other.m_folded.size() - right))
        return remaining;
    return QString::compare(m_original, other.m_original, Qt::CaseSensitive);
}
} // namespace asmr