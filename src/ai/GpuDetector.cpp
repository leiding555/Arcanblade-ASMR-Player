#include "ai/GpuDetector.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>

#include <algorithm>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_2.h>
#endif

namespace asmr {
namespace {

constexpr qsizetype MaximumOutputBytes = 64 * 1024;
constexpr int QueryTimeoutMs = 5000;
constexpr qsizetype MaximumDiagnostics = 32;

void diagnostic(QStringList *messages, const QString &message)
{
    if (!messages || messages->size() >= MaximumDiagnostics)
        return;
    if (messages->size() == MaximumDiagnostics - 1)
        messages->append(QStringLiteral("更多 GPU 检测诊断已省略。"));
    else
        messages->append(message);
}

std::optional<QStringList> csvFields(const QString &line)
{
    QStringList fields;
    QString field;
    bool quoted = false;
    bool closedQuote = false;
    bool fieldStarted = false;
    for (qsizetype index = 0; index < line.size(); ++index) {
        const QChar character = line.at(index);
        if (quoted) {
            if (character == QLatin1Char('"')) {
                if (index + 1 < line.size() && line.at(index + 1) == QLatin1Char('"')) {
                    field.append(character);
                    ++index;
                } else {
                    quoted = false;
                    closedQuote = true;
                }
            } else {
                field.append(character);
            }
        } else if (character == QLatin1Char(',')) {
            fields.append(field.trimmed());
            field.clear();
            closedQuote = false;
            fieldStarted = false;
        } else if (character == QLatin1Char('"')) {
            if (fieldStarted || closedQuote || !field.trimmed().isEmpty())
                return std::nullopt;
            field.clear();
            quoted = true;
            fieldStarted = true;
        } else {
            if (closedQuote && !character.isSpace())
                return std::nullopt;
            field.append(character);
            if (!character.isSpace())
                fieldStarted = true;
        }
    }
    if (quoted)
        return std::nullopt;
    fields.append(field.trimmed());
    return fields;
}

bool unavailable(const QString &value)
{
    return value.isEmpty() || value.compare(QStringLiteral("N/A"), Qt::CaseInsensitive) == 0
        || value.compare(QStringLiteral("[N/A]"), Qt::CaseInsensitive) == 0
        || value.compare(QStringLiteral("[Not Supported]"), Qt::CaseInsensitive) == 0;
}

#ifdef Q_OS_WIN

template<typename T> struct ComReleaser {
    void operator()(T *object) const { if (object) object->Release(); }
};

QString systemDirectory()
{
    wchar_t buffer[32768] = {};
    const UINT length = GetSystemDirectoryW(buffer, static_cast<UINT>(std::size(buffer)));
    return length > 0 && length < std::size(buffer)
        ? QDir::fromNativeSeparators(QString::fromWCharArray(buffer, static_cast<qsizetype>(length)))
        : QString();
}

void enumerateDxgi(GpuDetectionResult &result, const std::atomic_bool &cancelled)
{
    const QString directory = systemDirectory();
    if (directory.isEmpty()) {
        diagnostic(&result.diagnostics, QStringLiteral("无法取得 Windows 系统目录，未枚举 DXGI 适配器。"));
        return;
    }
    const QString path = QDir::toNativeSeparators(directory + QStringLiteral("/dxgi.dll"));
    const auto unload = [](HMODULE module) { if (module) FreeLibrary(module); };
    std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(unload)> module(
        LoadLibraryExW(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32), unload);
    if (!module) {
        diagnostic(&result.diagnostics, QStringLiteral("无法加载系统 DXGI，Windows 错误码：%1。").arg(GetLastError()));
        return;
    }
    using CreateFactory = HRESULT (WINAPI *)(REFIID, void **);
    const auto createFactory = reinterpret_cast<CreateFactory>(GetProcAddress(module.get(), "CreateDXGIFactory1"));
    IDXGIFactory1 *rawFactory = nullptr;
    if (!createFactory || FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&rawFactory)))) {
        diagnostic(&result.diagnostics, QStringLiteral("无法创建 DXGI 适配器枚举器。"));
        return;
    }
    std::unique_ptr<IDXGIFactory1, ComReleaser<IDXGIFactory1>> factory(rawFactory);
    constexpr UINT MaximumAdapters = 32;
    for (UINT index = 0; index < MaximumAdapters; ++index) {
        if (cancelled.load(std::memory_order_relaxed))
            return;
        IDXGIAdapter1 *rawAdapter = nullptr;
        const HRESULT enumeration = factory->EnumAdapters1(index, &rawAdapter);
        if (enumeration == DXGI_ERROR_NOT_FOUND)
            return;
        if (FAILED(enumeration)) {
            diagnostic(&result.diagnostics, QStringLiteral("DXGI 第 %1 个适配器枚举失败。").arg(index + 1));
            return;
        }
        std::unique_ptr<IDXGIAdapter1, ComReleaser<IDXGIAdapter1>> adapter(rawAdapter);
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description))) {
            diagnostic(&result.diagnostics, QStringLiteral("DXGI 第 %1 个适配器信息不可读。").arg(index + 1));
            continue;
        }
        GpuDevice device;
        device.id = QStringLiteral("dxgi:%1:%2")
            .arg(static_cast<quint32>(description.AdapterLuid.HighPart), 8, 16, QLatin1Char('0'))
            .arg(description.AdapterLuid.LowPart, 8, 16, QLatin1Char('0'));
        device.name = QString::fromWCharArray(description.Description);
        device.dedicatedMemoryBytes = static_cast<quint64>(description.DedicatedVideoMemory);
        device.software = (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        switch (description.VendorId) {
        case 0x10de: device.vendor = GpuVendor::Nvidia; break;
        case 0x1002: device.vendor = GpuVendor::Amd; break;
        case 0x8086: device.vendor = GpuVendor::Intel; break;
        default: break;
        }
        result.devices.append(device);
    }
    diagnostic(&result.diagnostics, QStringLiteral("DXGI 适配器数量达到 32 个上限，其余设备未枚举。"));
}

QString nvidiaSmiExecutable()
{
    QStringList candidates;
    const QString system = systemDirectory();
    if (!system.isEmpty())
        candidates.append(system + QStringLiteral("/nvidia-smi.exe"));
    for (const auto &variable : {QStringLiteral("ProgramW6432"), QStringLiteral("ProgramFiles")}) {
        const QString directory = QDir::fromNativeSeparators(qEnvironmentVariable(variable.toLatin1().constData()));
        if (!directory.isEmpty() && QDir::isAbsolutePath(directory))
            candidates.append(directory + QStringLiteral("/NVIDIA Corporation/NVSMI/nvidia-smi.exe"));
    }
    candidates.removeDuplicates();
    for (const auto &candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.isAbsolute() && info.isFile() && !info.isSymLink())
            return info.absoluteFilePath();
    }
    return {};
}

void queryNvidia(GpuDetectionResult &result, const std::atomic_bool &cancelled)
{
    const QString executable = nvidiaSmiExecutable();
    if (executable.isEmpty()) {
        const bool hasNvidia = std::any_of(result.devices.cbegin(), result.devices.cend(),
            [](const GpuDevice &device) { return device.vendor == GpuVendor::Nvidia; });
        if (hasNvidia)
            diagnostic(&result.diagnostics, QStringLiteral("系统或官方 NVSMI 目录未找到 nvidia-smi；保留 DXGI 信息，驱动版本未知。"));
        return;
    }
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setProgram(executable);
    process.setArguments({QStringLiteral("--query-gpu=pci.bus_id,name,driver_version,memory.total"),
        QStringLiteral("--format=csv,noheader,nounits")});
    QElapsedTimer timer;
    timer.start();
    process.start(QIODevice::ReadOnly);
    QByteArray standardOutput;
    QByteArray standardError;
    bool outputExceeded = false;
    const auto collect = [&] {
        const QByteArray output = process.readAllStandardOutput();
        const QByteArray error = process.readAllStandardError();
        qsizetype remaining = MaximumOutputBytes - standardOutput.size() - standardError.size();
        if (output.size() + error.size() > remaining)
            outputExceeded = true;
        const qsizetype outputCount = std::min(remaining, output.size());
        standardOutput.append(output.constData(), outputCount);
        remaining -= outputCount;
        standardError.append(error.constData(), std::min(remaining, error.size()));
    };
    while (process.state() != QProcess::NotRunning) {
        if (cancelled.load(std::memory_order_relaxed)) {
            process.kill();
            process.waitForFinished(1000);
            result.cancelled = true;
            return;
        }
        if (timer.elapsed() >= QueryTimeoutMs) {
            process.kill();
            process.waitForFinished(1000);
            diagnostic(&result.diagnostics, QStringLiteral("NVIDIA 信息查询超过 5 秒，已停止；保留 DXGI 信息。"));
            return;
        }
        if (process.state() == QProcess::Starting)
            process.waitForStarted(50);
        else
            process.waitForFinished(50);
        collect();
        if (outputExceeded) {
            process.kill();
            process.waitForFinished(1000);
            diagnostic(&result.diagnostics, QStringLiteral("NVIDIA 信息查询输出超过 64 KiB，已停止；保留 DXGI 信息。"));
            return;
        }
    }
    collect();
    if (cancelled.load(std::memory_order_relaxed)) {
        result.cancelled = true;
        return;
    }
    if (outputExceeded) {
        diagnostic(&result.diagnostics, QStringLiteral("NVIDIA 信息查询输出超过 64 KiB，保留 DXGI 信息。"));
        return;
    }
    if (process.error() == QProcess::FailedToStart || process.exitStatus() != QProcess::NormalExit
            || process.exitCode() != 0) {
        diagnostic(&result.diagnostics, QStringLiteral("NVIDIA 信息查询失败（退出码 %1）：%2。保留 DXGI 信息。")
            .arg(process.exitCode()).arg(standardError.isEmpty()
                ? process.errorString() : QString::fromUtf8(standardError).left(2048).trimmed()));
        return;
    }
    QStringList parseDiagnostics;
    const auto devices = GpuDetector::parseNvidiaSmiOutput(standardOutput, &parseDiagnostics);
    for (const auto &message : parseDiagnostics)
        diagnostic(&result.diagnostics, message);
    if (devices.isEmpty() || !parseDiagnostics.isEmpty()) {
        diagnostic(&result.diagnostics, QStringLiteral("NVIDIA 查询没有返回完整有效的设备清单，保留 DXGI 信息。"));
        return;
    }
    // Replace the complete NVIDIA inventory, never join two identical cards by name.
    result.devices.removeIf([](const GpuDevice &device) { return device.vendor == GpuVendor::Nvidia; });
    result.devices.append(devices);
}

#endif // Q_OS_WIN

} // namespace

QList<GpuDevice> GpuDetector::parseNvidiaSmiOutput(const QByteArray &output, QStringList *diagnostics)
{
    QList<GpuDevice> devices;
    if (output.size() > MaximumOutputBytes) {
        diagnostic(diagnostics, QStringLiteral("NVIDIA 查询输出超过 64 KiB，拒绝解析。"));
        return devices;
    }
    QStringDecoder decoder(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    QString text = decoder(output);
    if (decoder.hasError()) {
        diagnostic(diagnostics, QStringLiteral("NVIDIA 查询输出不是有效的 UTF-8，拒绝解析。"));
        return devices;
    }
    if (!text.isEmpty() && text.front() == QChar(0xfeff))
        text.remove(0, 1);
    static const QRegularExpression busId(QStringLiteral("^([0-9a-f]{4}|[0-9a-f]{8}):([0-9a-f]{2}):([0-9a-f]{2})\\.([0-7])$"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression memory(QStringLiteral("^([0-9]+)(?:\\s+MiB)?$"),
        QRegularExpression::CaseInsensitiveOption);
    QSet<QString> seen;
    const auto lines = text.split(QLatin1Char('\n'));
    for (qsizetype line = 0; line < lines.size(); ++line) {
        if (lines.at(line).trimmed().isEmpty())
            continue;
        const auto fields = csvFields(lines.at(line).trimmed());
        if (!fields || fields->size() != 4) {
            diagnostic(diagnostics, QStringLiteral("NVIDIA 查询第 %1 行不是四列有效 CSV，已忽略。").arg(line + 1));
            continue;
        }
        const auto bus = busId.match(fields->at(0));
        if (!bus.hasMatch() || fields->at(1).isEmpty() || fields->at(1).size() > 256) {
            diagnostic(diagnostics, QStringLiteral("NVIDIA 查询第 %1 行的 PCI 标识或设备名称无效，已忽略。").arg(line + 1));
            continue;
        }
        GpuDevice device;
        device.id = QStringLiteral("pci:%1:%2:%3.%4")
            .arg(bus.captured(1).toUInt(nullptr, 16), 8, 16, QLatin1Char('0'))
            .arg(bus.captured(2).toLower(), bus.captured(3).toLower(), bus.captured(4));
        if (seen.contains(device.id)) {
            diagnostic(diagnostics, QStringLiteral("NVIDIA 查询第 %1 行重复了同一 PCI 设备，已忽略。").arg(line + 1));
            continue;
        }
        device.vendor = GpuVendor::Nvidia;
        device.name = fields->at(1);
        device.driverVersion = unavailable(fields->at(2)) ? QString() : fields->at(2);
        if (!unavailable(fields->at(3))) {
            const auto sizeMatch = memory.match(fields->at(3));
            bool converted = false;
            const quint64 mebibytes = sizeMatch.hasMatch() ? sizeMatch.captured(1).toULongLong(&converted) : 0;
            if (!converted || mebibytes > std::numeric_limits<quint64>::max() / (1024ULL * 1024)) {
                diagnostic(diagnostics, QStringLiteral("NVIDIA 查询第 %1 行的显存容量无效，已忽略。").arg(line + 1));
                continue;
            }
            device.dedicatedMemoryBytes = mebibytes * 1024ULL * 1024;
        }
        seen.insert(device.id);
        devices.append(device);
    }
    return devices;
}

GpuDetectionResult GpuDetector::detect(const std::atomic_bool &cancelled)
{
    GpuDetectionResult result;
    if (cancelled.load(std::memory_order_relaxed)) {
        result.cancelled = true;
        return result;
    }
#ifdef Q_OS_WIN
    enumerateDxgi(result, cancelled);
    if (!cancelled.load(std::memory_order_relaxed))
        queryNvidia(result, cancelled);
#else
    diagnostic(&result.diagnostics, QStringLiteral("GPU 自动检测仅支持 Windows。"));
#endif
    if (cancelled.load(std::memory_order_relaxed) || result.cancelled) {
        result.cancelled = true;
        result.devices.clear();
    }
    return result;
}

} // namespace asmr