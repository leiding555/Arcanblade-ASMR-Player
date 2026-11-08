#include "update/SemanticVersion.h"

#include <QRegularExpression>

#include <algorithm>

namespace asmr {
namespace {
bool numeric(const QString &part)
{
    return std::all_of(part.begin(), part.end(), [](QChar value) {
        return value >= QLatin1Char('0') && value <= QLatin1Char('9');
    });
}

int numericCompare(const QString &left, const QString &right)
{
    if (left.size() != right.size()) return left.size() < right.size() ? -1 : 1;
    return left.compare(right, Qt::CaseSensitive);
}
} // namespace

std::optional<SemanticVersion> SemanticVersion::parse(const QString &text, bool allowTagPrefix)
{
    const QString value = allowTagPrefix && text.startsWith(QLatin1Char('v')) ? text.mid(1) : text;
    static const QRegularExpression expression(QStringLiteral(
        R"(\A(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?\z)"));
    const auto match = expression.match(value);
    if (!match.hasMatch()) return std::nullopt;
    SemanticVersion result;
    result.m_text = value;
    for (qsizetype index = 0; index < 3; ++index)
        result.m_core[std::size_t(index)] = match.captured(index + 1);
    if (!match.captured(4).isEmpty()) {
        result.m_prerelease = match.captured(4).split(QLatin1Char('.'));
        for (const auto &part : result.m_prerelease)
            if (numeric(part) && part.size() > 1 && part.startsWith(QLatin1Char('0')))
                return std::nullopt;
    }
    return result;
}

int SemanticVersion::compare(const SemanticVersion &left, const SemanticVersion &right)
{
    for (std::size_t index = 0; index < left.m_core.size(); ++index)
        if (const int order = numericCompare(left.m_core[index], right.m_core[index]); order != 0)
            return order;
    if (left.m_prerelease.isEmpty() || right.m_prerelease.isEmpty())
        return left.m_prerelease.isEmpty() == right.m_prerelease.isEmpty() ? 0
            : left.m_prerelease.isEmpty() ? 1 : -1;
    const auto count = std::min(left.m_prerelease.size(), right.m_prerelease.size());
    for (qsizetype index = 0; index < count; ++index) {
        const auto &first = left.m_prerelease[index];
        const auto &second = right.m_prerelease[index];
        const bool firstNumeric = numeric(first);
        const bool secondNumeric = numeric(second);
        if (firstNumeric != secondNumeric) return firstNumeric ? -1 : 1;
        const int order = firstNumeric ? numericCompare(first, second)
                                       : first.compare(second, Qt::CaseSensitive);
        if (order != 0) return order;
    }
    return left.m_prerelease.size() == right.m_prerelease.size() ? 0
        : left.m_prerelease.size() < right.m_prerelease.size() ? -1 : 1;
}
} // namespace asmr
