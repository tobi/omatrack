// libyaml-backed loader/serializer for racecraft.yml.
#include "YamlConfig.h"

#include <yaml.h>

#include <QDir>
#include <QFile>
#include <QSaveFile>

namespace racecraft {
namespace {

// ── emit ────────────────────────────────────────────────────────────

bool emitScalar(yaml_emitter_t* emitter, const QString& text) {
    const QByteArray utf8 = text.toUtf8();
    yaml_event_t event{};
    // Quote everything: plain style would turn "01:23" or "no" into a
    // non-string on the next load, and configuration values are user text.
    if (!yaml_scalar_event_initialize(
            &event, nullptr, nullptr,
            reinterpret_cast<yaml_char_t*>(const_cast<char*>(utf8.constData())),
            int(utf8.size()), 0, 1, YAML_DOUBLE_QUOTED_SCALAR_STYLE))
        return false;
    return yaml_emitter_emit(emitter, &event) != 0;
}

bool emitValue(yaml_emitter_t* emitter, const QVariant& value);

bool emitMap(yaml_emitter_t* emitter, const QVariantMap& map) {
    yaml_event_t event{};
    if (!yaml_mapping_start_event_initialize(&event, nullptr, nullptr, 1,
                                             YAML_BLOCK_MAPPING_STYLE) ||
        !yaml_emitter_emit(emitter, &event))
        return false;
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        if (!emitScalar(emitter, it.key())) return false;
        if (!emitValue(emitter, it.value())) return false;
    }
    if (!yaml_mapping_end_event_initialize(&event)) return false;
    return yaml_emitter_emit(emitter, &event) != 0;
}

bool emitSeq(yaml_emitter_t* emitter, const QVariantList& items) {
    yaml_event_t event{};
    if (!yaml_sequence_start_event_initialize(&event, nullptr, nullptr, 1,
                                              YAML_BLOCK_SEQUENCE_STYLE) ||
        !yaml_emitter_emit(emitter, &event))
        return false;
    for (const QVariant& item : items)
        if (!emitValue(emitter, item)) return false;
    if (!yaml_sequence_end_event_initialize(&event)) return false;
    return yaml_emitter_emit(emitter, &event) != 0;
}

bool emitValue(yaml_emitter_t* emitter, const QVariant& value) {
    switch (value.typeId()) {
        case QMetaType::QVariantMap:
            return emitMap(emitter, value.toMap());
        case QMetaType::QVariantList:
        case QMetaType::QStringList:
            return emitSeq(emitter, value.toList());
        case QMetaType::Bool:
            return emitScalar(emitter, value.toBool()
                                           ? QStringLiteral("true")
                                           : QStringLiteral("false"));
        case QMetaType::Double:
            return emitScalar(emitter, QString::number(value.toDouble(), 'g', 10));
        default:
            return emitScalar(emitter, value.toString());
    }
}

int writeHandler(void* data, unsigned char* buffer, size_t size) {
    auto* bytes = static_cast<QByteArray*>(data);
    bytes->append(reinterpret_cast<const char*>(buffer), qsizetype(size));
    return 1;
}

QByteArray serialize(const QVariantMap& root) {
    yaml_emitter_t emitter{};
    if (!yaml_emitter_initialize(&emitter)) return {};
    QByteArray bytes;
    yaml_emitter_set_output(&emitter, writeHandler, &bytes);
    yaml_emitter_set_indent(&emitter, 2);
    yaml_emitter_set_width(&emitter, -1);
    yaml_emitter_set_unicode(&emitter, 1);

    bool ok = true;
    yaml_event_t event{};
    ok = ok && yaml_stream_start_event_initialize(&event, YAML_UTF8_ENCODING) &&
         yaml_emitter_emit(&emitter, &event);
    ok = ok &&
         yaml_document_start_event_initialize(&event, nullptr, nullptr, nullptr, 1) &&
         yaml_emitter_emit(&emitter, &event);
    ok = ok && emitMap(&emitter, root);
    ok = ok && yaml_document_end_event_initialize(&event, 1) &&
         yaml_emitter_emit(&emitter, &event);
    ok = ok && yaml_stream_end_event_initialize(&event) &&
         yaml_emitter_emit(&emitter, &event);
    yaml_emitter_delete(&emitter);
    return ok ? bytes : QByteArray();
}

// ── parse ───────────────────────────────────────────────────────────

// Recursive-descent over the libyaml event stream. Returns false on a parser
// error so a damaged file degrades to defaults instead of partial state.
bool parseNode(yaml_parser_t* parser, yaml_event_t* event, QVariant* out);

bool parseMap(yaml_parser_t* parser, QVariantMap* out) {
    for (;;) {
        yaml_event_t event{};
        if (!yaml_parser_parse(parser, &event)) return false;
        if (event.type == YAML_MAPPING_END_EVENT) {
            yaml_event_delete(&event);
            return true;
        }
        if (event.type != YAML_SCALAR_EVENT) {
            yaml_event_delete(&event);
            return false;
        }
        const QString key = QString::fromUtf8(
            reinterpret_cast<const char*>(event.data.scalar.value),
            int(event.data.scalar.length));
        yaml_event_delete(&event);

        yaml_event_t valueEvent{};
        if (!yaml_parser_parse(parser, &valueEvent)) return false;
        QVariant value;
        const bool ok = parseNode(parser, &valueEvent, &value);
        yaml_event_delete(&valueEvent);
        if (!ok) return false;
        out->insert(key, value);
    }
}

bool parseSeq(yaml_parser_t* parser, QVariantList* out) {
    for (;;) {
        yaml_event_t event{};
        if (!yaml_parser_parse(parser, &event)) return false;
        if (event.type == YAML_SEQUENCE_END_EVENT) {
            yaml_event_delete(&event);
            return true;
        }
        QVariant value;
        const bool ok = parseNode(parser, &event, &value);
        yaml_event_delete(&event);
        if (!ok) return false;
        out->append(value);
    }
}

bool parseNode(yaml_parser_t* parser, yaml_event_t* event, QVariant* out) {
    switch (event->type) {
        case YAML_SCALAR_EVENT:
            *out = QString::fromUtf8(
                reinterpret_cast<const char*>(event->data.scalar.value),
                int(event->data.scalar.length));
            return true;
        case YAML_MAPPING_START_EVENT: {
            QVariantMap map;
            if (!parseMap(parser, &map)) return false;
            *out = map;
            return true;
        }
        case YAML_SEQUENCE_START_EVENT: {
            QVariantList list;
            if (!parseSeq(parser, &list)) return false;
            *out = list;
            return true;
        }
        default:
            return false;
    }
}

QVariantMap deserialize(const QByteArray& bytes, bool* ok) {
    *ok = false;
    yaml_parser_t parser{};
    if (!yaml_parser_initialize(&parser)) return {};
    yaml_parser_set_input_string(
        &parser, reinterpret_cast<const unsigned char*>(bytes.constData()),
        size_t(bytes.size()));

    QVariantMap root;
    bool done = false;
    while (!done) {
        yaml_event_t event{};
        if (!yaml_parser_parse(&parser, &event)) break;
        if (event.type == YAML_MAPPING_START_EVENT) {
            *ok = parseMap(&parser, &root);
            yaml_event_delete(&event);
            break;
        }
        done = event.type == YAML_STREAM_END_EVENT;
        if (done) *ok = true;  // empty document
        yaml_event_delete(&event);
    }
    yaml_parser_delete(&parser);
    return *ok ? root : QVariantMap();
}

}  // namespace

// ── document ────────────────────────────────────────────────────────

YamlConfig& YamlConfig::instance() {
    static YamlConfig config;
    return config;
}

QString YamlConfig::filePath() {
    QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (base.isEmpty())
        base = QDir::home().filePath(QStringLiteral(".config"));
    const QString dir = base + QStringLiteral("/racecraft");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/racecraft.yml");
}

YamlConfig::YamlConfig() { load(); }

void YamlConfig::load() {
    QFile file(filePath());
    if (!file.exists()) {
        fresh_ = true;
        return;
    }
    if (!file.open(QIODevice::ReadOnly)) return;
    bool ok = false;
    QVariantMap parsed = deserialize(file.readAll(), &ok);
    if (ok) root_ = parsed;
}

QVariant YamlConfig::value(const QStringList& path,
                           const QVariant& fallback) const {
    QVariantMap node = root_;
    for (int i = 0; i < path.size(); ++i) {
        const auto it = node.constFind(path.at(i));
        if (it == node.cend()) return fallback;
        if (i == path.size() - 1) return it.value();
        if (it.value().typeId() != QMetaType::QVariantMap) return fallback;
        node = it.value().toMap();
    }
    return fallback;
}

QVariant YamlConfig::value(const QString& slashPath,
                           const QVariant& fallback) const {
    return value(slashPath.split('/'), fallback);
}

namespace {
/// Functional insert: QVariantMap children are values, so a nested write has
/// to rebuild the chain of parents.
QVariantMap withValue(const QVariantMap& node, const QStringList& path,
                      int index, const QVariant& value) {
    QVariantMap copy = node;
    if (index == path.size() - 1) {
        copy.insert(path.at(index), value);
        return copy;
    }
    const QVariant child = copy.value(path.at(index));
    const QVariantMap childMap = child.typeId() == QMetaType::QVariantMap
                                     ? child.toMap()
                                     : QVariantMap();
    copy.insert(path.at(index), withValue(childMap, path, index + 1, value));
    return copy;
}
}  // namespace

void YamlConfig::setValue(const QStringList& path, const QVariant& value) {
    if (path.isEmpty()) return;
    if (!value.isValid()) {
        remove(path);
        return;
    }
    if (this->value(path) == value) return;
    root_ = withValue(root_, path, 0, value);
    dirty_ = true;
}

void YamlConfig::setValue(const QString& slashPath, const QVariant& value) {
    setValue(slashPath.split('/'), value);
}

QVariantMap YamlConfig::map(const QStringList& path) const {
    const QVariant node = value(path);
    return node.typeId() == QMetaType::QVariantMap ? node.toMap()
                                                   : QVariantMap();
}

void YamlConfig::setMap(const QStringList& path, const QVariantMap& value) {
    setValue(path, value);
}

void YamlConfig::remove(const QStringList& path) {
    if (path.isEmpty()) return;
    QVariantMap parent =
        path.size() == 1 ? root_ : map(path.mid(0, path.size() - 1));
    if (parent.remove(path.last()) == 0) return;
    if (path.size() == 1)
        root_ = parent;
    else
        setValue(path.mid(0, path.size() - 1), parent);
    dirty_ = true;
}

void YamlConfig::save() {
    if (!dirty_) return;
    const QByteArray bytes = serialize(root_);
    if (bytes.isEmpty()) return;
    QSaveFile file(filePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    file.write("# racecraft configuration. Edited by the app and by hand.\n");
    file.write(bytes);
    if (file.commit()) {
        dirty_ = false;
        fresh_ = false;
    }
}

}  // namespace racecraft
