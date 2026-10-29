#include "aowis/dvmdb/dvmdb_http_routes.h"
#include "aowis/dvmdb/storage_error.h"
#include "aowis/dvmdb/revision_record_v2.h"

#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
#include "aowis/dvmdb/hydraulic_network_persistence.h"
#include <QByteArray>
#include <variant>
#include <type_traits>
#endif

#include <httplib.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QCryptographicHash>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

namespace aowis::dvmdb::http {
namespace {

// Response size bounds of list-like routes; larger results return 413 and
// must use the native library. Enforced before the result is materialized.
constexpr std::size_t maxHttpDiffRecords = 1000;
constexpr std::size_t maxHttpEntityIds = 10000;


// Exact route patterns (also used to select per-route JSON body limits).
const char* const routeProjects = "/dvmdb/v1/projects";
const char* const routeWorkingCopies = "/dvmdb/v1/working-copies";
const char* const routeWorkingCopySave = "/dvmdb/v1/working-copies/:draft/save";
const char* const routeWorkingCopyLease = "/dvmdb/v1/working-copies/:draft/lease";
const char* const routeWorkingCopyRelease = "/dvmdb/v1/working-copies/:draft/release";
const char* const routeBranches = "/dvmdb/v1/projects/:id/branches";
const char* const routeRevisionBundleV2 = "/dvmdb/v1/projects/:id/branches/:branch/bundle";
const char* const revisionBundleMimeV2 = "application/vnd.aowis.revision-bundle-v2";
const char* const routeNetworks = "/dvmdb/v1/projects/:id/branches/:branch/networks";
const char* const routeEdits = "/dvmdb/v1/projects/:id/branches/:branch/networks/:network/edits";
const char* const routeModelCommitsV2 = "/dvmdb/v1/projects/:id/branches/:branch/model-commits";
const char* const routeMergesV2 = "/dvmdb/v1/projects/:id/merges";
const char* const routeHydraulicMergesV2 = "/dvmdb/v1/projects/:id/hydraulic-merges";

// A handled request produces a JSON body and a status.
struct Reply {
    int status = 200;
    QJsonObject body;
    int retryAfterSeconds = 0; // > 0: send a Retry-After header
};

void send(httplib::Response& response, const Reply& reply) {
    response.status = reply.status;
    if (reply.retryAfterSeconds > 0)
        response.set_header("Retry-After", std::to_string(reply.retryAfterSeconds));
    response.set_content(QJsonDocument(reply.body).toJson(QJsonDocument::Compact).toStdString(),
                         "application/json");
}

Reply errorReply(int status, const QString& code, const QString& message) {
    return Reply{status, QJsonObject{{"error", QJsonObject{{"code", code}, {"message", message}}}}};
}

// Rejections that leave a request body unread must close the connection;
// otherwise httplib would drain the (possibly huge) body to keep it alive.
void sendAndClose(httplib::Response& response, const Reply& reply) {
    send(response, reply);
    response.set_header("Connection", "close");
}

Reply projectFailure(const ProjectError& error) {
    int status = 400;
    QString code = QStringLiteral("invalid_request");
    switch (error.kind()) {
    case ProjectErrorKind::InvalidInput: break;
    case ProjectErrorKind::NotFound: status = 404; code = QStringLiteral("not_found"); break;
    case ProjectErrorKind::Conflict: status = 409; code = QStringLiteral("conflict"); break;
    case ProjectErrorKind::Unavailable: status = 503; code = QStringLiteral("unavailable"); break;
    case ProjectErrorKind::Busy: {
        // The project's operation queue is full; other projects are unaffected.
        Reply reply = errorReply(503, QStringLiteral("busy"), QString::fromUtf8(error.what()));
        reply.retryAfterSeconds = 1;
        return reply;
    }
    }
    return errorReply(status, code, QString::fromUtf8(error.what()));
}

Reply internalFailure(const std::exception& exception) {
    std::cerr << "AOWIS database request failed: " << exception.what() << '\n';
    return errorReply(500, QStringLiteral("internal_error"), QStringLiteral("database operation failed"));
}

// Classification is by StorageErrorKind only. Client-caused failures return
// their message; server-side failures are logged and answered generically so
// paths, errno text and corruption details never reach remote clients.
Reply storageFailure(const StorageError& error) {
    const QString message = QString::fromUtf8(error.what());
    switch (error.kind()) {
    case StorageErrorKind::InvalidInput:
        return errorReply(400, QStringLiteral("invalid_request"), message);
    case StorageErrorKind::NotFound:
        return errorReply(404, QStringLiteral("not_found"), message);
    case StorageErrorKind::Conflict:
        return errorReply(409, QStringLiteral("conflict"), message);
    case StorageErrorKind::LimitExceeded:
        return errorReply(413, QStringLiteral("limit_exceeded"), message);
    case StorageErrorKind::Busy: {
        Reply reply = errorReply(503, QStringLiteral("scratch_busy"),
                                 QStringLiteral("native internal scratch budget is busy"));
        reply.retryAfterSeconds = 1;
        return reply;
    }
    case StorageErrorKind::Unavailable:
        std::cerr << "AOWIS database unavailable: " << error.what() << '\n';
        return errorReply(503, QStringLiteral("unavailable"),
                          QStringLiteral("repository temporarily unavailable"));
    case StorageErrorKind::ResourceExhausted:
        std::cerr << "AOWIS database storage exhausted: " << error.what() << '\n';
        return errorReply(507, QStringLiteral("insufficient_storage"),
                          QStringLiteral("database storage is exhausted"));
    case StorageErrorKind::Corruption:
    case StorageErrorKind::Io:
    case StorageErrorKind::Internal:
        break;
    }
    std::cerr << "AOWIS database request failed [" << StorageError::kindName(error.kind())
              << "]: " << error.what() << '\n';
    return errorReply(500, QStringLiteral("internal_error"), QStringLiteral("database operation failed"));
}

Reply failureReply(const std::exception_ptr& failure) {
    try {
        std::rethrow_exception(failure);
    } catch (const ProjectError& error) {
        return projectFailure(error);
    } catch (const StorageError& error) {
        return storageFailure(error);
    } catch (const std::exception& error) {
        return internalFailure(error);
    } catch (...) {
        return internalFailure(std::runtime_error("non-standard exception"));
    }
}

template <typename Function>
void guarded(httplib::Response& response, Function&& function) {
    try {
        send(response, function());
    } catch (...) {
        send(response, failureReply(std::current_exception()));
    }
}

void requireInput(bool valid, const char* message) {
    if (!valid) throw ProjectError(ProjectErrorKind::InvalidInput, message);
}

std::string pathParam(const httplib::Request& request, const char* name) {
    const std::unordered_map<std::string, std::string>::const_iterator found =
        request.path_params.find(name);
    requireInput(found != request.path_params.end(), "missing path parameter");
    return found->second;
}

std::optional<std::string> queryValue(const httplib::Request& request, const char* name) {
    if (!request.has_param(name)) return std::nullopt;
    requireInput(request.get_param_value_count(name) == 1, "query parameter given more than once");
    return request.get_param_value(name);
}

QJsonObject parseBody(const httplib::Request& request, std::size_t maxBytes) {
    requireInput(!request.body.empty() && request.body.size() <= maxBytes,
                 "request body is empty or exceeds the endpoint size limit");
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray(request.body.data(), static_cast<qsizetype>(request.body.size())), &error);
    requireInput(error.error == QJsonParseError::NoError && document.isObject(),
                 "expected a JSON object");
    return document.object();
}

void requireKeys(const QJsonObject& object, const QStringList& accepted) {
    for (QJsonObject::const_iterator it = object.begin(); it != object.end(); ++it)
        requireInput(accepted.contains(it.key()), "unknown JSON field");
}

std::string requiredString(const QJsonObject& object, const QString& key) {
    requireInput(object.value(key).isString(), "missing or invalid string field");
    return object.value(key).toString().toUtf8().toStdString();
}

Hash parseHash(const std::string& value) {
    requireInput(value.size() == 64, "revision hash must contain 64 lowercase hex digits");
    for (char character : value)
        requireInput((character >= '0' && character <= '9') ||
                     (character >= 'a' && character <= 'f'), "invalid revision hash");
    const Hash parsed = fromHex(value);
    requireInput(parsed != Hash{}, "zero revision hash is invalid");
    return parsed;
}

RevisionId parseRevisionIdV2(const std::string& text) {
    const std::optional<RevisionId> id = RevisionId::parse(text);
    requireInput(id.has_value(), "revision ID must be a valid UUIDv7");
    return *id;
}

QString revisionIdStringV2(const RevisionId& id) {
    return QString::fromStdString(id.toString());
}

Key parseModelKeyV2(const std::string& text) {
    requireInput(text.size() == 36, "model key must be a canonical UUID");
    const QString value = QString::fromStdString(text);
    const QUuid uuid(value);
    requireInput(!uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == value,
                 "invalid model key UUID");
    const QByteArray bytes = uuid.toRfc4122();
    requireInput(bytes.size() == 16, "invalid model UUID byte length");
    Key result{};
    std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes.constData()), 16, result.begin());
    return result;
}

QString modelKeyStringV2(const Key& key) {
    return QUuid::fromRfc4122(QByteArray(reinterpret_cast<const char*>(key.data()), 16))
        .toString(QUuid::WithoutBraces);
}

Bytes decodeModelPayloadV2(const QJsonObject& object) {
    requireInput(object.value("payloadBase64").isString(), "missing model payloadBase64");
    const QByteArray encoded = object.value("payloadBase64").toString().toLatin1();
    requireInput(encoded.size() <= static_cast<qsizetype>(maxJsonModelBytes),
                 "model payloadBase64 exceeds size limit");
    const QByteArray::FromBase64Result decoded = QByteArray::fromBase64Encoding(
        encoded, QByteArray::AbortOnBase64DecodingErrors);
    requireInput(static_cast<bool>(decoded), "invalid model payloadBase64");
    const QByteArray& payload = decoded.decoded;
    if (payload.isEmpty()) return {};
    const std::uint8_t* first = reinterpret_cast<const std::uint8_t*>(payload.constData());
    return Bytes(first, first + payload.size());
}


// Use decimal strings for uint64 values: JSON numbers lose precision above
// 2^53 and must not silently alter the optimistic draft generation.
std::uint64_t strictUint64(const QJsonObject& object, const QString& field) {
    const std::string value = requiredString(object, field);
    requireInput(!value.empty() && value.size() <= 20 &&
                 std::all_of(value.begin(), value.end(), [](char ch) { return ch >= '0' && ch <= '9'; }),
                 "invalid unsigned decimal field");
    std::uint64_t result = 0;
    const std::from_chars_result parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    requireInput(parsed.ec == std::errc() && parsed.ptr == value.data() + value.size(),
                 "invalid or overflowing unsigned decimal field");
    return result;
}

QJsonValue optionalRevisionJson(const std::optional<RevisionId>& id) {
    return id ? QJsonValue(revisionIdStringV2(*id)) : QJsonValue(QJsonValue::Null);
}

QJsonObject workingCopyJson(const WorkingCopyInfoV1& draft, bool includeChanges = true) {
    QJsonArray changes;
    if (includeChanges) for (const ModelChange& change : draft.candidateChanges) {
        const QString uuid = modelKeyStringV2(change.key);
        if (!change.record) {
            changes.append(QJsonObject{{"uuid", uuid}, {"delete", true}});
            continue;
        }
        QJsonArray references;
        for (const Key& reference : change.record->references)
            references.append(modelKeyStringV2(reference));
        const Bytes& payload = change.record->payload;
        const QByteArray bytes = payload.empty() ? QByteArray() :
            QByteArray(reinterpret_cast<const char*>(payload.data()),
                       static_cast<qsizetype>(payload.size()));
        changes.append(QJsonObject{{"uuid", uuid}, {"kind", static_cast<int>(change.record->kind)},
                                   {"references", references},
                                   {"payloadBase64", QString::fromLatin1(bytes.toBase64())}});
    }
    QJsonObject result{
        {"id", QString::fromStdString(draft.id)},
        {"projectId", QString::fromStdString(draft.projectId)},
        {"branchId", QString::fromStdString(draft.branchId)},
        {"baseRevision", optionalRevisionJson(draft.baseRevision)},
        {"sessionId", QString::fromStdString(draft.sessionId)},
        {"generation", QString::number(static_cast<qulonglong>(draft.generation))},
        {"dirty", draft.dirty},
        {"leaseSessionId", draft.leaseSessionId
            ? QJsonValue(QString::fromStdString(*draft.leaseSessionId)) : QJsonValue(QJsonValue::Null)},
        {"leaseExpiresAtMs", QString::number(static_cast<qulonglong>(draft.leaseExpiresAtMs))}
    };
    if (includeChanges) result.insert(QStringLiteral("changes"), changes);
    return result;
}

// Identical native model-record encoding for committed batches and private
// draft overlays. Draft saves never invoke the commit operation.
std::vector<ModelChange> parseModelChangesV2(const QJsonObject& object, bool forCommit = false) {
    requireInput(object.value("changes").isArray(), "missing changes array");
    const QJsonArray entries = object.value("changes").toArray();
    requireInput(!entries.isEmpty() && entries.size() <= 128,
                 forCommit ? "model commit requires 1..128 changes" :
                             "model save requires 1..128 changes");
    std::vector<ModelChange> changes;
    std::set<Key> seen;
    changes.reserve(static_cast<std::size_t>(entries.size()));
    for (const QJsonValue& value : entries) {
        requireInput(value.isObject(), "model change must be an object");
        const QJsonObject item = value.toObject();
        requireKeys(item, {"uuid", "delete", "kind", "references", "payloadBase64"});
        const Key key = parseModelKeyV2(requiredString(item, "uuid"));
        requireInput(seen.insert(key).second,
                     forCommit ? "duplicate model UUID in commit" : "duplicate model UUID in save");
        if (item.value("delete") == QJsonValue(true)) {
            requireInput(item.size() == 2 && item.contains("uuid"),
                         "deletion may only contain uuid and delete");
            changes.push_back(ModelChange{key, std::nullopt});
            continue;
        }
        requireInput(!item.contains("delete"), "delete must be true or absent");
        requireInput(item.value("kind").isDouble(), "missing model record kind");
        const double number = item.value("kind").toDouble();
        requireInput(number >= 0 && number <= 65535 &&
                     number == static_cast<double>(static_cast<std::uint16_t>(number)),
                     "model record kind must be uint16");
        requireInput(item.value("references").isArray(), "missing model references");
        const QJsonArray references = item.value("references").toArray();
        requireInput(references.size() <= 65535, "too many model references");
        ModelRecord record;
        record.kind = static_cast<std::uint16_t>(number);
        for (const QJsonValue& reference : references) {
            requireInput(reference.isString(), "model reference must be a UUID");
            record.references.push_back(parseModelKeyV2(reference.toString().toStdString()));
        }
        record.payload = decodeModelPayloadV2(item);
        changes.push_back(ModelChange{key, std::move(record)});
    }
    return changes;
}


QJsonObject revisionJsonV2(const RevisionGraphRevisionV2& revision) {
    QJsonArray parents;
    QJsonArray parentHashes;
    for (const RevisionGraphParentV2& parent : revision.parents) {
        parents.append(revisionIdStringV2(parent.id));
        parentHashes.append(QString::fromStdString(toHex(parent.expectedRecordHash)));
    }
    return QJsonObject{{"id", revisionIdStringV2(revision.id)},
                       {"recordHash", QString::fromStdString(toHex(revision.recordHash))},
                       {"root", QString::fromStdString(toHex(revision.root))},
                       {"referencesRoot", QString::fromStdString(toHex(revision.referencesRoot))},
                       {"modelIndexed", revision.modelIndexed}, {"parents", parents},
                       {"parentRecordHashes", parentHashes}};
}

QString hashString(const Hash& hash) {
    return QString::fromStdString(toHex(hash));
}

QJsonValue optionalHashJson(const std::optional<Hash>& hash) {
    return hash ? QJsonValue(hashString(*hash)) : QJsonValue(QJsonValue::Null);
}

QJsonObject mergeJsonV2(const MergeResultV2& result) {
    QString status = QStringLiteral("up_to_date");
    switch (result.status) {
    case MergeStatus::UpToDate: break;
    case MergeStatus::FastForward: status = QStringLiteral("fast_forward"); break;
    case MergeStatus::Merged: status = QStringLiteral("merged"); break;
    case MergeStatus::Conflicts: status = QStringLiteral("conflicts"); break;
    }
    const std::function<QJsonValue(const std::optional<RevisionId>&)> revisionValue =
        [](const std::optional<RevisionId>& id) -> QJsonValue {
            return id ? QJsonValue(revisionIdStringV2(*id)) : QJsonValue(QJsonValue::Null);
        };
    QJsonArray conflicts;
    for (const MergeConflict& conflict : result.conflicts) {
        conflicts.append(QJsonObject{{"uuid", modelKeyStringV2(conflict.key)},
            {"base", optionalHashJson(conflict.base)},
            {"target", optionalHashJson(conflict.target)},
            {"source", optionalHashJson(conflict.source)},
            {"reason", QString::fromStdString(conflict.reason)}});
    }
    return QJsonObject{{"status", status}, {"base", revisionValue(result.base)},
        {"targetBefore", revisionValue(result.targetBefore)},
        {"sourceHead", revisionValue(result.sourceHead)},
        {"head", revisionValue(result.resultHead)}, {"conflicts", conflicts}};
}


// A page token is authenticated, not merely encoded: callers cannot forge a
// snapshot rooted at an arbitrary immutable object. A new server process has
// a new key, so tokens issued before a restart must be reacquired.
const QByteArray& graphTokenKey() {
    static const QByteArray key = [] {
        QByteArray result(32, '\0');
        for (int i = 0; i < 32; i += 4) {
            const quint32 word = QRandomGenerator::system()->generate();
            for (int j = 0; j < 4; ++j)
                result[i + j] = static_cast<char>(word >> (8 * j));
        }
        return result;
    }();
    return key;
}

QByteArray graphSignature(const QByteArray& payload) {
    return QMessageAuthenticationCode::hash(payload, graphTokenKey(), QCryptographicHash::Sha256);
}

// V2 cursors pin both logical identities and content digests. Reuse the
// process-local HMAC key so callers cannot forge a historical HEAD snapshot.
QString graphTokenV2(const RevisionGraphCursorV2& cursor, const std::string& projectId) {
    QJsonArray heads;
    for (const RevisionGraphHeadV2& head : cursor.heads) {
        heads.append(QJsonArray{QString::fromStdString(head.branchId.toString()),
                                QString::fromStdString(head.name),
                                head.revision ? QJsonValue(revisionIdStringV2(*head.revision))
                                              : QJsonValue(QJsonValue::Null),
                                head.revision ? QJsonValue(hashString(head.recordHash))
                                              : QJsonValue(QJsonValue::Null),
                                head.originBranchId
                                    ? QJsonValue(QString::fromStdString(head.originBranchId->toString()))
                                    : QJsonValue(QJsonValue::Null),
                                head.forkRevisionId
                                    ? QJsonValue(revisionIdStringV2(*head.forkRevisionId))
                                    : QJsonValue(QJsonValue::Null)});
    }
    const QByteArray payload = QJsonDocument(QJsonObject{
        {"v", 2}, {"project", QString::fromStdString(projectId)},
        {"skip", static_cast<qint64>(cursor.skip)}, {"heads", heads}
    }).toJson(QJsonDocument::Compact);
    return QString::fromLatin1((payload + graphSignature(payload)).toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

RevisionGraphCursorV2 readGraphTokenV2(const std::string& encoded,
                                       const std::string& projectId) {
    requireInput(!encoded.empty() && encoded.size() <= 230000,
                 "invalid UUIDv7 graph cursor size");
    for (const char ch : encoded)
        requireInput((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                     (ch >= '0' && ch <= '9') || ch == '-' || ch == '_',
                     "invalid UUIDv7 graph cursor characters");
    const QByteArray bytes = QByteArray::fromBase64(QByteArray::fromStdString(encoded),
                                                    QByteArray::Base64UrlEncoding);
    requireInput(bytes.size() > 32, "invalid UUIDv7 graph cursor");
    const QByteArray payload = bytes.left(bytes.size() - 32);
    const QByteArray signature = bytes.right(32);
    const QByteArray expected = graphSignature(payload);
    unsigned difference = 0;
    for (int i = 0; i < 32; ++i)
        difference |= static_cast<unsigned char>(signature[i] ^ expected[i]);
    requireInput(difference == 0, "UUIDv7 graph cursor expired or invalid");
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    requireInput(error.error == QJsonParseError::NoError && document.isObject(),
                 "invalid UUIDv7 graph cursor JSON");
    const QJsonObject object = document.object();
    requireInput(object.size() == 4 && object.value("v").toInt() == 2 &&
                 object.value("project").toString().toStdString() == projectId &&
                 object.value("heads").isArray() && object.value("skip").isDouble(),
                 "invalid UUIDv7 graph cursor fields");
    const double skip = object.value("skip").toDouble();
    requireInput(skip >= 1 && skip <= 250000 &&
                 skip == static_cast<double>(static_cast<std::size_t>(skip)),
                 "invalid UUIDv7 graph cursor offset");
    RevisionGraphCursorV2 cursor;
    cursor.skip = static_cast<std::size_t>(skip);
    const QJsonArray heads = object.value("heads").toArray();
    requireInput(heads.size() <= 1024, "too many UUIDv7 graph heads");
    for (const QJsonValue& value : heads) {
        requireInput(value.isArray(), "invalid UUIDv7 graph head");
        const QJsonArray head = value.toArray();
        requireInput((head.size() == 5 || head.size() == 6) && head.at(0).isString() &&
                     head.at(1).isString() &&
                     ((head.at(2).isString() && head.at(3).isString()) ||
                      (head.at(2).isNull() && head.at(3).isNull())) &&
                     (head.at(4).isNull() || head.at(4).isString()) &&
                     (head.size() == 5 || head.at(5).isNull() || head.at(5).isString()),
                     "invalid UUIDv7 graph head fields");
        const std::optional<BranchId> branch = BranchId::parse(head.at(0).toString().toStdString());
        requireInput(branch.has_value(), "invalid graph branch UUIDv7");
        std::optional<BranchId> origin;
        if (head.at(4).isString()) {
            origin = BranchId::parse(head.at(4).toString().toStdString());
            requireInput(origin && *origin != *branch, "invalid branch creation origin");
        }
        requireInput(head.size() != 6 || head.at(5).isNull() ||
                     (origin.has_value() && head.at(2).isString()),
                     "fork revision requires a source and committed branch HEAD");
        cursor.heads.push_back({*branch, head.at(1).toString().toStdString(),
              head.at(2).isString()
                  ? std::optional<RevisionId>(parseRevisionIdV2(head.at(2).toString().toStdString()))
                  : std::nullopt,
              head.at(3).isString() ? parseHash(head.at(3).toString().toStdString()) : Hash{},
              origin, head.size() == 6 && head.at(5).isString()
                  ? std::optional<RevisionId>(parseRevisionIdV2(head.at(5).toString().toStdString()))
                  : std::nullopt});
    }
    return cursor;
}

QJsonObject projectJson(const ProjectInfo& project) {
    return QJsonObject{{"id", QString::fromStdString(project.id)},
                       {"name", QString::fromStdString(project.name)}};
}

void requireBranch(Repository& repository, const std::string& branch) {
    requireInput(ProjectService::validBranchName(branch), "invalid branch name");
    if (!repository.hasBranch(branch))
        throw ProjectError(ProjectErrorKind::NotFound, "branch not found");
}

// Content-Length of a request, if present and well-formed (httplib rejects
// malformed values before routing).
std::optional<std::uint64_t> declaredLength(const httplib::Request& request) {
    if (!request.has_header("Content-Length")) return std::nullopt;
    return static_cast<std::uint64_t>(std::stoull(request.get_header_value("Content-Length")));
}

std::uint64_t descriptorSize(int fd) {
    struct stat data{};
    if (::fstat(fd, &data) != 0) throwSystemError("stat exported bundle");
    return static_cast<std::uint64_t>(data.st_size);
}

#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
using HydraulicEntity = hydraulic::HydraulicEntity;

QUuid parseUuid(const std::string& value) {
    requireInput(value.size() == 36, "UUID must be canonical lowercase without braces");
    const QString text = QString::fromStdString(value);
    const QUuid uuid(text);
    requireInput(!uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == text,
                 "invalid canonical UUID");
    return uuid;
}

QString uuidString(const QUuid& uuid) {
    return uuid.toString(QUuid::WithoutBraces);
}

QByteArray decodePayload(const QJsonObject& object, const QString& field,
                         qsizetype maxEncoded = 8 * 1024 * 1024) {
    requireInput(object.value(field).isString(), "missing base64 payload");
    const QByteArray encoded = object.value(field).toString().toLatin1();
    requireInput(encoded.size() <= maxEncoded, "base64 payload exceeds size limit");
    const QByteArray::FromBase64Result decoded = QByteArray::fromBase64Encoding(
        encoded, QByteArray::AbortOnBase64DecodingErrors);
    requireInput(static_cast<bool>(decoded), "invalid base64 payload");
    return decoded.decoded;
}

QString encodePayload(const QByteArray& bytes) {
    return QString::fromLatin1(bytes.toBase64());
}

template <std::size_t Index = 0>
HydraulicEntity decodeEntity(std::size_t requested, const QByteArray& payload) {
    if constexpr (Index < std::variant_size_v<HydraulicEntity>) {
        if (requested == Index) {
            using Type = std::variant_alternative_t<Index, HydraulicEntity>;
            QString error;
            std::optional<Type> entity = ModelCodec<Type>::decode(payload, &error);
            if (!entity) throw ProjectError(ProjectErrorKind::InvalidInput,
                                            "invalid canonical hydraulic entity payload: " + error.toStdString());
            return HydraulicEntity{std::move(*entity)};
        }
        return decodeEntity<Index + 1>(requested, payload);
    } else {
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid hydraulic entity type");
    }
}

QByteArray encodeEntity(const HydraulicEntity& entity) {
    return std::visit([]<typename Type>(const Type& item) {
        return ModelCodec<Type>::encode(item);
    }, entity);
}

QUuid entityUuid(const HydraulicEntity& entity) {
    return std::visit([]<typename Type>(const Type& item) {
        return item.uuid;
    }, entity);
}

hydraulic::HydraulicMergeSide mergeSide(const QJsonValue& value) {
    requireInput(value.isString() && (value.toString() == "target" || value.toString() == "source"),
                 "merge side must be target or source");
    return value.toString() == "target" ? hydraulic::HydraulicMergeSide::Target :
                                          hydraulic::HydraulicMergeSide::Source;
}

#endif

#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
// Pin a branch at one UUIDv7 revision for all hydraulic network reads. A
// historical revision must be an ancestor through either merge parent.
RevisionId revisionForReadV2(Repository& repository, const ProjectId& project,
                             const std::string& branch, const httplib::Request& request) {
    requireBranch(repository, branch);
    const RevisionId tip = repository.headV2(project, branch);
    const std::optional<std::string> requested = queryValue(request, "revision");
    if (!requested) return tip;
    const RevisionId selected = parseRevisionIdV2(*requested);
    if (!repository.revisionReachableFromV2(project, tip, selected))
        throw StorageError(StorageErrorKind::NotFound, "revision is not an ancestor of branch HEAD");
    return selected;
}

ProjectId requestProjectV2(const httplib::Request& request) {
    const std::optional<ProjectId> project = ProjectId::parse(pathParam(request, "id"));
    requireInput(project.has_value(), "project ID must be a UUIDv7");
    return *project;
}

void requireModelIndexedV2(Repository& repository, const ProjectId& project,
                           const RevisionId& revisionId, const char* missing) {
    const std::optional<Bytes> bytes = repository.readRevisionV2(project, revisionId);
    if (!bytes) throw StorageError(StorageErrorKind::NotFound, "revision UUID not found");
    const RevisionRecordV2 snapshot = decodeRevisionRecordV2(*bytes);
    if (snapshot.modelIndexed) return;
    if (snapshot.root == Hash{}) throw ProjectError(ProjectErrorKind::NotFound, missing);
    throw ProjectError(ProjectErrorKind::InvalidInput, "revision is not model-indexed");
}

std::vector<hydraulic::HydraulicEntityChange> parseHydraulicChangesV2(const QJsonObject& object) {
    requireKeys(object, {"expectedHead", "changes"});
    requireInput(object.value("changes").isArray(), "missing changes array");
    const QJsonArray input = object.value("changes").toArray();
    requireInput(input.size() <= 128, "hydraulic edit batch exceeds 128 entities");
    std::vector<hydraulic::HydraulicEntityChange> changes;
    std::set<QString> unique;
    changes.reserve(static_cast<std::size_t>(input.size()));
    for (const QJsonValue& value : input) {
        requireInput(value.isObject(), "edit change must be an object");
        const QJsonObject change = value.toObject();
        requireKeys(change, {"uuid", "delete", "type", "payloadBase64"});
        const QUuid uuid = parseUuid(requiredString(change, "uuid"));
        requireInput(unique.insert(uuidString(uuid)).second, "duplicate entity UUID in edit batch");
        if (change.value("delete") == QJsonValue(true)) {
            requireInput(!change.contains("type") && !change.contains("payloadBase64"),
                         "delete must not include an entity payload");
            changes.push_back(hydraulic::HydraulicEntityChange{uuid, std::nullopt});
        } else {
            requireInput(!change.contains("delete"), "delete must be true or omitted");
            requireInput(change.value("type").isDouble(), "missing entity type");
            const int type = change.value("type").toInt(-1);
            requireInput(type >= 0 && type < 18 && change.value("type").toDouble() == type,
                         "invalid entity type");
            HydraulicEntity decoded = decodeEntity(static_cast<std::size_t>(type),
                                                   decodePayload(change, "payloadBase64"));
            requireInput(entityUuid(decoded) == uuid, "entity payload UUID differs from edit UUID");
            changes.push_back(hydraulic::HydraulicEntityChange{uuid, std::move(decoded)});
        }
    }
    return changes;
}
#endif

// Per-route JSON body ceilings, checked from Content-Length before the body
// is read. Bundle uploads are bounded by the global payload limit instead.
std::optional<std::size_t> jsonBodyLimit(const httplib::Request& request) {
    if (request.method != "POST") return std::nullopt;
    const std::string& route = request.matched_route;
    if (route == routeProjects || route == routeBranches ||
        route == routeWorkingCopies || route == routeWorkingCopyLease ||
        route == routeWorkingCopyRelease) return maxJsonSmallBytes;
    if (route == routeWorkingCopySave) return maxJsonModelBytes;
    if (route == routeMergesV2 ||
        route == routeHydraulicMergesV2) return maxJsonMergeBytes;
    if (route == routeNetworks || route == routeEdits || route == routeModelCommitsV2) return maxJsonModelBytes;
    return std::nullopt;
}

void registerBundleRoutes(httplib::Server& server, ProjectService& service, BundleLimits limits,
                          const std::shared_ptr<TransferStagingBudget>& stagingBudget) {
    // Only AOWREV21/22 bundles are exposed over HTTP. The catalogue project
    // identity is authoritative; no name-derived filenames are used.
    server.Get(routeRevisionBundleV2, [&service, limits, stagingBudget](
        const httplib::Request& request, httplib::Response& response) {
        try {
            const std::string id = pathParam(request, "id");
            const std::optional<ProjectId> project = ProjectId::parse(id);
            requireInput(project.has_value(), "project must be UUIDv7");
            const std::string branch = pathParam(request, "branch");
            requireInput(ProjectService::validBranchName(branch), "invalid branch name");
            for (const httplib::Params::value_type& item : request.params)
                requireInput(item.first == "base", "unknown revision-bundle query parameter");
            const std::optional<std::string> baseText = queryValue(request, "base");
            const std::optional<RevisionId> base = baseText ? RevisionId::parse(*baseText) : std::nullopt;
            requireInput(!baseText || base.has_value(), "base must be a revision UUIDv7");
            std::optional<TransferStagingBudget::Reservation> slot = stagingBudget->reserve(limits.maxBytes);
            if (!slot) {
                send(response, errorReply(503, "staging_busy",
                                          QStringLiteral("revision bundle staging capacity exhausted")));
                response.set_header("Retry-After", "1");
                return;
            }
            std::shared_ptr<TransferStagingBudget::Reservation> reservation =
                std::make_shared<TransferStagingBudget::Reservation>(std::move(*slot));
            std::shared_ptr<ScratchFile> exported = std::make_shared<ScratchFile>();
            service.withRepository(id, [&](Repository& repository) {
                *exported = repository.createScratchFile();
                if (base) repository.exportIncrementalRevisionBundleV2(*project, branch, *base,
                                                                         exported->fd(), limits);
                else repository.exportRevisionBundleV2(*project, branch, exported->fd(), limits);
            });
            const std::uint64_t size = descriptorSize(exported->fd());
            if (limits.maxBytes != 0 && size > limits.maxBytes) {
                send(response, errorReply(413, "transfer_too_large",
                                          QStringLiteral("revision bundle exceeds transfer limit")));
                return;
            }
            response.status = 200;
            response.set_content_provider(static_cast<std::size_t>(size), revisionBundleMimeV2,
                [exported, reservation](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
                    (void)reservation;
                    std::array<char, 64 * 1024> buffer{};
                    const std::size_t chunk = std::min(length, buffer.size());
                    const ssize_t count = ::pread(exported->fd(), buffer.data(), chunk,
                                                  static_cast<off_t>(offset));
                    if (count <= 0) return false;
                    return sink.write(buffer.data(), static_cast<std::size_t>(count));
                });
        } catch (...) {
            send(response, failureReply(std::current_exception()));
        }
    });

    server.Post(routeRevisionBundleV2, [&service, limits, stagingBudget](
        const httplib::Request& request, httplib::Response& response,
        const httplib::ContentReader& reader) {
        bool bodyConsumed = false;
        try {
            const std::string id = pathParam(request, "id");
            const std::optional<ProjectId> project = ProjectId::parse(id);
            requireInput(project.has_value(), "project must be UUIDv7");
            const std::string branch = pathParam(request, "branch");
            requireInput(ProjectService::validBranchName(branch), "invalid branch name");
            requireInput(request.params.empty(), "revision-bundle upload does not accept query parameters");
            const std::optional<std::uint64_t> length = declaredLength(request);
            if (length && limits.maxBytes != 0 && *length > limits.maxBytes) {
                sendAndClose(response, errorReply(413, "transfer_too_large",
                                                  QStringLiteral("revision bundle exceeds transfer limit")));
                return;
            }
            ProjectService::Operation operation = service.reserveOperation(id);
            std::optional<TransferStagingBudget::Reservation> reservation =
                stagingBudget->reserve(limits.maxBytes);
            if (!reservation) {
                sendAndClose(response, errorReply(503, "staging_busy",
                                                  QStringLiteral("revision bundle staging capacity exhausted")));
                response.set_header("Retry-After", "1");
                return;
            }
            ScratchFile spool = service.createScratchFile(id);
            std::uint64_t received = 0;
            bool tooLarge = false;
            std::optional<int> writeError;
            const bool complete = reader([&](const char* data, std::size_t size) {
                if (limits.maxBytes != 0 && size > limits.maxBytes - std::min(received, limits.maxBytes)) {
                    tooLarge = true;
                    return false;
                }
                std::size_t done = 0;
                while (done < size) {
                    const ssize_t written = ::pwrite(spool.fd(), data + done, size - done,
                                                     static_cast<off_t>(received + done));
                    if (written < 0 && errno == EINTR) continue;
                    if (written <= 0) {
                        writeError = written == 0 ? EIO : errno;
                        return false;
                    }
                    done += static_cast<std::size_t>(written);
                }
                received += size;
                return true;
            });
            bodyConsumed = complete;
            if (tooLarge) {
                sendAndClose(response, errorReply(413, "transfer_too_large",
                                                  QStringLiteral("revision bundle exceeds transfer limit")));
                return;
            }
            if (writeError) {
                errno = *writeError;
                throwSystemError("spool revision bundle");
            }
            if (!complete) {
                response.set_header("Connection", "close");
                return;
            }
            requireInput(received != 0, "empty revision bundle");
            std::optional<RevisionBundleImportV2> imported;
            service.withRepository(operation, [&](Repository& repository) {
                imported = repository.importRevisionBundleV2(*project, spool.fd(), branch, limits);
            });
            const QString status = imported->status == BundleImportStatus::Created ?
                QStringLiteral("created") : imported->status == BundleImportStatus::FastForward ?
                QStringLiteral("fast_forward") : QStringLiteral("up_to_date");
            send(response, Reply{imported->status == BundleImportStatus::Created ? 201 : 200,
                                 QJsonObject{{"status", status},
                                             {"head", QString::fromStdString(imported->head.toString())},
                                             {"objectsImported", static_cast<qint64>(imported->objectsImported)}}});
        } catch (...) {
            if (bodyConsumed) send(response, failureReply(std::current_exception()));
            else sendAndClose(response, failureReply(std::current_exception()));
        }
    });


}

#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
void registerHydraulicRoutes(httplib::Server& server, ProjectService& service) {
    // Distinct domain-validated merge route; generic merges do not perform
    // hydraulic semantic validation.
    server.Post(routeHydraulicMergesV2, [&service](const httplib::Request& request,
                                                   httplib::Response& response) {
        guarded(response, [&]() {
            const ProjectId project = requestProjectV2(request);
            const QJsonObject object = parseBody(request, maxJsonMergeBytes);
            requireKeys(object, {"target", "source", "expectedTargetHead", "expectedSourceHead",
                                 "fieldChoices", "objectChoices"});
            const std::string target = requiredString(object, "target");
            const std::string source = requiredString(object, "source");
            requireInput(target != source, "hydraulic merge requires different branches");
            hydraulic::HydraulicMergeSelectionsV2 selections{
                parseRevisionIdV2(requiredString(object, "expectedTargetHead")),
                parseRevisionIdV2(requiredString(object, "expectedSourceHead")), {}, {}};
            requireInput(!object.contains("fieldChoices") || object.value("fieldChoices").isArray(),
                         "fieldChoices must be an array");
            requireInput(!object.contains("objectChoices") || object.value("objectChoices").isArray(),
                         "objectChoices must be an array");
            const QJsonArray fields = object.value("fieldChoices").toArray();
            const QJsonArray objects = object.value("objectChoices").toArray();
            requireInput(fields.size() <= 128 && objects.size() <= 128 &&
                         fields.size() + objects.size() <= 128,
                         "too many hydraulic resolution choices");
            for (const QJsonValue& value : fields) {
                requireInput(value.isObject(), "field choice must be an object");
                const QJsonObject choice = value.toObject();
                requireKeys(choice, {"entityUuid", "field", "side"});
                selections.fields.push_back(hydraulic::HydraulicFieldChoice{
                    parseUuid(requiredString(choice, "entityUuid")),
                    QString::fromStdString(requiredString(choice, "field")),
                    mergeSide(choice.value("side"))});
            }
            for (const QJsonValue& value : objects) {
                requireInput(value.isObject(), "object choice must be an object");
                const QJsonObject choice = value.toObject();
                requireKeys(choice, {"entityUuid", "side"});
                selections.objects.push_back(hydraulic::HydraulicObjectChoice{
                    parseUuid(requiredString(choice, "entityUuid")),
                    mergeSide(choice.value("side"))});
            }
            MergeResultV2 result;
            service.withRepository(pathParam(request, "id"), [&](Repository& repository) {
                result = hydraulic::mergeHydraulicBranchesV2(
                    repository, project, target, source, selections);
            });
            return Reply{result.status == MergeStatus::Conflicts ? 409 : 200, mergeJsonV2(result)};
        });
    });
    // Canonical hydraulic routes use UUIDv7 revision identities and preserve
    // the existing hydraulic payload codec and semantic validation.
    server.Get("/dvmdb/v1/projects/:id/branches/:branch/networks/:network",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const ProjectId project = requestProjectV2(request);
            const QUuid networkUuid = parseUuid(pathParam(request, "network"));
            QJsonObject result;
            service.withRepository(pathParam(request, "id"), [&](Repository& repository) {
                const RevisionId revision = revisionForReadV2(repository, project,
                                                               pathParam(request, "branch"), request);
                requireModelIndexedV2(repository, project, revision, "hydraulic network not found");
                const std::optional<NetworkHydraulic> network =
                    hydraulic::loadHydraulicNetworkAtRevisionV2(repository, project, revision, networkUuid);
                if (!network) throw ProjectError(ProjectErrorKind::NotFound, "hydraulic network not found");
                result = QJsonObject{{"revision", revisionIdStringV2(revision)},
                                     {"uuid", uuidString(networkUuid)},
                                     {"payloadBase64", encodePayload(ModelCodec<NetworkHydraulic>::encode(*network))}};
            });
            return Reply{200, result};
        });
    });
    server.Post(routeNetworks, [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const ProjectId project = requestProjectV2(request);
            const QJsonObject object = parseBody(request, maxJsonModelBytes);
            requireKeys(object, {"expectedHead", "payloadBase64"});
            const RevisionId expected = parseRevisionIdV2(requiredString(object, "expectedHead"));
            QString error;
            const std::optional<NetworkHydraulic> network = ModelCodec<NetworkHydraulic>::decode(
                decodePayload(object, "payloadBase64"), &error);
            if (!network) throw ProjectError(ProjectErrorKind::InvalidInput,
                "invalid canonical hydraulic network payload: " + error.toStdString());
            requireInput(!network->uuid.isNull(), "hydraulic network UUID is null");
            RevisionId committed = expected;
            service.withRepository(pathParam(request, "id"), [&](Repository& repository) {
                const std::string branch = pathParam(request, "branch");
                requireBranch(repository, branch);
                committed = hydraulic::saveHydraulicNetworkV2(repository, project, branch, *network, expected);
            });
            return Reply{201, QJsonObject{{"id", revisionIdStringV2(committed)}}};
        });
    });
    server.Get("/dvmdb/v1/projects/:id/branches/:branch/networks/:network/entities",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const ProjectId project = requestProjectV2(request);
            const QUuid networkUuid = parseUuid(pathParam(request, "network"));
            const std::optional<std::string> typeText = queryValue(request, "type");
            bool validType = false;
            const unsigned type = typeText ? QString::fromStdString(*typeText).toUInt(&validType) : 0U;
            requireInput(validType && type < 18, "entity type must be integer in 0..17");
            QJsonObject result;
            service.withRepository(pathParam(request, "id"), [&](Repository& repository) {
                const RevisionId revision = revisionForReadV2(repository, project,
                                                               pathParam(request, "branch"), request);
                requireModelIndexedV2(repository, project, revision, "hydraulic network not found");
                const std::optional<QList<QUuid>> ids = hydraulic::listHydraulicEntityIdsAtRevisionLimitedV2(
                    repository, project, revision, networkUuid,
                    static_cast<hydraulic::HydraulicEntityType>(type), maxHttpEntityIds);
                if (!ids) throw ProjectError(ProjectErrorKind::NotFound, "hydraulic network not found");
                QJsonArray entries;
                for (const QUuid& uuid : *ids) entries.append(uuidString(uuid));
                result = QJsonObject{{"revision", revisionIdStringV2(revision)},
                                     {"type", static_cast<int>(type)}, {"uuids", entries}};
            });
            return Reply{200, result};
        });
    });
    server.Get("/dvmdb/v1/projects/:id/branches/:branch/networks/:network/entities/:entity",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const ProjectId project = requestProjectV2(request);
            const QUuid networkUuid = parseUuid(pathParam(request, "network"));
            const QUuid entity = parseUuid(pathParam(request, "entity"));
            QJsonObject result;
            service.withRepository(pathParam(request, "id"), [&](Repository& repository) {
                const RevisionId revision = revisionForReadV2(repository, project,
                                                               pathParam(request, "branch"), request);
                requireModelIndexedV2(repository, project, revision, "hydraulic network not found");
                const std::optional<HydraulicEntity> item = hydraulic::loadHydraulicEntityAtRevisionV2(
                    repository, project, revision, networkUuid, entity);
                if (!item) throw ProjectError(ProjectErrorKind::NotFound, "hydraulic entity not found");
                result = QJsonObject{{"revision", revisionIdStringV2(revision)},
                                     {"uuid", uuidString(entity)}, {"type", static_cast<int>(item->index())},
                                     {"payloadBase64", encodePayload(encodeEntity(*item))}};
            });
            return Reply{200, result};
        });
    });
    server.Post(routeEdits, [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const ProjectId project = requestProjectV2(request);
            const QUuid networkUuid = parseUuid(pathParam(request, "network"));
            const QJsonObject object = parseBody(request, maxJsonModelBytes);
            const RevisionId expected = parseRevisionIdV2(requiredString(object, "expectedHead"));
            const std::vector<hydraulic::HydraulicEntityChange> changes = parseHydraulicChangesV2(object);
            RevisionId committed = expected;
            service.withRepository(pathParam(request, "id"), [&](Repository& repository) {
                const std::string branch = pathParam(request, "branch");
                requireBranch(repository, branch);
                committed = hydraulic::editHydraulicNetworkV2(repository, project, branch, networkUuid,
                                                               changes, expected);
            });
            return Reply{200, QJsonObject{{"id", revisionIdStringV2(committed)}}};
        });
    });


}
#endif

} // namespace

void registerRoutes(httplib::Server& server, ProjectService& service,
                    BundleLimits transferLimits,
                    std::shared_ptr<TransferStagingBudget> stagingBudget) {
    // Standalone library integrations have a bounded default even when they
    // use the three-argument API. The production executable configures this.
    if (!stagingBudget) {
        const std::uint64_t maximumTransfer = transferLimits.maxBytes == 0
            ? 32ULL * 1024ULL * 1024ULL : transferLimits.maxBytes;
        stagingBudget = std::make_shared<TransferStagingBudget>(maximumTransfer * 4ULL);
    }
    if (transferLimits.maxBytes == 0 || stagingBudget->maximumBytes() < transferLimits.maxBytes)
        throw std::invalid_argument("HTTP transfer limit must be finite and fit the staging budget");
    // Global ceiling: no request body larger than the bundle transfer limit
    // (or the largest JSON body) is ever read. Exceeding it yields 413.
    const std::size_t payloadLimit = static_cast<std::size_t>(std::max<std::uint64_t>(
        transferLimits.maxBytes == 0 ? std::numeric_limits<std::uint32_t>::max()
                                     : transferLimits.maxBytes, maxJsonModelBytes));
    server.set_payload_max_length(payloadLimit);
    server.set_read_timeout(std::chrono::seconds(30));
    server.set_write_timeout(std::chrono::seconds(60));
    server.set_keep_alive_timeout(std::chrono::seconds(10));
    // Per-route JSON limits, decided from Content-Length before the body is
    // read. JSON bodies must declare their length (no chunked encoding).
    server.set_pre_request_handler([](const httplib::Request& request, httplib::Response& response) {
        const std::optional<std::size_t> limit = jsonBodyLimit(request);
        if (!limit) return httplib::Server::HandlerResponse::Unhandled;
        if (!request.has_header("Content-Length")) {
            sendAndClose(response, errorReply(411, "length_required",
                                              QStringLiteral("JSON requests must declare Content-Length")));
            return httplib::Server::HandlerResponse::Handled;
        }
        if (*declaredLength(request) > *limit) {
            sendAndClose(response, errorReply(413, "limit_exceeded",
                                              QStringLiteral("request body exceeds the endpoint size limit")));
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    // Anything a handler did not catch (should not happen) is a generic 500.
    server.set_exception_handler([](const httplib::Request&, httplib::Response& response,
                                    std::exception_ptr failure) {
        send(response, failureReply(failure));
    });
    // httplib's own rejections (413/414/431, malformed requests) get JSON bodies.
    server.set_error_handler([](const httplib::Request&, httplib::Response& response) {
        if (!response.body.empty()) return httplib::Server::HandlerResponse::Unhandled;
        const int status = response.status;
        const QString code = status == 404 ? QStringLiteral("not_found")
            : status == 413 ? QStringLiteral("limit_exceeded")
            : status == 414 ? QStringLiteral("uri_too_long")
            : status == 431 ? QStringLiteral("header_too_large")
            : status >= 500 ? QStringLiteral("internal_error") : QStringLiteral("invalid_request");
        const QString message = status == 404 ? QStringLiteral("no such route")
            : QString::fromStdString(httplib::status_message(status));
        send(response, errorReply(status, code, message));
        return httplib::Server::HandlerResponse::Handled;
    });

    server.Get("/dvmdb/v1/health", [](const httplib::Request&, httplib::Response& response) {
        send(response, Reply{200, QJsonObject{{"status", QStringLiteral("ok")}}});
    });
    server.Get("/dvmdb/v1/repository", [&service](const httplib::Request&, httplib::Response& response) {
        guarded(response, [&]() {
            return Reply{200, QJsonObject{{"repositoryId", QString::fromStdString(service.repositoryId())}}};
        });
    });
    // Read-only VC-03 repository-qualified tracking metadata. The registry
    // is configuration, not proof of endpoint identity or authorization.
    server.Get("/dvmdb/v1/repository/tracking", [&service](const httplib::Request&,
                                                            httplib::Response& response) {
        guarded(response, [&]() {
            const RepositoryRoleV1 role = service.repositoryRoleV1();
            const QString roleName = role == RepositoryRoleV1::Private ? QStringLiteral("private") :
                role == RepositoryRoleV1::Shared ? QStringLiteral("shared") :
                QStringLiteral("unconfigured");
            QJsonArray remotes;
            for (const RemoteRepositoryV1& remote : service.remoteRepositoriesV1()) {
                remotes.append(QJsonObject{
                    {"repositoryId", QString::fromStdString(remote.repositoryId)},
                    {"endpoint", QString::fromStdString(remote.endpoint)}});
            }
            QJsonArray tracked;
            for (const RemoteTrackingRefV1& ref : service.remoteTrackingRefsV1()) {
                tracked.append(QJsonObject{
                    {"remoteRepositoryId", QString::fromStdString(ref.remoteRepositoryId)},
                    {"projectId", QString::fromStdString(ref.projectId)},
                    {"branchId", QString::fromStdString(ref.branchId)},
                    {"observedHeadRevisionId", ref.observedHead
                        ? QJsonValue(QString::fromStdString(ref.observedHead->toString()))
                        : QJsonValue(QJsonValue::Null)}});
            }
            return Reply{200, QJsonObject{
                {"repositoryId", QString::fromStdString(service.repositoryId())},
                {"role", roleName}, {"remotes", remotes}, {"tracking", tracked}}};
        });
    });
    server.Get("/dvmdb/v1/ready", [&service](const httplib::Request&, httplib::Response& response) {
        guarded(response, [&]() {
            (void)service.listProjects();
            return Reply{200, QJsonObject{{"status", QStringLiteral("ready")}}};
        });
    });
    // VC-04: private, persistent working copies. These are draft operations,
    // not revision-graph commits. Repository role is checked in the native API.
    server.Get(routeWorkingCopies, [&service](const httplib::Request&, httplib::Response& response) {
        guarded(response, [&]() {
            const std::vector<WorkingCopyInfoV1> drafts = service.listWorkingCopiesV1();
            requireInput(drafts.size() <= 1000, "too many working copies for one HTTP response");
            QJsonArray items;
            for (const WorkingCopyInfoV1& draft : drafts)
                items.append(workingCopyJson(draft, false));
            return Reply{200, QJsonObject{{"workingCopies", items}}};
        });
    });
    server.Post(routeWorkingCopies, [&service](const httplib::Request& request,
                                               httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject body = parseBody(request, maxJsonSmallBytes);
            requireKeys(body, {"projectId", "branch", "baseRevision", "sessionId"});
            requireInput(body.contains("baseRevision"), "missing baseRevision (use null for unborn)");
            const QJsonValue base = body.value("baseRevision");
            requireInput(base.isNull() || base.isString(), "baseRevision must be UUIDv7 or null");
            const std::optional<RevisionId> id = base.isString()
                ? std::optional<RevisionId>(parseRevisionIdV2(base.toString().toStdString()))
                : std::nullopt;
            return Reply{201, workingCopyJson(service.createWorkingCopyV1(
                requiredString(body, "projectId"), requiredString(body, "branch"), id,
                requiredString(body, "sessionId")))};
        });
    });
    server.Get("/dvmdb/v1/working-copies/:draft", [&service](const httplib::Request& request,
                                                              httplib::Response& response) {
        guarded(response, [&]() {
            return Reply{200, workingCopyJson(service.workingCopyV1(pathParam(request, "draft")))};
        });
    });
    server.Post(routeWorkingCopyLease, [&service](const httplib::Request& request,
                                                  httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject body = parseBody(request, maxJsonSmallBytes);
            requireKeys(body, {"sessionId", "seconds"});
            requireInput(body.value("seconds").isDouble(), "missing lease seconds");
            const double seconds = body.value("seconds").toDouble();
            requireInput(seconds >= 1 && seconds <= 120 && seconds == static_cast<double>(static_cast<int>(seconds)),
                         "lease seconds must be 1..120 integer");
            const WorkingCopyLeaseV1 lease = service.acquireWorkingCopyLeaseV1(
                pathParam(request, "draft"), requiredString(body, "sessionId"),
                static_cast<std::uint32_t>(seconds));
            return Reply{200, QJsonObject{{"token", QString::fromStdString(lease.token)},
                {"expiresAtMs", QString::number(static_cast<qulonglong>(lease.expiresAtMs))},
                {"generation", QString::number(static_cast<qulonglong>(lease.generation))}}};
        });
    });
    server.Post(routeWorkingCopySave, [&service](const httplib::Request& request,
                                                 httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject body = parseBody(request, maxJsonModelBytes);
            requireKeys(body, {"sessionId", "fencingToken", "expectedGeneration", "changes"});
            const std::vector<ModelChange> changes = parseModelChangesV2(body);
            return Reply{200, workingCopyJson(service.saveWorkingCopyChangesV1(
                pathParam(request, "draft"), requiredString(body, "sessionId"),
                requiredString(body, "fencingToken"), strictUint64(body, "expectedGeneration"), changes))};
        });
    });
    server.Post(routeWorkingCopyRelease, [&service](const httplib::Request& request,
                                                    httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject body = parseBody(request, maxJsonSmallBytes);
            requireKeys(body, {"sessionId", "fencingToken"});
            service.releaseWorkingCopyLeaseV1(pathParam(request, "draft"),
                requiredString(body, "sessionId"), requiredString(body, "fencingToken"));
            return Reply{200, QJsonObject{{"released", true}}};
        });
    });
    server.Get(routeProjects, [&service](const httplib::Request&, httplib::Response& response) {
        guarded(response, [&]() {
            QJsonArray projects;
            for (const ProjectInfo& project : service.listProjects()) projects.append(projectJson(project));
            return Reply{200, QJsonObject{{"projects", projects}}};
        });
    });
    server.Post(routeProjects, [&service](const httplib::Request& request,
                                          httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject object = parseBody(request, maxJsonSmallBytes);
            requireKeys(object, {"name", "unborn"});
            const QJsonValue unborn = object.value("unborn");
            requireInput(unborn.isUndefined() || unborn.isBool(), "unborn must be a boolean");
            const std::string name = requiredString(object, "name");
            return Reply{201, projectJson(unborn.toBool(false)
                ? service.createUnbornProjectV2(name) : service.createProjectV2(name))};
        });
    });
    server.Post("/dvmdb/v1/projects/clone-target", [&service](const httplib::Request& request,
                                                                httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject object = parseBody(request, maxJsonSmallBytes);
            requireKeys(object, {"projectId", "name"});
            const std::string id = requiredString(object, "projectId");
            const std::string name = requiredString(object, "name");
            return Reply{200, projectJson(service.provisionCloneTargetV2(id, name))};
        });
    });
    server.Get("/dvmdb/v1/projects/:id", [&service](const httplib::Request& request,
                                                  httplib::Response& response) {
        guarded(response, [&]() {
            return Reply{200, projectJson(service.project(pathParam(request, "id")))};
        });
    });
    registerBundleRoutes(server, service, transferLimits, stagingBudget);
    server.Get(routeBranches, [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            QJsonArray branches;
            for (const std::string& branch : service.listBranches(pathParam(request, "id")))
                branches.append(QString::fromStdString(branch));
            return Reply{200, QJsonObject{{"branches", branches}}};
        });
    });
    // Canonical API: UUIDv7 is the only revision identity on these routes.
    // SHA-256 is integrity metadata, never an address or branch HEAD.
    server.Post(routeBranches,
                [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject object = parseBody(request, maxJsonSmallBytes);
            requireKeys(object, {"name", "fromBranch", "revision"});
            const std::string project = pathParam(request, "id");
            const std::string name = requiredString(object, "name");
            std::optional<std::string> from;
            if (object.contains("fromBranch")) from = requiredString(object, "fromBranch");
            requireInput(!object.contains("revision") || from.has_value(),
                         "revision requires fromBranch");
            // Return the identity/provenance captured by the atomic native
            // branch write. Never re-resolve a mutable branch name afterward.
            const BranchCreationResultV2 created = object.contains("revision")
                ? service.createBranchAtRevisionWithIdentityV2(project, name, *from,
                    parseRevisionIdV2(requiredString(object, "revision")))
                : service.createBranchWithIdentityV2(project, name,
                    from ? std::optional<std::string_view>(*from) : std::nullopt);
            return Reply{201, QJsonObject{{"id", revisionIdStringV2(created.revisionId)},
                {"branch", QString::fromStdString(name)},
                {"branchId", QString::fromStdString(created.branchId.toString())},
                {"originBranchId", created.originBranchId
                    ? QJsonValue(QString::fromStdString(created.originBranchId->toString()))
                    : QJsonValue(QJsonValue::Null)},
                {"forkRevisionId", created.forkRevisionId
                    ? QJsonValue(revisionIdStringV2(*created.forkRevisionId))
                    : QJsonValue(QJsonValue::Null)}}};
        });
    });
    server.Patch("/dvmdb/v1/projects/:id/branches/:branch",
                 [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject object = parseBody(request, maxJsonSmallBytes);
            requireKeys(object, {"name", "expectedHead"});
            const std::string projectId = pathParam(request, "id");
            const std::string previous = pathParam(request, "branch");
            const std::string renamed = requiredString(object, "name");
            const RevisionId expected = parseRevisionIdV2(requiredString(object, "expectedHead"));
            const std::optional<ProjectId> project = ProjectId::parse(projectId);
            requireInput(project.has_value(), "project must be UUIDv7");
            service.withRepository(projectId, [&](Repository& repository) {
                repository.renameBranchV2(*project, previous, renamed, expected);
            });
            return Reply{200, QJsonObject{{"name", QString::fromStdString(renamed)},
                                           {"head", revisionIdStringV2(expected)}}};
        });
    });
    server.Get("/dvmdb/v1/projects/:id/branches/:branch/head",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const std::string project = pathParam(request, "id");
            const std::string branch = pathParam(request, "branch");
            const std::optional<RevisionId> head = service.optionalHeadV2(project, branch);
            return Reply{200, QJsonObject{{"id", head ? QJsonValue(revisionIdStringV2(*head))
                                                      : QJsonValue(QJsonValue::Null)}}};
        });
    });
    server.Get("/dvmdb/v1/projects/:id/branches/:branch/history",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            std::size_t limit = 100;
            if (const std::optional<std::string> value = queryValue(request, "limit")) {
                bool ok = false;
                const unsigned number = QString::fromStdString(*value).toUInt(&ok);
                requireInput(ok && number > 0 && number <= 1000, "invalid history limit");
                limit = number;
            }
            std::optional<RevisionId> after;
            if (const std::optional<std::string> value = queryValue(request, "after"))
                after = parseRevisionIdV2(*value);
            QJsonArray revisions;
            for (const RevisionGraphRevisionV2& revision : service.historyV2(
                    pathParam(request, "id"), pathParam(request, "branch"), limit, after))
                revisions.append(revisionJsonV2(revision));
            return Reply{200, QJsonObject{{"revisions", revisions}}};
        });
    });
    server.Get("/dvmdb/v1/projects/:id/revisions/:revision",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const std::string projectId = pathParam(request, "id");
            const RevisionId revisionId = parseRevisionIdV2(pathParam(request, "revision"));
            const std::optional<ProjectId> project = ProjectId::parse(projectId);
            requireInput(project.has_value(), "project must be UUIDv7");
            std::optional<RevisionRecordV2> record;
            service.withRepository(projectId, [&](Repository& repository) {
                const std::optional<Bytes> bytes = repository.readRevisionV2(*project, revisionId);
                if (bytes) record = decodeRevisionRecordV2(*bytes);
            });
            if (!record) throw ProjectError(ProjectErrorKind::NotFound, "revision not found");
            const Hash digest = hashRevisionRecordV2(*record);
            RevisionGraphRevisionV2 described{record->id, digest, record->root,
                                              record->referencesRoot, record->modelIndexed, {}};
            for (const RevisionParentV2& parent : record->parents)
                described.parents.push_back({parent.id, parent.expectedRecordHash});
            return Reply{200, revisionJsonV2(described)};
        });
    });
    server.Get("/dvmdb/v1/projects/:id/revision-graph",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const std::string project = pathParam(request, "id");
            std::size_t limit = 100;
            if (const std::optional<std::string> value = queryValue(request, "limit")) {
                bool ok = false;
                const unsigned number = QString::fromStdString(*value).toUInt(&ok);
                requireInput(ok && number > 0 && number <= 1000, "invalid graph limit");
                limit = number;
            }
            std::optional<RevisionGraphCursorV2> cursor;
            if (const std::optional<std::string> value = queryValue(request, "cursor"))
                cursor = readGraphTokenV2(*value, project);
            const RevisionGraphPageV2 page = service.revisionGraphV2(project, limit, std::move(cursor));
            QJsonArray heads;
            for (const RevisionGraphHeadV2& head : page.heads)
                heads.append(QJsonObject{{"branch", QString::fromStdString(head.name)},
                    {"branchId", QString::fromStdString(head.branchId.toString())},
                    {"revision", head.revision
                        ? QJsonValue(revisionIdStringV2(*head.revision)) : QJsonValue(QJsonValue::Null)},
                    {"recordHash", head.revision
                        ? QJsonValue(hashString(head.recordHash)) : QJsonValue(QJsonValue::Null)},
                    {"originBranchId", head.originBranchId
                        ? QJsonValue(QString::fromStdString(head.originBranchId->toString()))
                        : QJsonValue(QJsonValue::Null)},
                    {"forkRevisionId", head.forkRevisionId
                        ? QJsonValue(revisionIdStringV2(*head.forkRevisionId))
                        : QJsonValue(QJsonValue::Null)}});
            QJsonArray revisions;
            for (const RevisionGraphRevisionV2& revision : page.revisions)
                revisions.append(revisionJsonV2(revision));
            return Reply{200, QJsonObject{{"heads", heads}, {"revisions", revisions},
                {"complete", !page.next.has_value()},
                {"nextCursor", page.next ? QJsonValue(graphTokenV2(*page.next, project))
                                         : QJsonValue(QJsonValue::Null)}}};
        });
    });
    // This is a generic native ModelRecord batch, not the high-level hydraulic
    // editor: the caller supplies opaque model payload bytes. The native store
    // enforces reference integrity but does not decode hydraulic field semantics.
    server.Post(routeModelCommitsV2, [&service](const httplib::Request& request,
                                                 httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject object = parseBody(request, maxJsonModelBytes);
            requireKeys(object, {"expectedHead", "changes"});
            const RevisionId expected = parseRevisionIdV2(requiredString(object, "expectedHead"));
            std::vector<ModelChange> changes = parseModelChangesV2(object, true);
            const RevisionId saved = service.commitModelV2(pathParam(request, "id"),
                                           pathParam(request, "branch"), std::move(changes), expected);
            return Reply{201, QJsonObject{{"id", revisionIdStringV2(saved)}}};
        });
    });
    server.Post(routeMergesV2, [&service](const httplib::Request& request,
                                          httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject object = parseBody(request, maxJsonMergeBytes);
            requireKeys(object, {"target", "source", "expectedTargetHead", "expectedSourceHead"});
            const std::string target = requiredString(object, "target");
            const std::string source = requiredString(object, "source");
            requireInput(target != source, "merge branches must differ");
            const RevisionId expectedTarget = parseRevisionIdV2(requiredString(object, "expectedTargetHead"));
            const RevisionId expectedSource = parseRevisionIdV2(requiredString(object, "expectedSourceHead"));
            const MergeResultV2 result = service.mergeBranchesV2(pathParam(request, "id"),
                                             target, source, expectedTarget, expectedSource);
            return Reply{result.status == MergeStatus::Conflicts ? 409 : 200, mergeJsonV2(result)};
        });
    });
    server.Get("/dvmdb/v1/projects/:id/diff", [&service](const httplib::Request& request,
                                                            httplib::Response& response) {
        guarded(response, [&]() {
            const std::optional<std::string> from = queryValue(request, "from");
            const std::optional<std::string> to = queryValue(request, "to");
            requireInput(from && to, "diff-v2 requires from and to UUIDv7 revisions");
            const RevisionId before = parseRevisionIdV2(*from);
            const RevisionId after = parseRevisionIdV2(*to);
            const std::string project = pathParam(request, "id");
            const std::optional<ProjectId> projectId = ProjectId::parse(project);
            requireInput(projectId.has_value(), "project ID must be UUIDv7");
            QJsonArray changes;
            service.withRepository(project, [&](Repository& repository) {
                for (const Difference& difference : repository.diffRevisionsV2(
                        *projectId, before, after, maxHttpDiffRecords)) {
                    changes.append(QJsonObject{{"uuid", modelKeyStringV2(difference.key)},
                        {"before", optionalHashJson(difference.before)},
                        {"after", optionalHashJson(difference.after)}});
                }
            });
            return Reply{200, QJsonObject{{"from", revisionIdStringV2(before)},
                {"to", revisionIdStringV2(after)}, {"changes", changes}}};
        });
    });
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
    registerHydraulicRoutes(server, service);
#endif
}

} // namespace aowis::dvmdb::http
