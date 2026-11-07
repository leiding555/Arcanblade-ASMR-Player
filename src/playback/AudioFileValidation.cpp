#include "playback/AudioFileValidation.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <filesystem>
#include <system_error>

namespace asmr {
namespace {

AudioFileValidationResult failure(const QString &message)
{
    return {false, {}, message};
}

QString separators(QString path)
{
    return path.replace(QLatin1Char('\\'), QLatin1Char('/'));
}

bool unsafeComponent(const QString &component, bool allowDrive)
{
    if (component.isEmpty() || component == QStringLiteral("."))
        return false;
    if (component == QStringLiteral("..") || component.endsWith(QLatin1Char('.'))
        || component.endsWith(QLatin1Char(' '))) {
        return true;
    }
    if (allowDrive && component.size() == 2
        && ((component[0] >= QLatin1Char('A') && component[0] <= QLatin1Char('Z'))
            || (component[0] >= QLatin1Char('a') && component[0] <= QLatin1Char('z')))
        && component[1] == QLatin1Char(':')) {
        return false;
    }
    for (const QChar character : component) {
        if (character.unicode() < 32
            || QStringLiteral("<>:\"|?*").contains(character)) {
            return true;
        }
    }
    // Windows device names remain special even with an extension.
    const QString base = component.section(QLatin1Char('.'), 0, 0).trimmed().toUpper();
    if (base == QStringLiteral("CON") || base == QStringLiteral("PRN")
        || base == QStringLiteral("AUX") || base == QStringLiteral("NUL")
        || base == QStringLiteral("CONIN$") || base == QStringLiteral("CONOUT$")) {
        return true;
    }
    if (base.size() == 4
        && (base.startsWith(QStringLiteral("COM")) || base.startsWith(QStringLiteral("LPT")))) {
        const QChar suffix = base[3];
        if ((suffix >= QLatin1Char('1') && suffix <= QLatin1Char('9'))
            || suffix == QChar(0x00b9) || suffix == QChar(0x00b2) || suffix == QChar(0x00b3)) {
            return true;
        }
    }
    return false;
}

bool cleanPath(const QString &input, bool absolute, QString &cleaned)
{
    const QString path = separators(input);
    if (path.isEmpty() || path.contains(QChar::Null))
        return false;
    if (absolute) {
        if (path.startsWith(QStringLiteral("//?/")) || path.startsWith(QStringLiteral("//./"))
            || !QDir::isAbsolutePath(path)) {
            return false;
        }
#ifdef Q_OS_WIN
        const bool drive = path.size() >= 3
            && ((path[0] >= QLatin1Char('A') && path[0] <= QLatin1Char('Z'))
                || (path[0] >= QLatin1Char('a') && path[0] <= QLatin1Char('z')))
            && path[1] == QLatin1Char(':') && path[2] == QLatin1Char('/');
        const QStringList uncParts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        const bool unc = path.startsWith(QStringLiteral("//"))
            && !path.startsWith(QStringLiteral("///")) && uncParts.size() >= 2;
        if (!drive && !unc)
            return false;
#endif
    } else if (path.startsWith(QLatin1Char('/')) || QDir::isAbsolutePath(path)) {
        return false;
    }
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (qsizetype index = 0; index < parts.size(); ++index) {
        if (unsafeComponent(parts[index], absolute && index == 0 && !path.startsWith(QLatin1Char('/'))))
            return false;
    }
    cleaned = QDir::cleanPath(path);
    return absolute || (cleaned != QStringLiteral(".")
                         && !cleaned.startsWith(QStringLiteral("../")));
}

std::filesystem::path nativePath(const QString &path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(path).constData());
#endif
}

bool belongsToRoot(const std::filesystem::path &canonicalFile,
                   const std::filesystem::path &canonicalRoot)
{
    // Both paths must have resolved all reparse points first. Qt's
    // canonicalFilePath can preserve a Windows junction alias, whose lexical
    // ancestors would incorrectly lead back to the work root. MSVC canonical
    // obtains the final handle path, including intermediate junction targets.
    auto ancestor = canonicalFile.parent_path();
    while (!ancestor.empty()) {
        std::error_code error;
        const bool same = std::filesystem::equivalent(ancestor, canonicalRoot, error);
        if (!error && same)
            return true;
        const auto parent = ancestor.parent_path();
        if (parent == ancestor)
            break;
        ancestor = parent;
    }
    return false;
}

AudioFileValidationResult checkFile(const QString &path,
                                   const std::filesystem::path &canonicalRoot = {})
{
    const QFileInfo information(path);
    const QString suffix = information.suffix().toLower();
    if (suffix != QStringLiteral("mp3") && suffix != QStringLiteral("wav"))
        return failure(QStringLiteral("不支持此音频格式，仅支持 MP3 和 WAV。"));
    if (!information.exists())
        return failure(QStringLiteral("音频文件不存在或无法访问：%1").arg(path));
    if (!information.isFile())
        return failure(QStringLiteral("所选音频不是普通文件：%1").arg(path));
    std::error_code canonicalError;
    const auto canonicalFile = std::filesystem::canonical(nativePath(path), canonicalError);
    if (canonicalError || canonicalFile.empty())
        return failure(QStringLiteral("无法确认音频文件的实际路径：%1").arg(path));
    if (!canonicalRoot.empty() && !belongsToRoot(canonicalFile, canonicalRoot))
        return failure(QStringLiteral("音轨的实际文件位于作品目录之外，已拒绝打开：%1").arg(path));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return failure(QStringLiteral("无法读取音频文件：%1\n%2").arg(path, file.errorString()));
    return {true, path, {}};
}

} // namespace

AudioFileValidationResult validateAudioFile(const QString &absolutePath)
{
    QString path;
    if (!cleanPath(absolutePath, true, path))
        return failure(QStringLiteral("音频路径无效，必须为不含异常路径成分的绝对本地路径。"));
    return checkFile(path);
}

AudioFileValidationResult validateLibraryAudioFile(
    const QString &rootPath, const QString &relativePath)
{
    QString root;
    QString relative;
    if (!cleanPath(rootPath, true, root) || !cleanPath(relativePath, false, relative))
        return failure(QStringLiteral("作品根目录或音轨相对路径无效，已拒绝打开。"));
    const QFileInfo rootInformation(root);
    if (!rootInformation.exists() || !rootInformation.isDir())
        return failure(QStringLiteral("作品目录不存在或无法访问：%1").arg(root));
    std::error_code canonicalError;
    const auto canonicalRoot = std::filesystem::canonical(nativePath(root), canonicalError);
    if (canonicalError || canonicalRoot.empty())
        return failure(QStringLiteral("无法确认作品目录的实际路径：%1").arg(root));
    return checkFile(QDir::cleanPath(root + QLatin1Char('/') + relative), canonicalRoot);
}

} // namespace asmr