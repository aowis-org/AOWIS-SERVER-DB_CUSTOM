#include "aowis/native/db_http_routes.h"

#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUrlQuery>

#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace aowis::native::http {
namespace {
using Status = QHttpServerResponse::StatusCode;

QHttpServerResponse errorResponse(Status status, const QString& code, const QString& message) {
    return QHttpServerResponse(QJsonObject{{"error", QJsonObject{
        {"code", code}, {"message", message}}}}, status);
}

template <typename Function>
QHttpServerResponse guarded(Function&& function) {
    try { return function(); }
    catch (const ProjectError& error) {
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
    } catch (const std::exception& exception) {
        std::cerr << "AOWIS database request failed: " << exception.what() << '\n';
        return errorResponse(Status::InternalServerError, QStringLiteral("internal_error"),
                             QStringLiteral("database operation failed"));
    }
}

void requireInput(bool valid, const char* message) {
    if (!valid) throw ProjectError(ProjectErrorKind::InvalidInput, message);
}

QJsonObject parseBody(const QHttpServerRequest& request) {
    const QByteArray body = request.body();
    requireInput(!body.isEmpty() && body.size() <= 16 * 1024,
                 "request body must contain at most 16 KiB of JSON");
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
    return fromHex(utf8);
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
                       {"storage", project.storage == PhysicalStore::Stage7Pack ?
                                    QStringLiteral("pack") : QStringLiteral("blocks")}};
}

} // namespace

void registerRoutes(QHttpServer& server, ProjectService& service,
                    PhysicalStore defaultStorage) {
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
                requireInput(requested == "pack" || requested == "blocks", "invalid storage backend");
                storage = requested == "pack" ? PhysicalStore::Stage7Pack :
                                                 PhysicalStore::ExperimentalBlocks;
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
}

} // namespace aowis::native::http
