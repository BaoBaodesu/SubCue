#include "tts/voice_audio.h"
#include "tts/voice_document.h"
#include "tts/audio_quality.h"
#include "project/project_store.h"
#include "roughcut/wav_exporter.h"
#include "roughcut/rough_cut_project.h"
#include "common/file_path_guard.h"
#include <QtCore/QJsonDocument>
#include <QtCore/QCryptographicHash>
#include <QtCore/QSaveFile>
#include <QtCore/QTemporaryFile>
#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QtCore/QtEndian>
#include <cmath>
#include <cstring>
#include <limits>

namespace subcue {
namespace {
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
QByteArray header(quint32 bytes)
{
    QByteArray result("RIFF", 4);
    const auto add = [&result]<class T>(T value) { const auto converted = qToLittleEndian(value); result.append(reinterpret_cast<const char *>(&converted), sizeof(T)); };
    add(quint32(36 + bytes + bytes % 2)); result += QByteArrayLiteral("WAVEfmt "); add(quint32(16)); add(quint16(1)); add(quint16(1));
    add(quint32(48000)); add(quint32(144000)); add(quint16(3)); add(quint16(24)); result += QByteArrayLiteral("data"); add(bytes); return result;
}
}
QJsonArray VoiceAudio::plan(const QString &project, const QJsonObject &document, const QString &segment, bool allowStale, QString *error, QString *problemSegment)
{
    QJsonArray clips;
    if (!VoiceDocument::validate(document, error)) return {};
    ProjectStore store(project); QHash<QString, QString> assets;
    for (const auto &value : store.assets(error)) assets.insert(value.toObject().value(QStringLiteral("id")).toString(), store.resolveAudioPath(value.toObject().value(QStringLiteral("path")).toString()));
    if (error && !error->isEmpty()) return {};
    qint64 position = 0;
    for (const auto &value : document.value(QStringLiteral("segments")).toArray()) {
        const auto item = value.toObject(); const auto id = item.value(QStringLiteral("id")).toString(); if (!segment.isEmpty() && segment != id) continue;
        const auto take = item.value(QStringLiteral("selectedTake")).toObject();
        const auto reject = [&](const QString &message) { if (problemSegment) *problemSegment = id; fail(error, message); };
        if (take.isEmpty() || !assets.contains(take.value(QStringLiteral("id")).toString())) { reject(QStringLiteral("该句缺少选中 Take，不能静默漏句。")); return {}; }
        if (!allowStale && VoiceDocument::inputKey(document, item) != QByteArray::fromHex(take.value(QStringLiteral("inputKey")).toString().toLatin1())) { reject(QStringLiteral("选中 Take 已过期，请明确确认后导出。")); return {}; }
        const auto rate = take.value(QStringLiteral("sampleRate")).toInt();
        const auto start = qRound64(take.value(QStringLiteral("trimStart")).toString().toDouble() * 48000 / rate);
        const auto end = qRound64(take.value(QStringLiteral("trimEnd")).toString().toDouble() * 48000 / rate);
        const auto pause = segment.isEmpty() ? qRound64(item.value(QStringLiteral("pauseMs")).toDouble() * 48) : 0;
        if (end <= start || position > (std::numeric_limits<quint32>::max() - 37LL) / 3 - (end - start) - pause) { reject(QStringLiteral("编排超过标准 WAV 4GiB 限制或裁切过短。")); return {}; }
        clips.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("takeId"), take.value(QStringLiteral("id"))}, {QStringLiteral("path"), assets.value(take.value(QStringLiteral("id")).toString())},
            {QStringLiteral("sha256"), take.value(QStringLiteral("sha256"))}, {QStringLiteral("start"), QString::number(start)}, {QStringLiteral("end"), QString::number(end)},
            {QStringLiteral("position"), QString::number(position)}, {QStringLiteral("pause"), QString::number(pause)}, {QStringLiteral("gain"), item.value(QStringLiteral("gain"))},
            {QStringLiteral("sourceRate"), rate}, {QStringLiteral("sourceCount"), take.value(QStringLiteral("sampleCount"))}});
        position += end - start + pause;
    }
    if (clips.isEmpty()) fail(error, QStringLiteral("文稿没有可试听或导出的句子。")); return clips;
}
QByteArray VoiceAudio::key(const QJsonArray &clips)
{
    QJsonArray stable;
    for (const auto &value : clips) { auto item = value.toObject(); item.remove(QStringLiteral("path")); stable.append(item); }
    return QCryptographicHash::hash(QJsonDocument(stable).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
}
QJsonArray VoiceAudio::render(const QJsonArray &clips, const QString &path, bool normalize, QString *error,
    const std::atomic<bool> *cancel, const std::function<void(int, int)> &progress, bool waveformEnabled)
{
    QJsonArray rows; QStringList inputs; for (const auto &value : clips) inputs.append(value.toObject().value(QStringLiteral("path")).toString());
    if (clips.isEmpty() || !safeOutputPath(path, inputs, error)) return {};
    if (waveformEnabled && clips.size() > 1000) { fail(error, QStringLiteral("每次试听波形最多 1000 句，请使用单句试听；整轨导出不受此波形限制。")); return {}; }
    QTemporaryFile scratch(QFileInfo(path).absoluteDir().filePath(QStringLiteral(".voice-float-XXXXXX")));
    if (!scratch.open()) { fail(error, scratch.errorString()); return {}; }
    double energy = 0, peak = 0; qint64 count = 0;
    for (int i = 0; i < clips.size(); ++i) {
        const auto clip = clips[i].toObject(); const auto source = clip.value(QStringLiteral("path")).toString();
        const auto quality = AudioQuality::inspect(source, error, cancel);
        if (!quality || quality->sha256.toHex() != clip.value(QStringLiteral("sha256")).toString().toLatin1()
            || quality->sampleRate != clip.value(QStringLiteral("sourceRate")).toInt() || quality->sampleCount != clip.value(QStringLiteral("sourceCount")).toString().toLongLong()) {
            if (quality) fail(error, QStringLiteral("Take 内容或规格发生变化，请恢复资产。")); return {};
        }
        const auto length = clip.value(QStringLiteral("end")).toString().toLongLong() - clip.value(QStringLiteral("start")).toString().toLongLong();
        QVector<float> lows(256), highs(256); qint64 rendered = 0;
        const auto write = [&](QVector<float> block, qint64 offset) {
            for (qsizetype sample = 0; sample < block.size(); ++sample) {
                block[sample] *= static_cast<float>(clip.value(QStringLiteral("gain")).toDouble()); if (!std::isfinite(block[sample])) return false;
                const int column = static_cast<int>(std::min<qint64>(255, (offset + sample) * 256 / length));
                lows[column] = std::min(lows[column], block[sample]); highs[column] = std::max(highs[column], block[sample]);
                energy += double(block[sample]) * block[sample]; peak = std::max(peak, double(std::abs(block[sample])));
            }
            count += block.size(); rendered += block.size();
            const qint64 bytes = block.size() * qint64(sizeof(float));
            return scratch.write(reinterpret_cast<const char *>(block.constData()), bytes) == bytes;
        };
        if (!RoughCutWavExporter::stream(source, clip.value(QStringLiteral("start")).toString().toLongLong(), clip.value(QStringLiteral("end")).toString().toLongLong(), write, cancel, error)) return {};
        if (RoughCutProjectSerializer::mediaSha256(source, error, cancel) != quality->sha256) { fail(error, QStringLiteral("Take 在处理期间发生变化或处理已取消，未发布输出。")); return {}; }
        if (rendered < length && !write(QVector<float>(static_cast<qsizetype>(length - rendered)), rendered)) { fail(error, QStringLiteral("舍入边界写入失败。")); return {}; }
        qint64 pause = clip.value(QStringLiteral("pause")).toString().toLongLong(); const QByteArray silence(4096 * 4, '\0');
        while (pause > 0) { if (cancel && cancel->load()) { fail(error, QStringLiteral("音频处理已取消。")); return {}; } const auto size = std::min<qint64>(pause, 4096); if (scratch.write(silence.constData(), size * 4) != size * 4) { fail(error, scratch.errorString()); return {}; } count += size; pause -= size; }
        auto row = clip; row.remove(QStringLiteral("path")); QJsonArray waveform;
        bool clipped = false;
        for (int column = 0; column < 256; ++column) { if (waveformEnabled) waveform.append(QJsonArray{lows[column], highs[column]}); clipped |= lows[column] < -1 || highs[column] > 1; }
        if (waveformEnabled) row.insert(QStringLiteral("waveform"), waveform); row.insert(QStringLiteral("clipping"), clipped); rows.append(row);
        if (progress) progress(i + 1, clips.size());
    }
    // 整轨平均电平处理：明确为 RMS，不冒充 LUFS／真峰值测量；仅输出派生文件。
    const double gain = normalize && peak > 0 && count > 0 ? std::min(0.1 / std::sqrt(energy / count), std::pow(10.0, -1.0 / 20) / peak) : 1;
    QSaveFile output(path); if (!scratch.seek(0) || !output.open(QIODevice::WriteOnly) || output.write(header(static_cast<quint32>(count * 3))) != 44) { fail(error, output.errorString()); return {}; }
    while (!scratch.atEnd()) {
        if (cancel && cancel->load()) { fail(error, QStringLiteral("音频处理已取消。")); return {}; }
        const auto floats = scratch.read(4096 * 4); QByteArray pcm(floats.size() / 4 * 3, Qt::Uninitialized);
        for (qsizetype i = 0; i < floats.size() / 4; ++i) { float sample; std::memcpy(&sample, floats.constData() + i * 4, 4); const auto value = qRound64(std::clamp(double(sample) * gain, -1.0, 1.0) * 8388607); for (int byte = 0; byte < 3; ++byte) pcm[i * 3 + byte] = char((value >> (byte * 8)) & 255); }
        if (output.write(pcm) != pcm.size()) { fail(error, output.errorString()); return {}; }
    }
    if (cancel && cancel->load()) { fail(error, QStringLiteral("音频处理已取消。")); return {}; }
    if ((count * 3) % 2 && output.write("\0", 1) != 1) { fail(error, output.errorString()); return {}; }
    if (!output.commit()) { fail(error, output.errorString()); return {}; } return rows;
}
}
