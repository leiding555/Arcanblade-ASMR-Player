#pragma once

#include <QString>

namespace asmr {

struct AudioFileValidationResult {
    bool success = false;
    QString path;
    QString errorMessage;
};

// Filesystem checks are synchronous: callers should run them off the GUI thread.
// Success means a supported, readable regular file, not that its media decodes.
// The filesystem may change after this check; AudioPlayer still handles errors.
[[nodiscard]] AudioFileValidationResult validateAudioFile(const QString &absolutePath);

// Validate the raw relative path before joining it to the root. Resolve links
// and prove the target's parent chain belongs to the root's filesystem identity;
// a case-insensitive string prefix alone is not sufficient on Windows.
[[nodiscard]] AudioFileValidationResult validateLibraryAudioFile(
    const QString &rootPath, const QString &relativePath);

} // namespace asmr