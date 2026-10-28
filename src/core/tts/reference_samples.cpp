#include "tts/reference_samples.h"
#include "tts/audio_quality.h"
#include "asr/audio_chunk_extractor.h"
#include "roughcut/wav_exporter.h"
#include "roughcut/rough_cut_project.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QUuid>
#include <QtCore/QtEndian>
#include <cmath>
#include <cstring>
#include <numbers>

namespace subcue {
bool ReferenceSamples::derive(const QString &source, const QString &target, double startSeconds, double endSeconds,
    bool removeDc, bool reduceNoise, QString *error, const std::atomic<bool> *cancel)
{
    const auto fail = [error](const QString &message) { if (error) *error = message; return false; };
    if (!std::isfinite(startSeconds) || !std::isfinite(endSeconds) || startSeconds < 0
        || endSeconds <= startSeconds || endSeconds - startSeconds > 30 || source == target)
        return fail(QStringLiteral("裁切范围必须为 0～30 秒内的有效区间，输出不能覆盖原件。"));
    RoughCutTimelineClip clip;
    clip.sourceStartSample = qRound64(startSeconds * 24000); clip.sourceEndSample = qRound64(endSeconds * 24000);
    if (!RoughCutWavExporter::save(target, source, {clip}, 24000, 1, cancel, error)) return false;
    if (!removeDc && !reduceNoise) return true;
    QFile input(target); QSaveFile output(target);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) return fail(QStringLiteral("无法打开派生样本，请检查磁盘和权限。"));
    const auto header = input.read(44);
    if (header.size() != 44 || output.write(header) != 44) return fail(QStringLiteral("派生音频头写入失败。"));
    double previous = 0, filtered = 0, gain = 1;
    const double pole = std::exp(-2 * std::numbers::pi * 20 / 24000);
    while (!input.atEnd()) {
        if (cancel && cancel->load()) return fail(QStringLiteral("样本处理已取消。"));
        auto block = input.read(64 * 1024);
        if (block.isEmpty() || block.size() % 4) return fail(QStringLiteral("派生音频读取不完整。"));
        for (qsizetype i = 0; i < block.size(); i += 4) {
            quint32 bits = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(block.constData() + i));
            float value; std::memcpy(&value, &bits, 4);
            if (!std::isfinite(value)) return fail(QStringLiteral("样本含非有限值。"));
            if (removeDc) { filtered = value - previous + pole * filtered; previous = value; value = static_cast<float>(filtered); }
            // 保守软门：只衰减低电平区，最大 6dB；它不宣称能够去除所有背景噪声。
            if (reduceNoise) { const double targetGain = std::abs(value) < 0.003 ? 0.5 : 1; gain += (targetGain - gain) * 0.005; value *= static_cast<float>(gain); }
            std::memcpy(&bits, &value, 4); qToLittleEndian(bits, reinterpret_cast<uchar *>(block.data() + i));
        }
        if (output.write(block) != block.size()) return fail(QStringLiteral("派生音频写入失败，请检查剩余空间。"));
    }
    input.close();
    if (cancel && cancel->load()) return fail(QStringLiteral("样本处理已取消。"));
    return output.commit() || fail(output.errorString());
}

QJsonArray ReferenceSamples::prepare(const QStringList &paths, const QString &directory, QString *error,
    const std::atomic<bool> *cancel, const std::function<void(int, int)> &progress)
{
    return prepare(paths, directory, error, cancel, progress, true);
}
QJsonArray ReferenceSamples::prepare(const QStringList &paths, const QString &directory, QString *error,
    const std::atomic<bool> *cancel, const std::function<void(int, int)> &progress, bool split)
{
    QJsonArray result; QSet<QByteArray> sources, content; qint64 temporaryBytes = 0;
    if (paths.isEmpty() || paths.size() > 100 || !QDir().mkpath(directory)) {
        if (error) *error = QStringLiteral("每批请选择 1～100 个同一说话人文件，并检查临时目录权限。"); return {};
    }
    for (int file = 0; file < paths.size(); ++file) {
        if (cancel && cancel->load()) { if (error) *error = QStringLiteral("样本整理已取消。"); return {}; }
        QString problem; const auto quality = AudioQuality::inspect(paths[file], &problem, cancel);
        if (!quality || quality->peak == 0) {
            result.append(QJsonObject{{QStringLiteral("sourcePath"), paths[file]}, {QStringLiteral("error"), problem.isEmpty() ? QStringLiteral("样本全为静音。") : problem}});
            continue;
        }
        if (sources.contains(quality->sha256)) continue;
        sources.insert(quality->sha256);
        const double duration = static_cast<double>(quality->sampleCount) / quality->sampleRate;
        if (!split && duration > 30) {
            if (error) *error = QStringLiteral("单段参考请使用不超过 30 秒的片段，或勾选多段整理后重新添加。"); return {};
        }
        // 检测分块最多 12 秒。静音边界优先，持续讲话则给出长度切分并要求复核转写。
        for (double start = 0; start < duration;) {
            if (result.size() >= 2000) { if (error) *error = QStringLiteral("本批样本超过 2000 段，请分批整理。"); return {}; }
            const double end = std::min(duration, start + (split ? 12 : 30));
            const auto decoded = AudioChunkExtractor::decodePcm16kMono(paths[file], {start, end, qRound64(start * 1000), qRound64(end * 1000)}, cancel);
            if (const auto *failure = std::get_if<AppError>(&decoded)) { if (error) *error = failure->userMessage(); return {}; }
            const auto &pcm = std::get<QVector<float>>(decoded);
            qsizetype boundary = pcm.size(), quiet = 0;
            for (qsizetype i = 0; i < pcm.size(); ++i) {
                quiet = std::abs(pcm[i]) < 0.003 ? quiet + 1 : 0;
                if (split && i >= 3 * 16000 && quiet >= 3200) { boundary = i + 1; break; }
            }
            if (pcm.isEmpty() || boundary == 0) break;
            const double finish = std::min(end, start + static_cast<double>(boundary) / 16000);
            {
                const QString target = QDir(directory).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".wav"));
                if (!derive(paths[file], target, start, finish, false, false, &problem, cancel)) { if (error) *error = problem; return {}; }
                const auto derived = AudioQuality::inspect(target, &problem, cancel);
                if (!derived) { if (error) *error = problem; return {}; }
                if (derived->peak != 0 && !content.contains(derived->sha256)) {
                    temporaryBytes += QFileInfo(target).size();
                    if (temporaryBytes > 512LL * 1024 * 1024) { if (error) *error = QStringLiteral("本批临时样本超过 512MiB，请减少素材或分批整理。"); return {}; }
                    content.insert(derived->sha256);
                    result.append(QJsonObject{{QStringLiteral("sourcePath"), paths[file]}, {QStringLiteral("sourceSha256"), QString::fromLatin1(quality->sha256.toHex())},
                        {QStringLiteral("sampleId"), QFileInfo(target).completeBaseName()},
                        {QStringLiteral("referencePath"), target}, {QStringLiteral("referenceSha256"), QString::fromLatin1(derived->sha256.toHex())},
                        {QStringLiteral("startSeconds"), start}, {QStringLiteral("endSeconds"), finish}, {QStringLiteral("quality"), derived->referenceReview()},
                        {QStringLiteral("sourceSampleRate"), quality->sampleRate}, {QStringLiteral("sourceStartSample"), QString::number(qRound64(start * quality->sampleRate))},
                        {QStringLiteral("sourceEndSample"), QString::number(qRound64(finish * quality->sampleRate))},
                        {QStringLiteral("transcript"), QString()}, {QStringLiteral("selected"), true}});
                } else QFile::remove(target); // 丢弃未引用的临时派生文件，不触碰用户原件。
            }
            start = finish;
        }
        if (RoughCutProjectSerializer::mediaSha256(paths[file], &problem, cancel) != quality->sha256) {
            if (error) *error = QStringLiteral("原件在整理期间发生变化或整理已取消，请重新整理。"); return {};
        }
        if (progress) progress(file + 1, paths.size());
    }
    return result;
}
}
