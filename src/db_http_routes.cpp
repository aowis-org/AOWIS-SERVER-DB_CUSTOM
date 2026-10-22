#include "aowis/native/db_http_routes.h"

#ifdef AOWIS_HTTP_WITH_HYDRAULIC
#include "aowis/native/hydraulic_network_persistence.h"
#include <QByteArray>
#include <QUuid>
#include <variant>
#include <type_traits>
#endif


#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponder>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QFile>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QUuid>

#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cstdint>
#include <set>
#include <limits>
#include <optional>
#include <memory>
#include <utility>
#include <stdexcept>
#include <string>

namespace aowis::native::http {
namespace {
using Status = QHttpServerResponse::StatusCode;

QHttpServerResponse errorResponse(Status status, const QString& code, const QString& message) {
    return QHttpServerResponse(QJsonObject{{"error", QJsonObject{
        {"code", code}, {"message", message}}}}, status);
}

QHttpServerResponse projectFailure(const ProjectError& error) {
    Status status = Status::BadRequest;
    QString code = QStringLiteral("invalid_request");
    switch (error.kind()) {
    case ProjectErrorKind::InvalidInput: break;
    case ProjectErrorKind::NotFound:
        status = Status::NotFound; code = QStringLiteral("not_found"); break;
    case ProjectErrorKind::Conflict:
        status = Status::Conflict; code = QStringLiteral("conflict"); break;
    case ProjectErrorKind::Unavailable:
        status = Status::ServiceUnavailable; code = QStringLiteral("unavailable"); break;
    }
    return errorResponse(status, code, QString::fromUtf8(error.what()));
}

QHttpServerResponse internalFailure(const std::exception& exception) {
    std::cerr << "AOWIS database request failed: " << exception.what() << '\n';
    return errorResponse(Status::InternalServerError, QStringLiteral("internal_error"),
                         QStringLiteral("database operation failed"));
}

template <typename Function>
QHttpServerResponse guarded(Function&& function) {
    try { return function(); }
    catch (const ProjectError& error) { return projectFailure(error); }
    catch (const std::exception& exception) { return internalFailure(exception); }
}

// Responder routes must return void; once a body is sent, no other response
// may be written. All fallible export/file setup is completed before write().
template <typename Function>
void guardedResponder(QHttpServerResponder& responder, Function&& function) {
    try { function(); }
    catch (const ProjectError& error) { responder.sendResponse(projectFailure(error)); }
    catch (const std::exception& exception) { responder.sendResponse(internalFailure(exception)); }
}

// The responder assumes ownership of a supplied QIODevice and may continue
// reading it after the route returns. Retain the private export directory for
// exactly that lifetime; close the file before deleting the directory.
class BundleExportDevice final : public QFile {
public:
    BundleExportDevice(const QString& filename, std::unique_ptr<QTemporaryDir> directory)
        : QFile(filename), directory_(std::move(directory)) {}
    ~BundleExportDevice() override { this->close(); }
private:
    std::unique_ptr<QTemporaryDir> directory_;
};

void requireInput(bool valid, const char* message) {
    if (!valid) throw ProjectError(ProjectErrorKind::InvalidInput, message);
}

QJsonObject parseBody(const QHttpServerRequest& request, qsizetype maxBytes = 16 * 1024) {
    const QByteArray body = request.body();
    requireInput(!body.isEmpty() && body.size() <= maxBytes,
                 "request body is empty or exceeds the endpoint size limit");
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &error);
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

Hash parseHash(const QString& value) {
    const std::string utf8 = value.toUtf8().toStdString();
    requireInput(utf8.size() == 64, "revision hash must contain 64 lowercase hex digits");
    for (char character : utf8)
        requireInput((character >= '0' && character <= '9') ||
                     (character >= 'a' && character <= 'f'), "invalid revision hash");
    const Hash parsed = fromHex(utf8);
    requireInput(parsed != Hash{}, "zero revision hash is invalid");
    return parsed;
}

QJsonObject revisionJson(const Revision& revision) {
    QJsonArray parents;
    for (const Hash& parent : revision.parents)
        parents.append(QString::fromStdString(toHex(parent)));
    return QJsonObject{{"id", QString::fromStdString(toHex(revision.id))},
                       {"root", QString::fromStdString(toHex(revision.root))},
                       {"referencesRoot", QString::fromStdString(toHex(revision.referencesRoot))},
                       {"modelIndexed", revision.modelIndexed}, {"parents", parents}};
}

QJsonObject projectJson(const ProjectInfo& project) {
    return QJsonObject{{"id", QString::fromStdString(project.id)},
                       {"name", QString::fromStdString(project.name)},
                       {"storage", QStringLiteral("pack")}};
}



QString hashString(const Hash& hash) {
    return QString::fromStdString(toHex(hash));
}

QJsonValue optionalHashJson(const std::optional<Hash>& hash) {
    return hash ? QJsonValue(hashString(*hash)) : QJsonValue(QJsonValue::Null);
}

void requireBranch(Repository& repository, const std::string& branch) {
    requireInput(ProjectService::validBranchName(branch), "invalid branch name");
    const std::vector<std::string> branches = repository.listBranches();
    if (std::find(branches.begin(), branches.end(), branch) == branches.end())
        throw ProjectError(ProjectErrorKind::NotFound, "branch not found");
}

Revision checkedRevision(Repository& repository, const Hash& id) {
    try { return repository.revision(id); }
    catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).find("referenced content") != std::string_view::npos)
            throw ProjectError(ProjectErrorKind::NotFound, "revision not found");
        throw;
    }
}

// A client-selected revision may be historical; it is always looked up within
// the selected project and never through an untrusted filesystem path.
Hash revisionForRead(Repository& repository, const std::string& branch,
                     const QHttpServerRequest& request) {
    requireBranch(repository, branch);
    const QUrlQuery query = request.query();
    if (!query.hasQueryItem("revision")) return repository.head(branch).id;
    const Hash revisionId = parseHash(query.queryItemValue("revision"));
    (void)checkedRevision(repository, revisionId);
    return revisionId;
}

#ifdef AOWIS_HTTP_WITH_HYDRAULIC
using HydraulicEntity = hydraulic::HydraulicEntity;

QUuid parseUuid(const QString& value) {
    requireInput(value.size() == 36, "UUID must be canonical lowercase without braces");
    const QUuid uuid(value);
    requireInput(!uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == value,
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
    return parseHash(QString::fromStdString(requiredString(object, "expectedHead")));
}

void enforceHead(Repository& repository, const std::string& branch, const Hash& expected) {
    requireBranch(repository, branch);
    if (repository.head(branch).id != expected)
        throw ProjectError(ProjectErrorKind::Conflict, "branch head does not match expectedHead");
}

// The underlying hydraulic adapter also checks expectedHead during atomic
// publication. This precheck only improves the HTTP error response.
template <typename Function>
void hydraulicWrite(Function&& function) {
    try { function(); }
    catch (const ProjectError&) { throw; }
    catch (const std::invalid_argument& error) {
        throw ProjectError(ProjectErrorKind::InvalidInput, error.what());
    }
    catch (const std::runtime_error& error) {
        const std::string message(error.what());
        if (message.find("branch head changed") != std::string::npos ||
            message.find("branch advanced") != std::string::npos ||
            message.find("branch tip") != std::string::npos)
            throw ProjectError(ProjectErrorKind::Conflict, message);
        // Hydraulic validation and incompatible edit/merge choices are
        // expected client errors; I/O and corruption failures remain 500.
        if (message.starts_with("invalid ") || message.starts_with("cannot edit ") ||
            message.starts_with("hydraulic ") || message.starts_with("duplicate ") ||
            message.starts_with("null, network-owned") ||
            message.starts_with("cannot delete referenced model UUID") ||
            message.starts_with("model reference target") ||
            message.starts_with("cannot commit model records") ||
            message.find("merge contains unused") != std::string::npos ||
            message.starts_with("cannot select both object and fields"))
            throw ProjectError(ProjectErrorKind::InvalidInput, message);
        throw;
    }
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
#endif

} // namespace

void registerRoutes(QHttpServer& server, ProjectService& service,
                    PhysicalStore defaultStorage, BundleLimits transferLimits) {
    server.route("/api/v1/health", QHttpServerRequest::Method::Get, []() {
        return QHttpServerResponse(QJsonObject{{"status", QStringLiteral("ok")}});
    });
    server.route("/api/v1/ready", QHttpServerRequest::Method::Get, [&service]() {
        return guarded([&]() {
            (void)service.listProjects();
            return QHttpServerResponse(QJsonObject{{"status", QStringLiteral("ready")}});
        });
    });
    server.route("/api/v1/projects", QHttpServerRequest::Method::Get, [&service]() {
        return guarded([&]() {
            QJsonArray projects;
            for (const ProjectInfo& project : service.listProjects())
                projects.append(projectJson(project));
            return QHttpServerResponse(QJsonObject{{"projects", projects}});
        });
    });
    server.route("/api/v1/projects", QHttpServerRequest::Method::Post,
                 [&service, defaultStorage](const QHttpServerRequest& request) {
        return guarded([&]() {
            const QJsonObject object = parseBody(request);
            requireKeys(object, {"name", "storage"});
            const std::string name = requiredString(object, "name");
            PhysicalStore storage = defaultStorage;
            if (object.contains("storage")) {
                const std::string requested = requiredString(object, "storage");
                requireInput(requested == "pack", "invalid storage backend");
                storage = PhysicalStore::Pack;
            }
            return QHttpServerResponse(projectJson(service.createProject(name, storage)), Status::Created);
        });
    });
    server.route("/api/v1/projects/<arg>", QHttpServerRequest::Method::Get,
                 [&service](const QString& id) {
        return guarded([&]() {
            return QHttpServerResponse(projectJson(service.project(id.toStdString())));
        });
    });

    // Export directly from the private bundle file to the HTTP responder.
    // Qt reads a nonsequential QIODevice in chunks, so no second full-size
    // in-memory copy of the bundle is needed for the HTTP response.
    server.route("/api/v1/projects/<arg>/branches/<arg>/bundle",
                 QHttpServerRequest::Method::Get,
                 [&service, transferLimits](const QString& id, const QString& branch,
                                            const QHttpServerRequest& request,
                                            QHttpServerResponder& responder) {
        guardedResponder(responder, [&]() {
            const std::string target = branch.toStdString();
            requireInput(ProjectService::validBranchName(target), "invalid branch name");
            const QUrlQuery query = request.query();
            for (const std::pair<QString, QString>& item : query.queryItems())
                requireInput(item.first == QStringLiteral("base"), "unknown bundle query parameter");
            const std::optional<Hash> base = query.hasQueryItem("base") ?
                std::optional<Hash>(parseHash(query.queryItemValue("base"))) : std::nullopt;
            std::unique_ptr<QTemporaryDir> temporary = std::make_unique<QTemporaryDir>();
            if (!temporary->isValid())
                throw std::runtime_error("cannot create bundle export directory");
            const QString filename = temporary->filePath(QStringLiteral("export.aowbundle"));
            const std::filesystem::path path(filename.toStdString());
            try {
                service.withRepository(id.toStdString(), [&](Repository& repository) {
                    requireBranch(repository, target);
                    if (base) repository.exportIncrementalBranchBundle(target, *base, path, transferLimits);
                    else repository.exportBranchBundle(target, path, transferLimits);
                });
            } catch (const std::runtime_error& error) {
                const std::string_view detail(error.what());
                if (detail.find("bundle transfer limit:") != std::string_view::npos) {
                    responder.sendResponse(errorResponse(Status::PayloadTooLarge, "transfer_too_large",
                                                         QString::fromUtf8(error.what())));
                    return;
                }
                if (detail.find("incremental export base") != std::string_view::npos) {
                    responder.sendResponse(errorResponse(Status::Conflict, "invalid_base",
                                                         QString::fromUtf8(error.what())));
                    return;
                }
                throw;
            }
            std::unique_ptr<BundleExportDevice> file =
                std::make_unique<BundleExportDevice>(filename, std::move(temporary));
            if (!file->open(QIODevice::ReadOnly))
                throw std::runtime_error("cannot read exported bundle");
            if (file->size() < 0 ||
                static_cast<std::uint64_t>(file->size()) > transferLimits.maxBytes) {
                responder.sendResponse(errorResponse(Status::PayloadTooLarge, "transfer_too_large",
                                                     QStringLiteral("export exceeds configured transfer limit")));
                return;
            }
            // Qt owns the device after write(). Its destructor cleans up the
            // temporary file on success, disconnect and early response teardown.
            responder.write(file.release(), QByteArray("application/vnd.aowis.bundle"));
        });
    });
    server.route("/api/v1/projects/<arg>/branches/<arg>/bundle",
                 QHttpServerRequest::Method::Post,
                 [&service, transferLimits](const QString& id, const QString& branch,
                                            const QHttpServerRequest& request) {
        return guarded([&]() -> QHttpServerResponse {
            const std::string target = branch.toStdString();
            requireInput(ProjectService::validBranchName(target), "invalid branch name");
            const QByteArray body = request.body();
            requireInput(!body.isEmpty(), "empty replication bundle");
            if (static_cast<std::uint64_t>(body.size()) > transferLimits.maxBytes)
                return errorResponse(Status::PayloadTooLarge, "transfer_too_large",
                                     QStringLiteral("bundle exceeds configured transfer limit"));
            QTemporaryDir temporary;
            if (!temporary.isValid()) throw std::runtime_error("cannot create bundle import directory");
            const QString filename = temporary.filePath(QStringLiteral("import.aowbundle"));
            QFile file(filename);
            if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
                file.write(body) != body.size() || !file.flush())
                throw std::runtime_error("cannot spool replication bundle");
            file.close();
            BundleImportResult imported;
            try {
                service.withRepository(id.toStdString(), [&](Repository& repository) {
                    imported = repository.importBranchBundle(
                        std::filesystem::path(filename.toStdString()), target, transferLimits);
                });
            } catch (const std::runtime_error& error) {
                const std::string_view detail(error.what());
                if (detail.find("bundle transfer limit:") != std::string_view::npos)
                    return errorResponse(Status::PayloadTooLarge, "transfer_too_large",
                                         QString::fromUtf8(error.what()));
                if (detail.find("divergent") != std::string_view::npos ||
                    detail.find("base revision is missing") != std::string_view::npos)
                    return errorResponse(Status::Conflict, "divergent_history",
                                         QString::fromUtf8(error.what()));
                if (detail.find("bundle") != std::string_view::npos ||
                    detail.find("bundled") != std::string_view::npos ||
                    detail.find("replication") != std::string_view::npos)
                    return errorResponse(Status::BadRequest, "invalid_bundle",
                                         QString::fromUtf8(error.what()));
                throw;
            }
            const QString status = imported.status == BundleImportStatus::Created ?
                QStringLiteral("created") :
                imported.status == BundleImportStatus::FastForward ?
                QStringLiteral("fast_forward") : QStringLiteral("up_to_date");
            return QHttpServerResponse(QJsonObject{
                {"status", status}, {"head", hashString(imported.head)},
                {"objectsImported", static_cast<qint64>(imported.objectsImported)}},
                imported.status == BundleImportStatus::Created ? Status::Created : Status::Ok);
        });
    });
    server.route("/api/v1/projects/<arg>/branches", QHttpServerRequest::Method::Get,
                 [&service](const QString& id) {
        return guarded([&]() {
            QJsonArray branches;
            for (const std::string& branch : service.listBranches(id.toStdString()))
                branches.append(QString::fromStdString(branch));
            return QHttpServerResponse(QJsonObject{{"branches", branches}});
        });
    });
    server.route("/api/v1/projects/<arg>/branches", QHttpServerRequest::Method::Post,
                 [&service](const QString& id, const QHttpServerRequest& request) {
        return guarded([&]() {
            const QJsonObject object = parseBody(request);
            requireKeys(object, {"name", "fromBranch", "revision"});
            const std::string name = requiredString(object, "name");
            std::optional<std::string> fromBranch;
            std::optional<Hash> revision;
            if (object.contains("fromBranch")) fromBranch = requiredString(object, "fromBranch");
            if (object.contains("revision")) revision = parseHash(object.value("revision").toString());
            requireInput(!revision || fromBranch.has_value(), "revision requires fromBranch");
            const std::optional<std::string_view> branchView = fromBranch ?
                std::optional<std::string_view>(*fromBranch) : std::nullopt;
            return QHttpServerResponse(revisionJson(service.createBranch(
                id.toStdString(), name, branchView, revision)), Status::Created);
        });
    });
    server.route("/api/v1/projects/<arg>/branches/<arg>/head", QHttpServerRequest::Method::Get,
                 [&service](const QString& id, const QString& branch) {
        return guarded([&]() {
            return QHttpServerResponse(revisionJson(service.head(id.toStdString(), branch.toStdString())));
        });
    });
    server.route("/api/v1/projects/<arg>/branches/<arg>/history", QHttpServerRequest::Method::Get,
                 [&service](const QString& id, const QString& branch, const QHttpServerRequest& request) {
        return guarded([&]() {
            const QUrlQuery query = request.query();
            std::size_t limit = 100;
            if (query.hasQueryItem("limit")) {
                bool ok = false;
                const unsigned result = query.queryItemValue("limit").toUInt(&ok);
                requireInput(ok && result > 0 && result <= 1000, "invalid history limit");
                limit = static_cast<std::size_t>(result);
            }
            std::optional<Hash> after;
            if (query.hasQueryItem("after")) after = parseHash(query.queryItemValue("after"));
            QJsonArray revisions;
            const std::vector<Revision> history = service.history(id.toStdString(), branch.toStdString(), limit, after);
            for (const Revision& revision : history) revisions.append(revisionJson(revision));
            return QHttpServerResponse(QJsonObject{{"revisions", revisions}});
        });
    });
    server.route("/api/v1/projects/<arg>/revisions/<arg>", QHttpServerRequest::Method::Get,
                 [&service](const QString& id, const QString& hash) {
        return guarded([&]() {
            return QHttpServerResponse(revisionJson(service.revision(id.toStdString(), parseHash(hash))));
        });
    });
    // Diffs are immutable revision-to-revision comparisons, never two moving heads.
    server.route("/api/v1/projects/<arg>/diff", QHttpServerRequest::Method::Get,
                 [&service](const QString& id, const QHttpServerRequest& request) {
        return guarded([&]() {
            const QUrlQuery query = request.query();
            requireInput(query.hasQueryItem("from") && query.hasQueryItem("to"),
                         "diff requires from and to revision hashes");
            const Hash from = parseHash(query.queryItemValue("from"));
            const Hash to = parseHash(query.queryItemValue("to"));
            QJsonArray differences;
            service.withRepository(id.toStdString(), [&](Repository& repository) {
                (void)checkedRevision(repository, from);
                (void)checkedRevision(repository, to);
                const std::vector<Difference> changes = repository.diffRevisions(from, to);
                requireInput(changes.size() <= 1000,
                             "diff exceeds 1000 records; use native library for large diffs");
                for (const Difference& item : changes) {
                    const QByteArray bytes(reinterpret_cast<const char*>(item.key.data()),
                                           static_cast<qsizetype>(item.key.size()));
                    const QUuid uuid = QUuid::fromRfc4122(bytes);
                    differences.append(QJsonObject{{"uuid", uuid.toString(QUuid::WithoutBraces)},
                                                   {"before", optionalHashJson(item.before)},
                                                   {"after", optionalHashJson(item.after)}});
                }
            });
            return QHttpServerResponse(QJsonObject{{"from", hashString(from)},
                                                   {"to", hashString(to)},
                                                   {"changes", differences}});
        });
    });
#ifdef AOWIS_HTTP_WITH_HYDRAULIC
    // Snapshot-pinned complete hydraulic network. NetworkHydraulic uses the
    // shared canonical model codec, never a second JSON field serializer.
    server.route("/api/v1/projects/<arg>/branches/<arg>/networks/<arg>",
                 QHttpServerRequest::Method::Get,
                 [&service](const QString& id, const QString& branch,
                            const QString& network, const QHttpServerRequest& request) {
        return guarded([&]() {
            const QUuid networkUuid = parseUuid(network);
            QJsonObject result;
            service.withRepository(id.toStdString(), [&](Repository& repository) {
                const Hash revisionId = revisionForRead(repository, branch.toStdString(), request);
                const Revision snapshot = repository.revision(revisionId);
                if (!snapshot.modelIndexed) {
                    if (snapshot.root == Hash{})
                        throw ProjectError(ProjectErrorKind::NotFound, "hydraulic network not found");
                    throw ProjectError(ProjectErrorKind::InvalidInput, "revision is not model-indexed");
                }
                QString error;
                const std::optional<NetworkHydraulic> loaded =
                    hydraulic::loadHydraulicNetworkAtRevision(repository, revisionId, networkUuid, &error);
                if (!loaded) {
                    if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
                    throw ProjectError(ProjectErrorKind::NotFound, "hydraulic network not found");
                }
                result = QJsonObject{{"revision", hashString(revisionId)},
                                     {"uuid", uuidString(networkUuid)},
                                     {"payloadBase64", encodePayload(ModelCodec<NetworkHydraulic>::encode(*loaded))}};
            });
            return QHttpServerResponse(result);
        });
    });
    server.route("/api/v1/projects/<arg>/branches/<arg>/networks", QHttpServerRequest::Method::Post,
                 [&service](const QString& id, const QString& branch, const QHttpServerRequest& request) {
        return guarded([&]() {
            const QJsonObject object = parseBody(request, 8 * 1024 * 1024);
            requireKeys(object, {"expectedHead", "payloadBase64"});
            const Hash expected = requiredExpectedHead(object);
            QString error;
            const std::optional<NetworkHydraulic> network = ModelCodec<NetworkHydraulic>::decode(
                decodePayload(object, "payloadBase64"), &error);
            requireInput(network.has_value() && !network->uuid.isNull(),
                         "invalid canonical hydraulic network payload");
            Revision committed;
            service.withRepository(id.toStdString(), [&](Repository& repository) {
                enforceHead(repository, branch.toStdString(), expected);
                hydraulicWrite([&]() {
                    committed = hydraulic::saveHydraulicNetwork(
                        repository, branch.toStdString(), *network, expected);
                });
            });
            return QHttpServerResponse(revisionJson(committed), Status::Created);
        });
    });
    server.route("/api/v1/projects/<arg>/branches/<arg>/networks/<arg>/entities",
                 QHttpServerRequest::Method::Get,
                 [&service](const QString& id, const QString& branch, const QString& network,
                            const QHttpServerRequest& request) {
        return guarded([&]() {
            const QUuid networkUuid = parseUuid(network);
            const QUrlQuery query = request.query();
            bool typeValid = false;
            const unsigned type = query.queryItemValue("type").toUInt(&typeValid);
            requireInput(typeValid && type < 18, "entity type must be an integer in 0..17");
            QJsonObject result;
            service.withRepository(id.toStdString(), [&](Repository& repository) {
                const Hash revisionId = revisionForRead(repository, branch.toStdString(), request);
                const Revision snapshot = repository.revision(revisionId);
                if (!snapshot.modelIndexed) {
                    if (snapshot.root == Hash{})
                        throw ProjectError(ProjectErrorKind::NotFound, "hydraulic network not found");
                    throw ProjectError(ProjectErrorKind::InvalidInput, "revision is not model-indexed");
                }
                QString error;
                const std::optional<QList<QUuid>> ids = hydraulic::listHydraulicEntityIdsAtRevision(
                    repository, revisionId, networkUuid,
                    static_cast<hydraulic::HydraulicEntityType>(type), &error);
                if (!ids) {
                    if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
                    throw ProjectError(ProjectErrorKind::NotFound, "hydraulic network not found");
                }
                requireInput(ids->size() <= 10000,
                             "entity list exceeds 10000 UUIDs; use native library for large collections");
                QJsonArray entries;
                for (const QUuid& uuid : *ids) entries.append(uuidString(uuid));
                result = QJsonObject{{"revision", hashString(revisionId)},
                                     {"type", static_cast<int>(type)}, {"uuids", entries}};
            });
            return QHttpServerResponse(result);
        });
    });
    server.route("/api/v1/projects/<arg>/branches/<arg>/networks/<arg>/entities/<arg>",
                 QHttpServerRequest::Method::Get,
                 [&service](const QString& id, const QString& branch, const QString& network,
                            const QString& entity, const QHttpServerRequest& request) {
        return guarded([&]() {
            const QUuid networkUuid = parseUuid(network);
            const QUuid entityUuid = parseUuid(entity);
            QJsonObject result;
            service.withRepository(id.toStdString(), [&](Repository& repository) {
                const Hash revisionId = revisionForRead(repository, branch.toStdString(), request);
                const Revision snapshot = repository.revision(revisionId);
                if (!snapshot.modelIndexed) {
                    if (snapshot.root == Hash{})
                        throw ProjectError(ProjectErrorKind::NotFound, "hydraulic network not found");
                    throw ProjectError(ProjectErrorKind::InvalidInput, "revision is not model-indexed");
                }
                QString error;
                const std::optional<HydraulicEntity> item = hydraulic::loadHydraulicEntityAtRevision(
                    repository, revisionId, networkUuid, entityUuid, &error);
                if (!item) {
                    if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
                    throw ProjectError(ProjectErrorKind::NotFound, "hydraulic entity not found");
                }
                result = QJsonObject{{"revision", hashString(revisionId)},
                                     {"uuid", uuidString(entityUuid)},
                                     {"type", static_cast<int>(item->index())},
                                     {"payloadBase64", encodePayload(encodeEntity(*item))}};
            });
            return QHttpServerResponse(result);
        });
    });
    server.route("/api/v1/projects/<arg>/branches/<arg>/networks/<arg>/edits",
                 QHttpServerRequest::Method::Post,
                 [&service](const QString& id, const QString& branch, const QString& network,
                            const QHttpServerRequest& request) {
        return guarded([&]() {
            const QUuid networkUuid = parseUuid(network);
            const QJsonObject object = parseBody(request, 8 * 1024 * 1024);
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
                const QUuid uuid = parseUuid(QString::fromStdString(requiredString(change, "uuid")));
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
            Revision committed;
            hydraulic::HydraulicEditDiagnostics diagnostics;
            service.withRepository(id.toStdString(), [&](Repository& repository) {
                enforceHead(repository, branch.toStdString(), expected);
                hydraulicWrite([&]() {
                    committed = hydraulic::editHydraulicNetwork(repository, branch.toStdString(),
                                                                networkUuid, changes, &diagnostics, expected);
                });
            });
            return QHttpServerResponse(QJsonObject{{"revision", revisionJson(committed)},
                                                   {"diagnostics", diagnosticJson(diagnostics)}});
        });
    });
    server.route("/api/v1/projects/<arg>/merges", QHttpServerRequest::Method::Post,
                 [&service](const QString& id, const QHttpServerRequest& request) {
        return guarded([&]() {
            const QJsonObject object = parseBody(request, 256 * 1024);
            requireKeys(object, {"target", "source", "expectedTargetHead", "expectedSourceHead",
                                 "fieldChoices", "objectChoices"});
            const std::string target = requiredString(object, "target");
            const std::string source = requiredString(object, "source");
            const Hash targetHead = parseHash(QString::fromStdString(requiredString(object, "expectedTargetHead")));
            const Hash sourceHead = parseHash(QString::fromStdString(requiredString(object, "expectedSourceHead")));
            requireInput(target != source, "merge requires different branches");
            hydraulic::HydraulicMergeSelections selections;
            selections.expectedTargetHead = targetHead;
            selections.expectedSourceHead = sourceHead;
            const QJsonArray fields = object.value("fieldChoices").toArray();
            const QJsonArray objects = object.value("objectChoices").toArray();
            requireInput(fields.size() <= 128 && objects.size() <= 128,
                         "too many merge resolution choices");
            requireInput(!object.contains("fieldChoices") || object.value("fieldChoices").isArray(),
                         "fieldChoices must be an array");
            requireInput(!object.contains("objectChoices") || object.value("objectChoices").isArray(),
                         "objectChoices must be an array");
            for (const QJsonValue& value : fields) {
                requireInput(value.isObject(), "field choice must be an object");
                const QJsonObject choice = value.toObject();
                requireKeys(choice, {"entityUuid", "field", "side"});
                selections.fields.push_back(hydraulic::HydraulicFieldChoice{
                    parseUuid(QString::fromStdString(requiredString(choice, "entityUuid"))),
                    QString::fromStdString(requiredString(choice, "field")),
                    mergeSide(choice.value("side"))});
            }
            for (const QJsonValue& value : objects) {
                requireInput(value.isObject(), "object choice must be an object");
                const QJsonObject choice = value.toObject();
                requireKeys(choice, {"entityUuid", "side"});
                selections.objects.push_back(hydraulic::HydraulicObjectChoice{
                    parseUuid(QString::fromStdString(requiredString(choice, "entityUuid"))),
                    mergeSide(choice.value("side"))});
            }
            MergeResult result;
            service.withRepository(id.toStdString(), [&](Repository& repository) {
                requireBranch(repository, target);
                requireBranch(repository, source);
                if (repository.head(target).id != targetHead || repository.head(source).id != sourceHead)
                    throw ProjectError(ProjectErrorKind::Conflict, "merge branch head has advanced");
                hydraulicWrite([&]() {
                    result = hydraulic::mergeHydraulicBranches(repository, target, source, selections);
                });
            });
            const QJsonObject output = mergeJson(result);
            return QHttpServerResponse(output, result.status == MergeStatus::Conflicts ?
                                       Status::Conflict : Status::Ok);
        });
    });
#endif

}

} // namespace aowis::native::http
