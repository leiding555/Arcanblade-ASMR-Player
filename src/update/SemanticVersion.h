#pragma once

#include <QString>
#include <QStringList>

#include <array>
#include <optional>

namespace asmr {

// Numeric components remain decimal strings: valid SemVer components need not
// fit an integer type, including prerelease counters.
class SemanticVersion final {
public:
    [[nodiscard]] static std::optional<SemanticVersion> parse(
        const QString &text, bool allowTagPrefix = false);
    [[nodiscard]] static int compare(const SemanticVersion &left, const SemanticVersion &right);
    [[nodiscard]] QString toString() const { return m_text; }
    [[nodiscard]] bool isPrerelease() const { return !m_prerelease.isEmpty(); }

private:
    QString m_text;
    std::array<QString, 3> m_core;
    QStringList m_prerelease;
};

} // namespace asmr
