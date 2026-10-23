#include "aowis/native/db_http_routes.h"
#include "aowis/native/storage_error.h"

#ifdef AOWIS_HTTP_WITH_HYDRAULIC
#include "aowis/native/hydraulic_network_persistence.h"
#include <QByteArray>
#include <variant>
#include <type_traits>
#endif

#include <httplib.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
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

namespace aowis::native::http {
namespace {

// Response size bounds of list-like routes; larger results return 413 and
// must use the native library. Enforced before the result is materialized.
constexpr std::size_t maxHttpDiffRecords = 1000;
constexpr std::size_t maxHttpEntityIds = 10000;

const char* const bundleMime = "application/vnd.aowis.bundle";

// Exact route patterns (also used to select per-route JSON body limits).
const char* const routeProjects = "/api/v1/projects";
const char* const routeBranches = "/api/v1/projects/:id/branches";
const char* const routeBundle = "/api/v1/projects/:id/branches/:branch/bundle";
const char* const routeMerges = "/api/v1/projects/:id/merges";
const char* const routeNetworks = "/api/v1/projects/:id/branches/:branch/networks";
const char* const routeEdits = "/api/v1/projects/:id/branches/:branch/networks/:network/edits";

// A handled request produces a JSON body and a status.
struct Reply {
    int status = 200;
    QJsonObject body;
};

void send(httplib::Response& response, const Reply& reply) {
    response.status = reply.status;
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

// A failing hydraulic load reports damage as text only (an empty text means
// "not found" and is handled by the caller).
[[noreturn]] void throwLoadFailure(const QString& error) {
    throw StorageError(StorageErrorKind::Corruption, error.toStdString());
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

QString hashString(const Hash& hash) {
    return QString::fromStdString(toHex(hash));
}

QJsonValue optionalHashJson(const std::optional<Hash>& hash) {
    return hash ? QJsonValue(hashString(*hash)) : QJsonValue(QJsonValue::Null);
}

QJsonObject revisionJson(const Revision& revision) {
    QJsonArray parents;
    for (const Hash& parent : revision.parents) parents.append(hashString(parent));
    return QJsonObject{{"id", hashString(revision.id)},
                       {"root", hashString(revision.root)},
                       {"referencesRoot", hashString(revision.referencesRoot)},
                       {"modelIndexed", revision.modelIndexed}, {"parents", parents}};
}

QJsonObject projectJson(const ProjectInfo& project) {
    return QJsonObject{{"id", QString::fromStdString(project.id)},
                       {"name", QString::fromStdString(project.name)},
                       {"storage", QStringLiteral("pack")}};
}

void requireBranch(Repository& repository, const std::string& branch) {
    requireInput(ProjectService::validBranchName(branch), "invalid branch name");
    const std::vector<std::string> branches = repository.listBranches();
    if (std::find(branches.begin(), branches.end(), branch) == branches.end())
        throw ProjectError(ProjectErrorKind::NotFound, "branch not found");
}

// A client-selected revision may be historical; it is always looked up within
// the selected project and never through an untrusted filesystem path.
Hash revisionForRead(Repository& repository, const std::string& branch,
                     const httplib::Request& request) {
    requireBranch(repository, branch);
    const std::optional<std::string> requested = queryValue(request, "revision");
    if (!requested) return repository.head(branch).id;
    const Hash revisionId = parseHash(*requested);
    // Branch-scoped routes only serve that branch's own history (head or any
    // ancestor, including merged second parents). Unknown hashes: NotFound.
    if (!repository.revisionOnBranch(branch, revisionId))
        throw StorageError(StorageErrorKind::NotFound, "revision is not part of branch " + branch);
    return revisionId;
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

#ifdef AOWIS_HTTP_WITH_HYDRAULIC
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
            requireInput(entity.has_value(), "invalid canonical hydraulic entity payload");
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

Hash requiredExpectedHead(const QJsonObject& object) {
    return parseHash(requiredString(object, "expectedHead"));
}

void enforceHead(Repository& repository, const std::string& branch, const Hash& expected) {
    requireBranch(repository, branch);
    if (repository.head(branch).id != expected)
        throw ProjectError(ProjectErrorKind::Conflict, "branch head does not match expectedHead");
}

QJsonObject diagnosticJson(const hydraulic::HydraulicEditDiagnostics& diagnostics) {
    return QJsonObject{{"indexedScalarFastPath", diagnostics.indexedScalarFastPath},
                       {"indexedTopologyFastPath", diagnostics.indexedTopologyFastPath},
                       {"indexedMembershipFastPath", diagnostics.indexedMembershipFastPath},
                       {"indexedCatalogueFastPath", diagnostics.indexedCatalogueFastPath},
                       {"indexedPhysicalFastPath", diagnostics.indexedPhysicalFastPath},
                       {"indexedDependentUpdateFastPath", diagnostics.indexedDependentUpdateFastPath},
                       {"validatedEntities", static_cast<qint64>(diagnostics.validatedEntities)}};
}

hydraulic::HydraulicMergeSide mergeSide(const QJsonValue& value) {
    requireInput(value.isString() && (value.toString() == "target" || value.toString() == "source"),
                 "merge side must be target or source");
    return value.toString() == "target" ? hydraulic::HydraulicMergeSide::Target :
                                          hydraulic::HydraulicMergeSide::Source;
}

QJsonObject mergeJson(const MergeResult& result) {
    QString status = QStringLiteral("up_to_date");
    switch (result.status) {
    case MergeStatus::UpToDate: break;
    case MergeStatus::FastForward: status = QStringLiteral("fast_forward"); break;
    case MergeStatus::Merged: status = QStringLiteral("merged"); break;
    case MergeStatus::Conflicts: status = QStringLiteral("conflicts"); break;
    }
    QJsonArray conflicts;
    for (const MergeConflict& conflict : result.conflicts) {
        conflicts.append(QJsonObject{{"uuid", uuidString(hydraulic::uuidFromKey(conflict.key))},
                                     {"base", optionalHashJson(conflict.base)},
                                     {"target", optionalHashJson(conflict.target)},
                                     {"source", optionalHashJson(conflict.source)},
                                     {"reason", QString::fromStdString(conflict.reason)}});
    }
    return QJsonObject{{"status", status}, {"base", hashString(result.base)},
                       {"targetBefore", hashString(result.targetBefore)},
                       {"sourceHead", hashString(result.sourceHead)},
                       {"revision", revisionJson(result.revision)}, {"conflicts", conflicts}};
}

// A revision used by a hydraulic read must carry a model index.
void requireModelIndexed(Repository& repository, const Hash& revisionId, const char* missing) {
    const Revision snapshot = repository.revision(revisionId);
    if (snapshot.modelIndexed) return;
    if (snapshot.root == Hash{}) throw ProjectError(ProjectErrorKind::NotFound, missing);
    throw ProjectError(ProjectErrorKind::InvalidInput, "revision is not model-indexed");
}
#endif

// Per-route JSON body ceilings, checked from Content-Length before the body
// is read. Bundle uploads are bounded by the global payload limit instead.
std::optional<std::size_t> jsonBodyLimit(const httplib::Request& request) {
    if (request.method != "POST") return std::nullopt;
    const std::string& route = request.matched_route;
    if (route == routeProjects || route == routeBranches) return maxJsonSmallBytes;
    if (route == routeMerges) return maxJsonMergeBytes;
    if (route == routeNetworks || route == routeEdits) return maxJsonModelBytes;
    return std::nullopt;
}

void registerBundleRoutes(httplib::Server& server, ProjectService& service, BundleLimits limits) {
    // Export into a nameless project scratch file, then stream it in chunks.
    // The file lives exactly as long as the response (content provider).
    server.Get(routeBundle, [&service, limits](const httplib::Request& request,
                                               httplib::Response& response) {
        try {
            const std::string id = pathParam(request, "id");
            const std::string target = pathParam(request, "branch");
            requireInput(ProjectService::validBranchName(target), "invalid branch name");
            for (const httplib::Params::value_type& item : request.params)
                requireInput(item.first == "base", "unknown bundle query parameter");
            const std::optional<std::string> baseText = queryValue(request, "base");
            const std::optional<Hash> base = baseText ? std::optional<Hash>(parseHash(*baseText))
                                                      : std::nullopt;
            std::shared_ptr<ScratchFile> exported = std::make_shared<ScratchFile>();
            try {
                service.withRepository(id, [&](Repository& repository) {
                    requireBranch(repository, target);
                    *exported = repository.createScratchFile();
                    if (base) repository.exportIncrementalBranchBundle(target, *base, exported->fd(), limits);
                    else repository.exportBranchBundle(target, exported->fd(), limits);
                });
            } catch (const StorageError& error) {
                // Route-specific error codes; every other kind uses the common mapping.
                if (error.kind() == StorageErrorKind::LimitExceeded) {
                    send(response, errorReply(413, "transfer_too_large", QString::fromUtf8(error.what())));
                    return;
                }
                if (error.kind() == StorageErrorKind::Conflict) {
                    send(response, errorReply(409, "invalid_base", QString::fromUtf8(error.what())));
                    return;
                }
                throw;
            }
            const std::uint64_t size = descriptorSize(exported->fd());
            if (limits.maxBytes != 0 && size > limits.maxBytes) {
                send(response, errorReply(413, "transfer_too_large",
                                          QStringLiteral("export exceeds configured transfer limit")));
                return;
            }
            response.status = 200;
            response.set_content_provider(static_cast<std::size_t>(size), bundleMime,
                [exported](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
                    std::array<char, 64 * 1024> buffer{};
                    const std::size_t chunk = std::min(length, buffer.size());
                    const ssize_t count = ::pread(exported->fd(), buffer.data(), chunk,
                                                  static_cast<off_t>(offset));
                    if (count <= 0) return false; // aborts the response
                    return sink.write(buffer.data(), static_cast<std::size_t>(count));
                });
        } catch (...) {
            send(response, failureReply(std::current_exception()));
        }
    });

    // Stream the upload into a project scratch file while it arrives: the body
    // is never held in memory and no project lock is held during the transfer.
    server.Post(routeBundle, [&service, limits](const httplib::Request& request,
                                                httplib::Response& response,
                                                const httplib::ContentReader& reader) {
        bool bodyConsumed = false; // before that, every rejection closes the connection
        try {
            const std::string id = pathParam(request, "id");
            const std::string target = pathParam(request, "branch");
            requireInput(ProjectService::validBranchName(target), "invalid branch name");
            const std::optional<std::uint64_t> length = declaredLength(request);
            if (length && limits.maxBytes != 0 && *length > limits.maxBytes) {
                sendAndClose(response, errorReply(413, "transfer_too_large",
                                                  QStringLiteral("bundle exceeds configured transfer limit")));
                return;
            }
            ScratchFile spool = service.createScratchFile(id); // 404/400 before any body byte
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
                                                  QStringLiteral("bundle exceeds configured transfer limit")));
                return;
            }
            if (writeError) {
                errno = *writeError;
                try { throwSystemError("spool replication bundle"); }
                catch (...) { sendAndClose(response, failureReply(std::current_exception())); }
                return;
            }
            if (!complete) {
                // Client disconnected or httplib enforced the payload limit;
                // httplib reports that itself. Nothing was imported.
                response.set_header("Connection", "close");
                return;
            }
            requireInput(received != 0, "empty replication bundle");
            BundleImportResult imported;
            try {
                service.withRepository(id, [&](Repository& repository) {
                    imported = repository.importBranchBundle(spool.fd(), target, limits);
                });
            } catch (const StorageError& error) {
                switch (error.kind()) {
                case StorageErrorKind::LimitExceeded:
                    send(response, errorReply(413, "transfer_too_large", QString::fromUtf8(error.what())));
                    return;
                case StorageErrorKind::Conflict:
                    send(response, errorReply(409, "divergent_history", QString::fromUtf8(error.what())));
                    return;
                case StorageErrorKind::InvalidInput:
                    send(response, errorReply(400, "invalid_bundle", QString::fromUtf8(error.what())));
                    return;
                default:
                    throw;
                }
            }
            const QString status = imported.status == BundleImportStatus::Created ?
                QStringLiteral("created") :
                imported.status == BundleImportStatus::FastForward ?
                QStringLiteral("fast_forward") : QStringLiteral("up_to_date");
            send(response, Reply{imported.status == BundleImportStatus::Created ? 201 : 200,
                                 QJsonObject{{"status", status}, {"head", hashString(imported.head)},
                                             {"objectsImported",
                                              static_cast<qint64>(imported.objectsImported)}}});
        } catch (...) {
            if (bodyConsumed) send(response, failureReply(std::current_exception()));
            else sendAndClose(response, failureReply(std::current_exception()));
        }
    });
}

#ifdef AOWIS_HTTP_WITH_HYDRAULIC
void registerHydraulicRoutes(httplib::Server& server, ProjectService& service) {
    // Snapshot-pinned complete hydraulic network. NetworkHydraulic uses the
    // shared canonical model codec, never a second JSON field serializer.
    server.Get("/api/v1/projects/:id/branches/:branch/networks/:network",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const QUuid networkUuid = parseUuid(pathParam(request, "network"));
            QJsonObject result;
            service.withRepository(pathParam(request, "id"), [&](Repository& repository) {
                const Hash revisionId = revisionForRead(repository, pathParam(request, "branch"), request);
                requireModelIndexed(repository, revisionId, "hydraulic network not found");
                QString error;
                const std::optional<NetworkHydraulic> loaded =
                    hydraulic::loadHydraulicNetworkAtRevision(repository, revisionId, networkUuid, &error);
                if (!loaded) {
                    if (!error.isEmpty()) throwLoadFailure(error);
                    throw ProjectError(ProjectErrorKind::NotFound, "hydraulic network not found");
                }
                result = QJsonObject{{"revision", hashString(revisionId)},
                                     {"uuid", uuidString(networkUuid)},
                                     {"payloadBase64",
                                      encodePayload(ModelCodec<NetworkHydraulic>::encode(*loaded))}};
            });
            return Reply{200, result};
        });
    });
    server.Post(routeNetworks, [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject object = parseBody(request, maxJsonModelBytes);
            requireKeys(object, {"expectedHead", "payloadBase64"});
            const Hash expected = requiredExpectedHead(object);
            QString error;
            const std::optional<NetworkHydraulic> network = ModelCodec<NetworkHydraulic>::decode(
                decodePayload(object, "payloadBase64"), &error);
            requireInput(network.has_value() && !network->uuid.isNull(),
                         "invalid canonical hydraulic network payload");
            const std::string branch = pathParam(request, "branch");
            Revision committed;
            service.withRepository(pathParam(request, "id"), [&](Repository& repository) {
                enforceHead(repository, branch, expected);
                committed = hydraulic::saveHydraulicNetwork(repository, branch, *network, expected);
            });
            return Reply{201, revisionJson(committed)};
        });
    });
    server.Get("/api/v1/projects/:id/branches/:branch/networks/:network/entities",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const QUuid networkUuid = parseUuid(pathParam(request, "network"));
            const std::optional<std::string> typeText = queryValue(request, "type");
            bool typeValid = false;
            const unsigned type = typeText ? QString::fromStdString(*typeText).toUInt(&typeValid) : 0U;
            requireInput(typeValid && type < 18, "entity type must be an integer in 0..17");
            QJsonObject result;
            service.withRepository(pathParam(request, "id"), [&](Repository& repository) {
                const Hash revisionId = revisionForRead(repository, pathParam(request, "branch"), request);
                requireModelIndexed(repository, revisionId, "hydraulic network not found");
                // Bounded before materialization: more than 10000 UUIDs is 413.
                const std::optional<QList<QUuid>> ids = hydraulic::listHydraulicEntityIdsAtRevisionLimited(
                    repository, revisionId, networkUuid,
                    static_cast<hydraulic::HydraulicEntityType>(type), maxHttpEntityIds);
                if (!ids) throw ProjectError(ProjectErrorKind::NotFound, "hydraulic network not found");
                QJsonArray entries;
                for (const QUuid& uuid : *ids) entries.append(uuidString(uuid));
                result = QJsonObject{{"revision", hashString(revisionId)},
                                     {"type", static_cast<int>(type)}, {"uuids", entries}};
            });
            return Reply{200, result};
        });
    });
    server.Get("/api/v1/projects/:id/branches/:branch/networks/:network/entities/:entity",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const QUuid networkUuid = parseUuid(pathParam(request, "network"));
            const QUuid entity = parseUuid(pathParam(request, "entity"));
            QJsonObject result;
            service.withRepository(pathParam(request, "id"), [&](Repository& repository) {
                const Hash revisionId = revisionForRead(repository, pathParam(request, "branch"), request);
                requireModelIndexed(repository, revisionId, "hydraulic network not found");
                QString error;
                const std::optional<HydraulicEntity> item = hydraulic::loadHydraulicEntityAtRevision(
                    repository, revisionId, networkUuid, entity, &error);
                if (!item) {
                    if (!error.isEmpty()) throwLoadFailure(error);
                    throw ProjectError(ProjectErrorKind::NotFound, "hydraulic entity not found");
                }
                result = QJsonObject{{"revision", hashString(revisionId)},
                                     {"uuid", uuidString(entity)},
                                     {"type", static_cast<int>(item->index())},
                                     {"payloadBase64", encodePayload(encodeEntity(*item))}};
            });
            return Reply{200, result};
        });
    });
    server.Post(routeEdits, [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const QUuid networkUuid = parseUuid(pathParam(request, "network"));
            const QJsonObject object = parseBody(request, maxJsonModelBytes);
            requireKeys(object, {"expectedHead", "changes"});
            const Hash expected = requiredExpectedHead(object);
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
            const std::string branch = pathParam(request, "branch");
            Revision committed;
            hydraulic::HydraulicEditDiagnostics diagnostics;
            service.withRepository(pathParam(request, "id"), [&](Repository& repository) {
                enforceHead(repository, branch, expected);
                committed = hydraulic::editHydraulicNetwork(repository, branch, networkUuid, changes,
                                                            &diagnostics, expected);
            });
            return Reply{200, QJsonObject{{"revision", revisionJson(committed)},
                                          {"diagnostics", diagnosticJson(diagnostics)}}};
        });
    });
    server.Post(routeMerges, [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject object = parseBody(request, maxJsonMergeBytes);
            requireKeys(object, {"target", "source", "expectedTargetHead", "expectedSourceHead",
                                 "fieldChoices", "objectChoices"});
            const std::string target = requiredString(object, "target");
            const std::string source = requiredString(object, "source");
            const Hash targetHead = parseHash(requiredString(object, "expectedTargetHead"));
            const Hash sourceHead = parseHash(requiredString(object, "expectedSourceHead"));
            requireInput(target != source, "merge requires different branches");
            requireInput(!object.contains("fieldChoices") || object.value("fieldChoices").isArray(),
                         "fieldChoices must be an array");
            requireInput(!object.contains("objectChoices") || object.value("objectChoices").isArray(),
                         "objectChoices must be an array");
            hydraulic::HydraulicMergeSelections selections;
            selections.expectedTargetHead = targetHead;
            selections.expectedSourceHead = sourceHead;
            const QJsonArray fields = object.value("fieldChoices").toArray();
            const QJsonArray objects = object.value("objectChoices").toArray();
            requireInput(fields.size() <= 128 && objects.size() <= 128, "too many merge resolution choices");
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
                    parseUuid(requiredString(choice, "entityUuid")), mergeSide(choice.value("side"))});
            }
            MergeResult result;
            service.withRepository(pathParam(request, "id"), [&](Repository& repository) {
                requireBranch(repository, target);
                requireBranch(repository, source);
                if (repository.head(target).id != targetHead || repository.head(source).id != sourceHead)
                    throw ProjectError(ProjectErrorKind::Conflict, "merge branch head has advanced");
                result = hydraulic::mergeHydraulicBranches(repository, target, source, selections);
            });
            return Reply{result.status == MergeStatus::Conflicts ? 409 : 200, mergeJson(result)};
        });
    });
}
#endif

} // namespace

void registerRoutes(httplib::Server& server, ProjectService& service,
                    PhysicalStore defaultStorage, BundleLimits transferLimits) {
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

    server.Get("/api/v1/health", [](const httplib::Request&, httplib::Response& response) {
        send(response, Reply{200, QJsonObject{{"status", QStringLiteral("ok")}}});
    });
    server.Get("/api/v1/ready", [&service](const httplib::Request&, httplib::Response& response) {
        guarded(response, [&]() {
            (void)service.listProjects();
            return Reply{200, QJsonObject{{"status", QStringLiteral("ready")}}};
        });
    });
    server.Get(routeProjects, [&service](const httplib::Request&, httplib::Response& response) {
        guarded(response, [&]() {
            QJsonArray projects;
            for (const ProjectInfo& project : service.listProjects()) projects.append(projectJson(project));
            return Reply{200, QJsonObject{{"projects", projects}}};
        });
    });
    server.Post(routeProjects, [&service, defaultStorage](const httplib::Request& request,
                                                           httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject object = parseBody(request, maxJsonSmallBytes);
            requireKeys(object, {"name", "storage"});
            const std::string name = requiredString(object, "name");
            PhysicalStore storage = defaultStorage;
            if (object.contains("storage")) {
                requireInput(requiredString(object, "storage") == "pack", "invalid storage backend");
                storage = PhysicalStore::Pack;
            }
            return Reply{201, projectJson(service.createProject(name, storage))};
        });
    });
    server.Get("/api/v1/projects/:id", [&service](const httplib::Request& request,
                                                  httplib::Response& response) {
        guarded(response, [&]() {
            return Reply{200, projectJson(service.project(pathParam(request, "id")))};
        });
    });
    registerBundleRoutes(server, service, transferLimits);
    server.Get(routeBranches, [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            QJsonArray branches;
            for (const std::string& branch : service.listBranches(pathParam(request, "id")))
                branches.append(QString::fromStdString(branch));
            return Reply{200, QJsonObject{{"branches", branches}}};
        });
    });
    server.Post(routeBranches, [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject object = parseBody(request, maxJsonSmallBytes);
            requireKeys(object, {"name", "fromBranch", "revision"});
            const std::string name = requiredString(object, "name");
            std::optional<std::string> fromBranch;
            std::optional<Hash> revision;
            if (object.contains("fromBranch")) fromBranch = requiredString(object, "fromBranch");
            if (object.contains("revision")) revision = parseHash(requiredString(object, "revision"));
            requireInput(!revision || fromBranch.has_value(), "revision requires fromBranch");
            const std::optional<std::string_view> branchView = fromBranch ?
                std::optional<std::string_view>(*fromBranch) : std::nullopt;
            return Reply{201, revisionJson(service.createBranch(pathParam(request, "id"), name,
                                                                branchView, revision))};
        });
    });
    server.Get("/api/v1/projects/:id/branches/:branch/head",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            return Reply{200, revisionJson(service.head(pathParam(request, "id"),
                                                        pathParam(request, "branch")))};
        });
    });
    server.Get("/api/v1/projects/:id/branches/:branch/history",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            std::size_t limit = 100;
            if (const std::optional<std::string> text = queryValue(request, "limit")) {
                bool ok = false;
                const unsigned result = QString::fromStdString(*text).toUInt(&ok);
                requireInput(ok && result > 0 && result <= 1000, "invalid history limit");
                limit = static_cast<std::size_t>(result);
            }
            std::optional<Hash> after;
            if (const std::optional<std::string> text = queryValue(request, "after")) after = parseHash(*text);
            QJsonArray revisions;
            for (const Revision& revision : service.history(pathParam(request, "id"),
                                                            pathParam(request, "branch"), limit, after))
                revisions.append(revisionJson(revision));
            return Reply{200, QJsonObject{{"revisions", revisions}}};
        });
    });
    server.Get("/api/v1/projects/:id/revisions/:hash",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            return Reply{200, revisionJson(service.revision(pathParam(request, "id"),
                                                            parseHash(pathParam(request, "hash"))))};
        });
    });
    // Diffs are immutable revision-to-revision comparisons, never two moving heads.
    server.Get("/api/v1/projects/:id/diff", [&service](const httplib::Request& request,
                                                      httplib::Response& response) {
        guarded(response, [&]() {
            const std::optional<std::string> fromText = queryValue(request, "from");
            const std::optional<std::string> toText = queryValue(request, "to");
            requireInput(fromText && toText, "diff requires from and to revision hashes");
            const Hash from = parseHash(*fromText);
            const Hash to = parseHash(*toText);
            QJsonArray differences;
            service.withRepository(pathParam(request, "id"), [&](Repository& repository) {
                // Bounded before materialization: more than 1000 records is 413.
                for (const Difference& item : repository.diffRevisions(from, to, maxHttpDiffRecords)) {
                    const QByteArray bytes(reinterpret_cast<const char*>(item.key.data()),
                                           static_cast<qsizetype>(item.key.size()));
                    differences.append(QJsonObject{
                        {"uuid", QUuid::fromRfc4122(bytes).toString(QUuid::WithoutBraces)},
                        {"before", optionalHashJson(item.before)},
                        {"after", optionalHashJson(item.after)}});
                }
            });
            return Reply{200, QJsonObject{{"from", hashString(from)}, {"to", hashString(to)},
                                          {"changes", differences}}};
        });
    });
#ifdef AOWIS_HTTP_WITH_HYDRAULIC
    registerHydraulicRoutes(server, service);
#endif
}

} // namespace aowis::native::http
