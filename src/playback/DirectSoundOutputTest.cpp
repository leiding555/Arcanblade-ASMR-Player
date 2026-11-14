#include "playback/DirectSoundOutputTest.h"

#ifdef Q_OS_WIN
#include <QScopeGuard>
#include <qt_windows.h>

#include <initguid.h>
#include <mmdeviceapi.h>
#include <dsound.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#endif

namespace asmr {

#ifdef Q_OS_WIN
namespace {
struct ReleaseCom {
    void operator()(IUnknown *ptr) const { ptr->Release(); }
};
template<class T> using ComPointer = std::unique_ptr<T, ReleaseCom>;
}
#endif

DirectSoundTestResult testDirectSoundOutput(const QByteArray &endpointId, quintptr window,
                                           const QByteArray &stereoPcm, int sampleRate,
                                           const std::atomic_bool &cancelled)
{
#ifdef Q_OS_WIN
    using namespace std::chrono_literals;
    const auto cancelledResult = DirectSoundTestResult{QStringLiteral("cancelled")};
    if (cancelled.load()) return cancelledResult;
    const auto failed = [](const char *stage, HRESULT error) {
        return DirectSoundTestResult{QString::fromLatin1(stage), quint32(error)};
    };
    if (endpointId.isEmpty() || !window || stereoPcm.isEmpty() || sampleRate <= 0)
        return failed("test_arguments", E_INVALIDARG);

    auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return failed("CoInitializeEx", hr);
    const auto uninitialize = qScopeGuard([] { CoUninitialize(); });
    IMMDeviceEnumerator *rawEnumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&rawEnumerator));
    if (FAILED(hr)) return failed("CoCreateInstance", hr);
    ComPointer<IMMDeviceEnumerator> enumerator(rawEnumerator);
    IMMDevice *rawDevice = nullptr;
    const auto id = QString::fromUtf8(endpointId).toStdWString();
    hr = enumerator->GetDevice(id.c_str(), &rawDevice);
    if (FAILED(hr)) return failed("GetDevice", hr);
    ComPointer<IMMDevice> device(rawDevice);

    IPropertyStore *rawProperties = nullptr;
    hr = device->OpenPropertyStore(STGM_READ, &rawProperties);
    if (FAILED(hr)) return failed("OpenPropertyStore", hr);
    ComPointer<IPropertyStore> properties(rawProperties);
    PROPVARIANT value{};
    const auto clear = qScopeGuard([&value] { PropVariantClear(&value); });
    hr = properties->GetValue(PKEY_AudioEndpoint_GUID, &value);
    if (FAILED(hr)) return failed("PKEY_AudioEndpoint_GUID", hr);
    if (value.vt != VT_LPWSTR || !value.pwszVal)
        return failed("PKEY_AudioEndpoint_GUID", E_UNEXPECTED);
    GUID guid{};
    hr = CLSIDFromString(value.pwszVal, &guid);
    if (FAILED(hr)) return failed("CLSIDFromString", hr);
    if (cancelled.load()) return cancelledResult;

    IDirectSound8 *rawOutput = nullptr;
    hr = DirectSoundCreate8(&guid, &rawOutput, nullptr);
    if (FAILED(hr)) return failed("DirectSoundCreate8", hr);
    ComPointer<IDirectSound8> output(rawOutput);
    hr = output->SetCooperativeLevel(reinterpret_cast<HWND>(window), DSSCL_NORMAL);
    if (FAILED(hr)) return failed("SetCooperativeLevel", hr);
    WAVEFORMATEX format{WAVE_FORMAT_PCM, 2, DWORD(sampleRate), DWORD(sampleRate * 4), 4, 16, 0};
    DSBUFFERDESC descriptor{};
    descriptor.dwSize = sizeof(descriptor);
    descriptor.dwFlags = DSBCAPS_GLOBALFOCUS;
    descriptor.dwBufferBytes = DWORD(stereoPcm.size());
    descriptor.lpwfxFormat = &format;
    IDirectSoundBuffer *rawBuffer = nullptr;
    hr = output->CreateSoundBuffer(&descriptor, &rawBuffer, nullptr);
    if (FAILED(hr)) return failed("CreateSoundBuffer", hr);
    ComPointer<IDirectSoundBuffer> buffer(rawBuffer);
    void *first = nullptr;
    void *second = nullptr;
    DWORD firstBytes = 0;
    DWORD secondBytes = 0;
    hr = buffer->Lock(0, descriptor.dwBufferBytes, &first, &firstBytes, &second, &secondBytes, 0);
    if (FAILED(hr)) return failed("Lock", hr);
    std::memcpy(first, stereoPcm.constData(), firstBytes);
    if (secondBytes) std::memcpy(second, stereoPcm.constData() + firstBytes, secondBytes);
    hr = buffer->Unlock(first, firstBytes, second, secondBytes);
    if (FAILED(hr)) return failed("Unlock", hr);
    if (cancelled.load()) return cancelledResult;
    hr = buffer->Play(0, 0, 0);
    if (FAILED(hr)) return failed("Play", hr);
    const auto stop = qScopeGuard([&buffer] { buffer->Stop(); });
    const auto duration = std::chrono::milliseconds(stereoPcm.size() * 1000 / format.nAvgBytesPerSec);
    const auto deadline = std::chrono::steady_clock::now() + duration + 250ms;
    bool observedPlaying = false;
    while (!cancelled.load() && std::chrono::steady_clock::now() < deadline) {
        DWORD status = 0;
        hr = buffer->GetStatus(&status);
        if (FAILED(hr)) return failed("GetStatus", hr);
        if (status & DSBSTATUS_BUFFERLOST) return failed("GetStatus", DSERR_BUFFERLOST);
        if (status & DSBSTATUS_PLAYING) observedPlaying = true;
        else if (observedPlaying) return {QStringLiteral("completed")};
        std::this_thread::sleep_for(10ms);
    }
    if (cancelled.load()) return cancelledResult;
    return failed("wait_for_playback", HRESULT_FROM_WIN32(WAIT_TIMEOUT));
#else
    Q_UNUSED(endpointId);
    Q_UNUSED(window);
    Q_UNUSED(stereoPcm);
    Q_UNUSED(sampleRate);
    Q_UNUSED(cancelled);
    return {QStringLiteral("unsupported_platform"), 1};
#endif
}

} // namespace asmr
