#pragma once

#include <QString>

namespace asmr {

struct WorkCategory {
    qint64 id = 0;
    QString name;
    QString managedCoverFile;
    bool operator==(const WorkCategory &) const = default;
};

struct WorkCategoryMembership {
    qint64 categoryId = 0;
    qint64 workId = 0;
    qint64 addedAtMs = 0;
    bool operator==(const WorkCategoryMembership &) const = default;
};

} // namespace asmr
