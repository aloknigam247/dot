#include "ConfigGGlobalReader.h"

#include "Config.h"
#include "NvimConnector.h"

#include <QDebug>
#include <QMetaType>
#include <QPointer>
#include <QStringList>
#include <QVariant>

namespace dot {

constexpr auto kGGlobalPrefix = "dot_";

static std::optional<QVariant> coerce(ConfigType type, const QVariant &raw) {
    switch(type) {
        case ConfigType::Bool: {
            if(raw.typeId() == QMetaType::Bool) return raw;
            bool ok = false;
            const qlonglong v = raw.toLongLong(&ok);
            if(ok) return QVariant(v != 0);
            return std::nullopt;
        }
        case ConfigType::Int: {
            bool ok = false;
            const qlonglong v = raw.toLongLong(&ok);
            if(!ok) return std::nullopt;
            return QVariant(v);
        }
        case ConfigType::Float: {
            bool ok = false;
            const double v = raw.toDouble(&ok);
            if(!ok) return std::nullopt;
            return QVariant(v);
        }
        case ConfigType::String: {
            if(raw.canConvert<QString>()) return QVariant(raw.toString());
            return std::nullopt;
        }
        case ConfigType::StringList: {
            if(raw.typeId() == QMetaType::QVariantList || raw.typeId() == QMetaType::QStringList) {
                const QVariantList list = raw.toList();
                QStringList out;
                out.reserve(list.size());
                for(const QVariant &v: list) out.push_back(v.toString());
                return QVariant(out);
            }
            if(raw.canConvert<QString>()) { return QVariant(QStringList{ raw.toString() }); }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

void ConfigGGlobalReader::read(NvimConnector &connector, Config &cfg) {
    const quint64 generation = ++cfg.m_gGlobalReadGeneration;
    QStringList names = cfg.registeredNames();
    const QPointer<Config> target(&cfg);
    for(QString &name: names) name.prepend(QLatin1StringView(kGGlobalPrefix));
    connector.readGGlobals(names,
                           [generation, target](NvimConnector::GGlobalSnapshotResult result) {
        if(!target || target->m_gGlobalReadGeneration != generation) return;
        if(!result) {
            qWarning().noquote() << "ConfigGGlobalReader:" << result.error().message;
            return;
        }
        readFromMap(*result, *target);
    });
}

void ConfigGGlobalReader::readFromMap(const QHash<QString, QVariant> &vars, Config &cfg) {
    QHash<QString, QVariant> snapshot;
    const QStringList names = cfg.registeredNames();
    snapshot.reserve(names.size());
    for(const QString &name: names) {
        const QString gName = QString::fromLatin1(kGGlobalPrefix) + name;
        const auto it = vars.constFind(gName);
        if(it == vars.cend()) continue;
        const auto coerced = coerce(cfg.type(name), *it);
        if(!coerced) {
            qWarning().noquote() << "ConfigGGlobalReader:" << gName << "has an invalid type";
            continue;
        }
        snapshot.insert(name, *coerced);
    }
    cfg.replaceGGlobals(snapshot);
}

} // namespace dot
