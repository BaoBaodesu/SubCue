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
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

extern "C" {
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace subcue {
namespace {

// 独立音频线程单次等待上限：设备事件丢失或设备被拔出时也要能回到循环检查退出。
constexpr DWORD kRenderWaitTimeoutMs = 200;
// 共享模式缓冲请求值（100ns 单位，20ms）：请求过大只抬高延迟，不改善连续播放。
constexpr REFERENCE_TIME kBufferDurationRequest = 200'000;
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

void storeSample(BYTE *destination, int bits, int validBits, bool ieeeFloat, float value)
{
    if (ieeeFloat && bits == 32) {
        *reinterpret_cast<float *>(destination) = value;
        return;
    }
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    if (bits == 16) {
        const auto pcm = static_cast<std::int16_t>(std::lrint(clamped * 32767.0f));
        std::memcpy(destination, &pcm, sizeof(pcm));
    } else if (bits == 24) {
        const auto pcm = static_cast<std::int32_t>(std::lrint(clamped * 8388607.0f));
        destination[0] = static_cast<BYTE>(pcm & 0xff);
        destination[1] = static_cast<BYTE>((pcm >> 8) & 0xff);
        destination[2] = static_cast<BYTE>((pcm >> 16) & 0xff);
    } else if (bits == 32) {
        const int clampedBits = std::clamp(validBits, 1, 32);
        const double maximum = clampedBits == 32
            ? 2147483647.0
            : static_cast<double>((1LL << (clampedBits - 1)) - 1);
        std::int32_t pcm = static_cast<std::int32_t>(std::llround(clamped * maximum));
        if (clampedBits < 32) pcm *= static_cast<std::int32_t>(1U << (32 - clampedBits));
        std::memcpy(destination, &pcm, sizeof(pcm));
    } else {
        std::memset(destination, 0, static_cast<size_t>(std::max(1, bits / 8)));
    }
}

// 采样率已经对齐：只做声卡格式转换与通道对齐，不做重采样，因此不存在边界硬切。
void convertFrames(
    const float *source,
    int sourceChannels,
    qint64 sourceFrames,
    BYTE *destination,
    UINT32 destinationFrames,
    int destinationChannels,
    int destinationBits,
    int destinationValidBits,
    int bytesPerFrame,
    bool ieeeFloat)
{
    for (UINT32 frame = 0; frame < destinationFrames; ++frame) {
        const float *sample = source && frame < sourceFrames && sourceChannels > 0
            ? source + static_cast<qint64>(frame) * sourceChannels
            : nullptr;
        const float left = sample ? sample[0] : 0.0f;
        const float right = sample ? (sourceChannels > 1 ? sample[1] : sample[0]) : 0.0f;
        BYTE *out = destination + static_cast<size_t>(frame) * static_cast<size_t>(bytesPerFrame);
        const int bytesPerSample = destinationBits / 8;
        for (int channel = 0; channel < destinationChannels; ++channel) {
            const float value = channel == 0 ? left : (channel == 1 ? right : 0.0f);
            storeSample(out + channel * bytesPerSample, destinationBits, destinationValidBits,
                        ieeeFloat, value);
        }
    }
}

} // namespace

struct WasapiAudioDevice::Impl {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> renderer;
    HANDLE bufferEvent = nullptr;
    WAVEFORMATEX *mixFormat = nullptr;
    UINT32 bufferFrames = 0;
    int sourceRate = 0;
    int sourceChannels = 0;
    int mixRate = 0;
    int mixChannels = 0;
    int mixBits = 16;
    int mixValidBits = 16;
    int bytesPerFrame = 0;
    qint64 underrunFrames = 0;
    qint64 fadeInFrames = 0;
    bool ieeeFloat = false;
    bool started = false;
    bool paused = false;
    bool comReady = false;
    std::atomic<bool> stopRequested{false};
    std::atomic<double> playbackRate{1.0};
    AudioSampleProvider provider;

    ~Impl()
    {
        release();
    }

    void resetStream()
    {
        if (bufferEvent) {
            CloseHandle(bufferEvent);
            bufferEvent = nullptr;
        }
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

    void release()
    {
        resetStream();
        provider = {};
        mixRate = 0;
        mixChannels = 0;
        underrunFrames = 0;
    }
};

WasapiAudioDevice::WasapiAudioDevice()
    : impl_(std::make_unique<Impl>())
{
}

WasapiAudioDevice::~WasapiAudioDevice()
{
    stop();
}

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

int WasapiAudioDevice::preferredSampleRate() const
{
    return impl_->mixRate > 0 ? impl_->mixRate : 48'000;
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

    // 事件驱动：驱动在需要填充时唤醒独立音频线程，不再依赖 UI 定时器轮询。
    impl_->bufferEvent = CreateEventExW(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (impl_->bufferEvent == nullptr) {
        if (error) {
            *error = AppError(ErrorDomain::Media, static_cast<int>(GetLastError()),
                              QStringLiteral("无法创建音频事件句柄"));
        }
        stop();
        return false;
    }

    result = impl_->client->Initialize(
        AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBufferDurationRequest, 0,
        impl_->mixFormat, nullptr);
    if (FAILED(result)) {
        if (error) {
            *error = AppError(ErrorDomain::Media, static_cast<int>(result),
                              QStringLiteral("无法初始化声卡输出"), hresultMessage(result));
        }
        stop();
        return false;
    }

    result = impl_->client->SetEventHandle(impl_->bufferEvent);
    if (FAILED(result)) {
        if (error) {
            *error = AppError(ErrorDomain::Media, static_cast<int>(result),
                              QStringLiteral("无法注册音频事件"), hresultMessage(result));
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
    impl_->mixValidBits = impl_->mixBits;
    impl_->bytesPerFrame = static_cast<int>(impl_->mixFormat->nBlockAlign);
    impl_->ieeeFloat = impl_->mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (impl_->mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE && impl_->mixFormat->cbSize >= 22) {
        const auto *extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(impl_->mixFormat);
        impl_->ieeeFloat = extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        if (extensible->Samples.wValidBitsPerSample > 0) {
            impl_->mixValidBits = static_cast<int>(extensible->Samples.wValidBitsPerSample);
        }
    }
    if ((impl_->ieeeFloat && impl_->mixBits != 32)
        || (!impl_->ieeeFloat && impl_->mixBits != 16
            && impl_->mixBits != 24 && impl_->mixBits != 32)
        || impl_->bytesPerFrame < impl_->mixChannels * (impl_->mixBits / 8)) {
        if (error) {
            *error = AppError(ErrorDomain::Media, 2,
                QStringLiteral("声卡混音格式暂不支持"),
                QStringLiteral("%1 位，%2 声道，blockAlign=%3")
                    .arg(impl_->mixBits).arg(impl_->mixChannels).arg(impl_->bytesPerFrame));
        }
        stop();
        return false;
    }

    resetResampler();
    if (impl_->mixRate != impl_->sourceRate && !resampler_) {
        qCWarning(subcuePlaybackLog) << "WASAPI mix rate differs and fallback resampler unavailable"
                                     << impl_->sourceRate << impl_->mixRate;
    }
    flushRequest_ = 0;
    appliedFlush_ = 0;

    BYTE *buffer = nullptr;
    result = impl_->renderer->GetBuffer(impl_->bufferFrames, &buffer);
    if (SUCCEEDED(result) && buffer) {
        writeSilence(buffer, impl_->bufferFrames, impl_->bytesPerFrame);
        impl_->renderer->ReleaseBuffer(impl_->bufferFrames, 0);
    }

    impl_->paused = false;
    impl_->stopRequested = false;
    thread_ = std::thread(&WasapiAudioDevice::audioLoop, this);
    if (!thread_.joinable()) {
        if (error) {
            *error = AppError(ErrorDomain::Media, 3, QStringLiteral("无法创建音频渲染线程"));
        }
        stop();
        return false;
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
    qCInfo(subcuePlaybackLog) << "WASAPI started" << impl_->mixRate << "Hz"
                              << impl_->mixChannels << "ch, buffer" << impl_->bufferFrames << "frames";
    return true;
}

void WasapiAudioDevice::stop()
{
    impl_->stopRequested = true;
    if (impl_->bufferEvent) {
        SetEvent(impl_->bufferEvent);
    }
    joinWorker();
    if (impl_->client) {
        impl_->client->Stop();
    }
    resampler_.reset();
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

void WasapiAudioDevice::setPlaybackRate(double rate)
{
    impl_->playbackRate = std::clamp(rate, 0.5, 3.0);
    // 倍率只允许在暂停时切换；清掉设备中仍按旧倍率排队的样本，先填静音再淡入。
    if (impl_->paused && impl_->client && impl_->renderer && SUCCEEDED(impl_->client->Reset())) {
        BYTE *buffer = nullptr;
        if (SUCCEEDED(impl_->renderer->GetBuffer(impl_->bufferFrames, &buffer)) && buffer) {
            writeSilence(buffer, impl_->bufferFrames, impl_->bytesPerFrame);
            impl_->renderer->ReleaseBuffer(impl_->bufferFrames, 0);
        }
    }
    ++flushRequest_;
}

void WasapiAudioDevice::setSampleProvider(AudioSampleProvider provider)
{
    impl_->provider = std::move(provider);
}

qint64 WasapiAudioDevice::renderFrame()
{
    return fillBuffer();
}

void WasapiAudioDevice::flushResampler()
{
    ++flushRequest_;
}

bool WasapiAudioDevice::isStarted() const
{
    return impl_->started;
}

bool WasapiAudioDevice::isHardware() const
{
    return true;
}

void WasapiAudioDevice::resetResampler()
{
    resampler_.reset();
    impl_->fadeInFrames = std::max(1, impl_->mixRate / 200);
    const int effectiveRate = std::max(1, static_cast<int>(
        std::lround(impl_->sourceRate * impl_->playbackRate.load())));
    if (impl_->mixRate == effectiveRate) {
        return;
    }
    AVChannelLayout inputLayout{};
    AVChannelLayout outputLayout{};
    av_channel_layout_default(&inputLayout, impl_->sourceChannels);
    av_channel_layout_default(&outputLayout, impl_->mixChannels);
    SwrContext *rawContext = nullptr;
    const int result = swr_alloc_set_opts2(
        &rawContext, &outputLayout, AV_SAMPLE_FMT_FLT, impl_->mixRate,
        &inputLayout, AV_SAMPLE_FMT_FLT, effectiveRate, 0, nullptr);
    if (result < 0 || rawContext == nullptr) {
        if (rawContext) {
            swr_free(&rawContext);
        }
        return;
    }
    AVDictionary *options = nullptr;
    // 兜底路径只求平滑：线性插值加略低截止，避免每次边界产生硬切。
    av_opt_set_int(rawContext, "linear_interp", 1, 0);
    av_opt_set_double(rawContext, "cutoff", 0.97, 0);
    av_opt_set_dict(rawContext, &options);
    av_dict_free(&options);
    if (swr_init(rawContext) >= 0) {
        resampler_.reset(rawContext);
    } else {
        swr_free(&rawContext);
    }
}

void WasapiAudioDevice::joinWorker()
{
    if (thread_.joinable()) {
        thread_.join();
    }
}

void WasapiAudioDevice::audioLoop()
{
    // WASAPI 客户端在本线程释放，避免跨线程 COM 争用导致退出卡死。
    ensureCom(nullptr);
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    while (!impl_->stopRequested.load()) {
        const DWORD waited = WaitForSingleObject(impl_->bufferEvent, kRenderWaitTimeoutMs);
        if (impl_->stopRequested.load()) {
            break;
        }
        if (waited != WAIT_OBJECT_0 || impl_->paused) {
            continue;
        }
        if (flushRequest_.load() != appliedFlush_) {
            appliedFlush_ = flushRequest_.load();
            resetResampler();
        }
        (void)fillBuffer();
    }

    if (mmcss) {
        AvRevertMmThreadCharacteristics(mmcss);
    }
    impl_->renderer.reset();
    if (impl_->client) {
        impl_->client->Stop();
    }
    impl_->client.reset();
    impl_->device.reset();
}

qint64 WasapiAudioDevice::fillBuffer()
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

    qint64 written = 0;
    if (impl_->provider) {
        written = resampler_ ? fillBufferResampled(destination, available)
                             : fillBufferDirect(destination, available);
    }
    if (written < static_cast<qint64>(available)) {
        writeSilence(destination + static_cast<size_t>(written) * static_cast<size_t>(impl_->bytesPerFrame),
                     available - static_cast<UINT32>(written), impl_->bytesPerFrame);
    }
    impl_->renderer->ReleaseBuffer(available, 0);
    if (written <= 0) {
        impl_->underrunFrames += available;
        if (impl_->underrunFrames == static_cast<qint64>(available)) {
            qCInfo(subcuePlaybackLog) << "Audio underrun: decoder has not primed the ring buffer yet";
        }
    }
    return written;
}

qint64 WasapiAudioDevice::fillBufferDirect(unsigned char *destination, unsigned int available)
{
    const int sourceRate = impl_->sourceRate > 0 ? impl_->sourceRate : impl_->mixRate;
    const qint64 sourceFrames = av_rescale(available, sourceRate, std::max(1, impl_->mixRate));
    if (sourceFrames <= 0) {
        return 0;
    }
    QVector<float> samples(static_cast<qsizetype>(sourceFrames) * impl_->sourceChannels);
    const qint64 pulled = impl_->provider(samples.data(), sourceFrames);
    const qint64 frames = std::clamp<qint64>(pulled, 0, sourceFrames);
    applyFadeIn(samples, frames, impl_->sourceChannels);
    convertFrames(samples.constData(), impl_->sourceChannels, frames, destination,
                  static_cast<UINT32>(frames), impl_->mixChannels, impl_->mixBits,
                  impl_->mixValidBits, impl_->bytesPerFrame, impl_->ieeeFloat);
    return frames;
}

qint64 WasapiAudioDevice::fillBufferResampled(unsigned char *destination, unsigned int available)
{
    // 按本次设备请求反算输入量。不能只取固定余量，否则 44.1kHz -> 48kHz 时
    // 每个周期只会生成很少的有效样本，其余部分被静音填充并形成持续爆音。
    const int effectiveRate = std::max(1, static_cast<int>(
        std::lround(impl_->sourceRate * impl_->playbackRate.load())));
    const int maximumInput = static_cast<int>(av_rescale_rnd(
        available, effectiveRate, std::max(1, impl_->mixRate), AV_ROUND_UP));
    QVector<float> samples(static_cast<qsizetype>(maximumInput) * impl_->sourceChannels);
    const qint64 pulled = impl_->provider(samples.data(), maximumInput);
    const int inputFrames = static_cast<int>(std::clamp<qint64>(pulled, 0, maximumInput));
    if (inputFrames <= 0) {
        return 0;
    }
    QVector<float> converted(static_cast<qsizetype>(available) * impl_->mixChannels);
    uint8_t *output[] = {reinterpret_cast<uint8_t *>(converted.data())};
    const uint8_t *input[] = {reinterpret_cast<const uint8_t *>(samples.constData())};
    const int produced = swr_convert(resampler_.get(), output, static_cast<int>(available),
                                     input, inputFrames);
    if (produced <= 0) {
        return 0;
    }
    applyFadeIn(converted, produced, impl_->mixChannels);
    convertFrames(converted.constData(), impl_->mixChannels, produced, destination,
                  static_cast<UINT32>(produced), impl_->mixChannels, impl_->mixBits,
                  impl_->mixValidBits, impl_->bytesPerFrame, impl_->ieeeFloat);
    return produced;
}

void WasapiAudioDevice::applyFadeIn(QVector<float> &samples, qint64 frames, int channels)
{
    if (impl_->fadeInFrames <= 0 || frames <= 0 || channels <= 0) {
        return;
    }
    const qint64 total = std::max<qint64>(1, impl_->mixRate / 200);
    const qint64 faded = std::min(frames, impl_->fadeInFrames);
    for (qint64 frame = 0; frame < faded; ++frame) {
        const float gain = static_cast<float>(total - impl_->fadeInFrames + frame + 1)
            / static_cast<float>(total);
        for (int channel = 0; channel < channels; ++channel) {
            samples[static_cast<qsizetype>(frame * channels + channel)] *= gain;
        }
    }
    impl_->fadeInFrames -= faded;
}

} // namespace subcue
