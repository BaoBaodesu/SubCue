#include "platform/windows/wasapi_audio_device.h"

#include "common/logging.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <initguid.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

extern "C" {
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <cstring>

namespace subcue {
namespace {

struct ComReleaser {
    template<typename T>
    void operator()(T *value) const noexcept
    {
        if (value) {
            value->Release();
        }
    }
};

template<typename T>
using ComPtr = std::unique_ptr<T, ComReleaser>;

QString hresultMessage(HRESULT result)
{
    return QStringLiteral("HRESULT 0x%1").arg(static_cast<quint32>(result), 8, 16, QChar(u'0'));
}

bool ensureCom(AppError *error)
{
    const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(result) || result == S_FALSE || result == RPC_E_CHANGED_MODE) {
        return true;
    }
    if (error) {
        *error = AppError(ErrorDomain::Media, static_cast<int>(result),
                          QStringLiteral("无法初始化音频 COM"), hresultMessage(result));
    }
    return false;
}

ComPtr<IMMDevice> defaultRenderDevice(AppError *error)
{
    IMMDeviceEnumerator *enumeratorRaw = nullptr;
    HRESULT result = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void **>(&enumeratorRaw));
    if (FAILED(result) || enumeratorRaw == nullptr) {
        if (error) {
            *error = AppError(ErrorDomain::Media, static_cast<int>(result),
                              QStringLiteral("无法创建音频设备枚举器"), hresultMessage(result));
        }
        return {};
    }
    ComPtr<IMMDeviceEnumerator> enumerator(enumeratorRaw);

    IMMDevice *deviceRaw = nullptr;
    result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &deviceRaw);
    if (FAILED(result) || deviceRaw == nullptr) {
        if (error) {
            *error = AppError(ErrorDomain::Media, static_cast<int>(result),
                              QStringLiteral("没有可用的播放设备"), hresultMessage(result));
        }
        return {};
    }
    return ComPtr<IMMDevice>(deviceRaw);
}

void writeSilence(BYTE *destination, UINT32 frames, int bytesPerFrame)
{
    if (destination && frames > 0 && bytesPerFrame > 0) {
        std::memset(destination, 0, static_cast<size_t>(frames) * static_cast<size_t>(bytesPerFrame));
    }
}

void convertFrames(
    const float *source,
    int sourceChannels,
    qint64 sourceFrames,
    BYTE *destination,
    UINT32 destinationFrames,
    int destinationChannels,
    int destinationBits,
    bool ieeeFloat)
{
    for (UINT32 frame = 0; frame < destinationFrames; ++frame) {
        const qint64 sourceIndex = sourceFrames <= 0
            ? -1
            : av_rescale(frame, sourceFrames, destinationFrames);
        float left = 0.0f;
        float right = 0.0f;
        if (sourceIndex >= 0 && sourceIndex < sourceFrames && source != nullptr) {
            const float *sample = source + sourceIndex * sourceChannels;
            left = sample[0];
            right = sourceChannels > 1 ? sample[1] : sample[0];
        }
        BYTE *out = destination + static_cast<size_t>(frame) * static_cast<size_t>(destinationChannels)
            * static_cast<size_t>(destinationBits / 8);
        if (ieeeFloat && destinationBits == 32) {
            auto *floats = reinterpret_cast<float *>(out);
            if (destinationChannels >= 1) {
                floats[0] = left;
            }
            if (destinationChannels >= 2) {
                floats[1] = right;
            }
            for (int channel = 2; channel < destinationChannels; ++channel) {
                floats[channel] = 0.0f;
            }
        } else {
            auto *pcm = reinterpret_cast<qint16 *>(out);
            const auto toInt16 = [](float value) -> qint16 {
                const int scaled = static_cast<int>(value * 32767.0f);
                return static_cast<qint16>(std::clamp(scaled, -32768, 32767));
            };
            if (destinationChannels >= 1) {
                pcm[0] = toInt16(left);
            }
            if (destinationChannels >= 2) {
                pcm[1] = toInt16(right);
            }
            for (int channel = 2; channel < destinationChannels; ++channel) {
                pcm[channel] = 0;
            }
        }
    }
}

} // namespace

struct WasapiAudioDevice::Impl {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> renderer;
    WAVEFORMATEX *mixFormat = nullptr;
    UINT32 bufferFrames = 0;
    int sourceRate = 0;
    int sourceChannels = 0;
    int mixRate = 0;
    int mixChannels = 0;
    int mixBits = 16;
    int bytesPerFrame = 0;
    bool ieeeFloat = false;
    bool started = false;
    bool paused = false;
    bool comReady = false;

    ~Impl()
    {
        release();
    }

    void release()
    {
        renderer.reset();
        if (client) {
            client->Stop();
        }
        client.reset();
        device.reset();
        if (mixFormat) {
            CoTaskMemFree(mixFormat);
            mixFormat = nullptr;
        }
        bufferFrames = 0;
        started = false;
        paused = false;
    }
};

WasapiAudioDevice::WasapiAudioDevice()
    : impl_(std::make_unique<Impl>())
{
}

WasapiAudioDevice::~WasapiAudioDevice() = default;

QString WasapiAudioDevice::backendId() const
{
    return QStringLiteral("wasapi");
}

bool WasapiAudioDevice::probe(AppError *error)
{
    if (!ensureCom(error)) {
        return false;
    }
    impl_->comReady = true;
    auto device = defaultRenderDevice(error);
    return static_cast<bool>(device);
}

bool WasapiAudioDevice::start(int sampleRate, int channels, AppError *error)
{
    stop();
    if (sampleRate <= 0 || channels <= 0) {
        if (error) {
            *error = AppError(ErrorDomain::Media, 1, QStringLiteral("WASAPI 输出参数无效"));
        }
        return false;
    }
    if (!ensureCom(error)) {
        return false;
    }
    impl_->comReady = true;
    impl_->device = defaultRenderDevice(error);
    if (!impl_->device) {
        return false;
    }

    IAudioClient *clientRaw = nullptr;
    HRESULT result = impl_->device->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&clientRaw));
    if (FAILED(result) || clientRaw == nullptr) {
        if (error) {
            *error = AppError(ErrorDomain::Media, static_cast<int>(result),
                              QStringLiteral("无法激活音频客户端"), hresultMessage(result));
        }
        stop();
        return false;
    }
    impl_->client.reset(clientRaw);

    result = impl_->client->GetMixFormat(&impl_->mixFormat);
    if (FAILED(result) || impl_->mixFormat == nullptr) {
        if (error) {
            *error = AppError(ErrorDomain::Media, static_cast<int>(result),
                              QStringLiteral("无法读取音频混音格式"), hresultMessage(result));
        }
        stop();
        return false;
    }

    result = impl_->client->Initialize(
        AUDCLNT_SHAREMODE_SHARED, 0, 1'000'000, 0, impl_->mixFormat, nullptr);
    if (FAILED(result)) {
        if (error) {
            *error = AppError(ErrorDomain::Media, static_cast<int>(result),
                              QStringLiteral("无法初始化声卡输出"), hresultMessage(result));
        }
        stop();
        return false;
    }

    result = impl_->client->GetBufferSize(&impl_->bufferFrames);
    if (FAILED(result) || impl_->bufferFrames == 0) {
        if (error) {
            *error = AppError(ErrorDomain::Media, static_cast<int>(result),
                              QStringLiteral("无法读取音频缓冲区"), hresultMessage(result));
        }
        stop();
        return false;
    }

    IAudioRenderClient *rendererRaw = nullptr;
    result = impl_->client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void **>(&rendererRaw));
    if (FAILED(result) || rendererRaw == nullptr) {
        if (error) {
            *error = AppError(ErrorDomain::Media, static_cast<int>(result),
                              QStringLiteral("无法获取音频渲染服务"), hresultMessage(result));
        }
        stop();
        return false;
    }
    impl_->renderer.reset(rendererRaw);

    impl_->sourceRate = sampleRate;
    impl_->sourceChannels = channels;
    impl_->mixRate = static_cast<int>(impl_->mixFormat->nSamplesPerSec);
    impl_->mixChannels = static_cast<int>(impl_->mixFormat->nChannels);
    impl_->mixBits = static_cast<int>(impl_->mixFormat->wBitsPerSample);
    impl_->bytesPerFrame = impl_->mixChannels * (impl_->mixBits / 8);
    impl_->ieeeFloat = impl_->mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (impl_->mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE && impl_->mixFormat->cbSize >= 22) {
        const auto *extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(impl_->mixFormat);
        impl_->ieeeFloat = extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }

    BYTE *buffer = nullptr;
    result = impl_->renderer->GetBuffer(impl_->bufferFrames, &buffer);
    if (SUCCEEDED(result) && buffer) {
        writeSilence(buffer, impl_->bufferFrames, impl_->bytesPerFrame);
        impl_->renderer->ReleaseBuffer(impl_->bufferFrames, 0);
    }

    result = impl_->client->Start();
    if (FAILED(result)) {
        if (error) {
            *error = AppError(ErrorDomain::Media, static_cast<int>(result),
                              QStringLiteral("无法启动声卡输出"), hresultMessage(result));
        }
        stop();
        return false;
    }

    impl_->started = true;
    impl_->paused = false;
    qCInfo(subcuePlaybackLog) << "WASAPI started" << impl_->mixRate << "Hz"
                              << impl_->mixChannels << "ch";
    return true;
}

void WasapiAudioDevice::stop()
{
    impl_->release();
}

void WasapiAudioDevice::pause()
{
    impl_->paused = true;
    if (impl_->client) {
        impl_->client->Stop();
    }
}

void WasapiAudioDevice::resume()
{
    if (!impl_->started || !impl_->client) {
        return;
    }
    impl_->paused = false;
    impl_->client->Start();
}

qint64 WasapiAudioDevice::render(AudioOutput &output)
{
    if (!impl_->started || !impl_->client || !impl_->renderer || impl_->paused) {
        return 0;
    }

    UINT32 padding = 0;
    if (FAILED(impl_->client->GetCurrentPadding(&padding))) {
        return 0;
    }
    const UINT32 available = impl_->bufferFrames > padding ? impl_->bufferFrames - padding : 0;
    if (available == 0) {
        return 0;
    }

    BYTE *destination = nullptr;
    if (FAILED(impl_->renderer->GetBuffer(available, &destination)) || destination == nullptr) {
        return 0;
    }

    const qint64 sourceFrames = av_rescale(available, impl_->sourceRate, std::max(1, impl_->mixRate));
    const QVector<float> samples = output.takeFrames(sourceFrames);
    const qint64 takenFrames = impl_->sourceChannels > 0
        ? static_cast<qint64>(samples.size()) / impl_->sourceChannels
        : 0;
    const UINT32 renderedFrames = static_cast<UINT32>(std::min<qint64>(
        available,
        av_rescale_rnd(takenFrames, impl_->mixRate, std::max(1, impl_->sourceRate), AV_ROUND_DOWN)));
    writeSilence(destination, available, impl_->bytesPerFrame);
    convertFrames(
        samples.constData(),
        impl_->sourceChannels,
        takenFrames,
        destination,
        renderedFrames,
        impl_->mixChannels,
        impl_->mixBits,
        impl_->ieeeFloat);
    impl_->renderer->ReleaseBuffer(available, 0);
    return takenFrames;
}

bool WasapiAudioDevice::isStarted() const
{
    return impl_->started;
}

bool WasapiAudioDevice::isHardware() const
{
    return true;
}

} // namespace subcue
