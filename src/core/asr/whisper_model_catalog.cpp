#include "asr/whisper_model_catalog.h"

namespace subcue {
namespace {

WhisperModelSpec makeSpec(const char *id, qint64 size, const char *sha256Hex)
{
    const QString modelId = QString::fromLatin1(id);
    WhisperModelSpec spec;
    spec.id = modelId;
    spec.fileName = QStringLiteral("ggml-%1.bin").arg(modelId);
    spec.url = QUrl(QStringLiteral("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/%1")
        .arg(spec.fileName));
    spec.size = size;
    spec.sha256Hex = QByteArray(sha256Hex);
    return spec;
}

} // namespace

QString WhisperModelCatalog::fileNameFor(QStringView id)
{
    return QStringLiteral("ggml-%1.bin").arg(id);
}

QVector<WhisperModelSpec> WhisperModelCatalog::all()
{
    return {
        makeSpec("tiny", 77'691'713, "be07e048e1e599ad46341c8d2a135645097a538221678b7acdd1b1919c6e1b21"),
        makeSpec("tiny.en", 77'704'715, "921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f"),
        makeSpec("base", 147'951'465, "60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe"),
        makeSpec("base.en", 147'964'211, "a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002"),
        makeSpec("small", 487'601'967, "1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b"),
        makeSpec("small.en", 487'614'201, "c6138d6d58ecc8322097e0f987c32f1be8bb0a18532a3f88f734d1bbf9c41e5d"),
        makeSpec("medium", 1'533'763'059, "6c14d5adee5f86394037b4e4e8b59f1673b6cee10e3cf0b11bbdbee79c156208"),
        makeSpec("medium.en", 1'533'774'781, "cc37e93478338ec7700281a7ac30a10128929eb8f427dda2e865faa8f6da4356"),
        makeSpec("large-v1", 3'094'623'691, "7d99f41a10525d0206bddadd86760181fa920438b6b33237e3118ff6c83bb53d"),
        makeSpec("large-v2", 3'094'623'691, "9a423fe4d40c82774b6af34115b8b935f34152246eb19e80e376071d3f999487"),
        makeSpec("large-v3", 3'095'033'483, "64d182b440b98d5203c4f9bd541544d84c605196c4f7b845dfa11fb23594d1e2"),
        makeSpec("large-v3-turbo", 1'624'555'275, "1fc70f774d38eb169993ac391eea357ef47c88757ef72ee5943879b7e8e2bc69"),
    };
}

std::optional<WhisperModelSpec> WhisperModelCatalog::find(QStringView id)
{
    const QVector<WhisperModelSpec> models = all();
    for (const WhisperModelSpec &spec : models) {
        if (spec.id == id) {
            return spec;
        }
    }
    return std::nullopt;
}

} // namespace subcue
