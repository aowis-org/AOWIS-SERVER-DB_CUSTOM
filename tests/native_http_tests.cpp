#include "aowis/native/db_http_routes.h"
#ifdef AOWIS_HTTP_WITH_HYDRAULIC
#include "aowis/native/hydraulic_network_persistence.h"
#endif

#include <QCoreApplication>
#include <QUuid>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QHttpServer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

class TemporaryRoot {
public:
    TemporaryRoot() {
        char name[] = "/tmp/aowis-db-http-XXXXXX";
        char* result = ::mkdtemp(name);
        check(result != nullptr, "mkdtemp failed");
        this->root_ = result;
    }
    ~TemporaryRoot() { std::error_code error; std::filesystem::remove_all(this->root_, error); }
    const std::filesystem::path& root() const { return this->root_; }
private:
    std::filesystem::path root_;
};

struct Response {
    int status = 0;
    QJsonObject json;
};

class Client {
public:
    explicit Client(quint16 port)
        : base_(QStringLiteral("http://127.0.0.1:%1").arg(port)) {}
    Response call(const QByteArray& verb, const QString& path, const QJsonObject& body = {}) {
        QNetworkRequest request(QUrl(this->base_ + path));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        QNetworkReply* reply = nullptr;
        if (verb == "GET") reply = this->manager_.get(request);
        else reply = this->manager_.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(5000);
        loop.exec();
        check(timer.isActive() && reply->isFinished(), "HTTP request timed out");
        const QByteArray bytes = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonDocument json = QJsonDocument::fromJson(bytes);
        reply->deleteLater();
        check(json.isObject(), "HTTP response is not JSON");
        return Response{status, json.object()};
    }
private:
    QString base_;
    QNetworkAccessManager manager_;
};

#ifdef AOWIS_HTTP_WITH_HYDRAULIC
void exerciseHydraulicApi(Client& client, aowis::native::ProjectService& service,
                          const QString& projectId, const QString& otherProject,
                          const QString& rawHead) {
    namespace native = aowis::native;
    const QString prefix = QStringLiteral("/api/v1/projects/") + projectId;
    const QString main = prefix + "/branches/main";
    const QUuid networkUuid = QUuid::createUuid();
    const QUuid junctionUuid = QUuid::createUuid();
    const QString networkId = networkUuid.toString(QUuid::WithoutBraces);
    const QString junctionId = junctionUuid.toString(QUuid::WithoutBraces);
    const QString networkRoute = main + "/networks/" + networkId;

    check(client.call("GET", networkRoute).status == 404,
          "untouched branch must have no hydraulic network");
    NetworkHydraulic network;
    network.uuid = networkUuid;
    network.id = QStringLiteral("api-test");
    HydraulicNodeJunction junction;
    junction.uuid = junctionUuid;
    junction.id = QStringLiteral("J1");
    junction.elevation_m = 123;
    network.nodes_junctions.append(junction);
    const QString payload = QString::fromLatin1(native::ModelCodec<NetworkHydraulic>::encode(network).toBase64());
    check(client.call("POST", main + "/networks",
         QJsonObject{{"expectedHead", rawHead}, {"payloadBase64", "!base64!"}}).status == 400,
          "invalid base64 network accepted");
    check(client.call("POST", main + "/networks",
         QJsonObject{{"payloadBase64", payload}}).status == 400,
          "save without expectedHead accepted");
    const Response created = client.call("POST", main + "/networks",
        QJsonObject{{"expectedHead", rawHead}, {"payloadBase64", payload}});
    check(created.status == 201, "first hydraulic HTTP save failed");
    const QString createdHead = created.json.value("id").toString();
    check(createdHead.size() == 64 && createdHead != rawHead,
          "hydraulic HTTP save did not advance revision");
    check(client.call("POST", main + "/networks",
        QJsonObject{{"expectedHead", rawHead}, {"payloadBase64", payload}}).status == 409,
          "stale network save accepted");
    NetworkHydraulic invalid = network;
    invalid.nodes_junctions.append(junction); // duplicate UUID must be rejected by hydraulic semantics
    check(client.call("POST", main + "/networks",
        QJsonObject{{"expectedHead", createdHead},
                    {"payloadBase64", QString::fromLatin1(
                        native::ModelCodec<NetworkHydraulic>::encode(invalid).toBase64())}}).status == 400,
          "invalid duplicate-UUID network accepted");
    check(client.call("GET", main + "/head").json.value("id").toString() == createdHead,
          "semantic validation failure changed branch head");
    check(client.call("GET", "/api/v1/projects/" + otherProject +
                      "/branches/main/networks/" + networkId).status == 404,
          "hydraulic network leaked across projects");

    const Response read = client.call("GET", networkRoute);
    check(read.status == 200 && read.json.value("revision").toString() == createdHead,
          "network GET revision differs from saved head");
    const QByteArray readBytes = QByteArray::fromBase64(read.json.value("payloadBase64").toString().toLatin1());
    QString codecError;
    const std::optional<NetworkHydraulic> recovered =
        native::ModelCodec<NetworkHydraulic>::decode(readBytes, &codecError);
    check(recovered && recovered->uuid == networkUuid && recovered->nodes_junctions.size() == 1,
          "HTTP canonical network payload failed to roundtrip");
    const Response ids = client.call("GET", networkRoute + "/entities?type=9");
    check(ids.status == 200 && ids.json.value("uuids").toArray().size() == 1 &&
          ids.json.value("uuids").toArray().first().toString() == junctionId,
          "HTTP membership listing incorrect");
    check(client.call("GET", networkRoute + "/entities?type=18").status == 400,
          "invalid entity type accepted");
    const Response item = client.call("GET", networkRoute + "/entities/" + junctionId);
    check(item.status == 200 && item.json.value("type").toInt() == 9,
          "targeted HTTP entity read failed");
    check(client.call("GET", networkRoute + "/entities/" + QUuid::createUuid().toString(QUuid::WithoutBraces)).status == 404,
          "nonmember entity returned from network");
    check(client.call("GET", networkRoute + "?revision=" + rawHead).status == 404,
          "network appeared before initial save");
    check(client.call("GET", networkRoute + "?revision=" + createdHead).status == 200,
          "historical network read failed");

    junction.elevation_m = 456;
    const QString changed = QString::fromLatin1(native::ModelCodec<HydraulicNodeJunction>::encode(junction).toBase64());
    const QJsonObject update{{"uuid", junctionId}, {"type", 9}, {"payloadBase64", changed}};
    check(client.call("POST", networkRoute + "/edits",
          QJsonObject{{"expectedHead", createdHead}, {"changes", QJsonArray{QJsonObject{
              {"uuid", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"type", 9},
              {"payloadBase64", changed}}}}}).status == 400,
          "mismatching UUID payload accepted");
    const Response edited = client.call("POST", networkRoute + "/edits",
        QJsonObject{{"expectedHead", createdHead}, {"changes", QJsonArray{update}}});
    check(edited.status == 200, "atomic HTTP entity edit failed");
    const QString editedHead = edited.json.value("revision").toObject().value("id").toString();
    check(editedHead.size() == 64 && editedHead != createdHead,
          "entity edit did not publish a revision");
    check(client.call("POST", networkRoute + "/edits",
        QJsonObject{{"expectedHead", createdHead}, {"changes", QJsonArray{update}}}).status == 409,
          "stale entity edit accepted");
    const Response historical = client.call("GET", networkRoute + "/entities/" + junctionId +
                                            "?revision=" + createdHead);
    check(historical.status == 200 &&
          historical.json.value("payloadBase64").toString() == item.json.value("payloadBase64").toString(),
          "historical entity payload changed");
    const Response current = client.call("GET", networkRoute + "/entities/" + junctionId);
    check(current.status == 200 && current.json.value("payloadBase64").toString() == changed,
          "entity edit payload was not retained");
    const Response diff = client.call("GET", prefix + "/diff?from=" + createdHead + "&to=" + editedHead);
    check(diff.status == 200 && !diff.json.value("changes").toArray().isEmpty(),
          "revision diff omitted entity update");
    check(client.call("GET", prefix + "/diff?from=bad&to=" + editedHead).status == 400,
          "malformed revision diff accepted");
    check(client.call("GET", prefix + "/diff?from=" + QString(64, QChar('f')) +
                      "&to=" + editedHead).status == 404,
          "unknown diff base revision was not reported as missing");
    check(client.call("GET", networkRoute + "?revision=" + QString(64, QChar('f'))).status == 404,
          "unknown historical hydraulic revision was not reported as missing");

    const Response fork = client.call("POST", prefix + "/branches",
        QJsonObject{{"name", "http_feature"}, {"fromBranch", "main"}});
    check(fork.status == 201 && fork.json.value("id").toString() == editedHead,
          "hydraulic feature fork failed");
    const QString feature = prefix + "/branches/http_feature/networks/" + networkId;
    const Response unchanged = client.call("POST", feature + "/edits",
        QJsonObject{{"expectedHead", editedHead}, {"changes", QJsonArray{}}});
    check(unchanged.status == 200 &&
          unchanged.json.value("revision").toObject().value("id").toString() == editedHead,
          "empty edit batch changed revision");
    // An already incorporated feature must report up-to-date and not create a new revision.
    const Response merge = client.call("POST", prefix + "/merges",
        QJsonObject{{"target", "main"}, {"source", "http_feature"},
                    {"expectedTargetHead", editedHead}, {"expectedSourceHead", editedHead}});
    check(merge.status == 200 && merge.json.value("status").toString() == "up_to_date",
          "equal-tip hydraulic merge failed");
    check(client.call("POST", prefix + "/merges",
        QJsonObject{{"target", "main"}, {"source", "http_feature"},
                    {"expectedTargetHead", createdHead}, {"expectedSourceHead", editedHead}}).status == 409,
          "stale merge tip accepted");
    // Divergent edits to the same canonical field must remain conflicts until
    // a client explicitly selects one side. Neither branch moves on conflict.
    junction.elevation_m = 101;
    const QString targetPayload = QString::fromLatin1(
        native::ModelCodec<HydraulicNodeJunction>::encode(junction).toBase64());
    const Response targetEdit = client.call("POST", networkRoute + "/edits",
        QJsonObject{{"expectedHead", editedHead}, {"changes", QJsonArray{
            QJsonObject{{"uuid", junctionId}, {"type", 9}, {"payloadBase64", targetPayload}}}}});
    check(targetEdit.status == 200, "target-side divergent edit failed");
    const QString targetRevision = targetEdit.json.value("revision").toObject().value("id").toString();
    junction.elevation_m = 202;
    const QString sourcePayload = QString::fromLatin1(
        native::ModelCodec<HydraulicNodeJunction>::encode(junction).toBase64());
    const Response sourceEdit = client.call("POST", feature + "/edits",
        QJsonObject{{"expectedHead", editedHead}, {"changes", QJsonArray{
            QJsonObject{{"uuid", junctionId}, {"type", 9}, {"payloadBase64", sourcePayload}}}}});
    check(sourceEdit.status == 200, "source-side divergent edit failed");
    const QString sourceRevision = sourceEdit.json.value("revision").toObject().value("id").toString();
    const QJsonObject mergeRequest{{"target", "main"}, {"source", "http_feature"},
        {"expectedTargetHead", targetRevision}, {"expectedSourceHead", sourceRevision}};
    const Response conflicted = client.call("POST", prefix + "/merges", mergeRequest);
    check(conflicted.status == 409 &&
          conflicted.json.value("status").toString() == "conflicts" &&
          !conflicted.json.value("conflicts").toArray().isEmpty(),
          "divergent canonical field edit was not reported as a conflict");
    check(client.call("GET", main + "/head").json.value("id").toString() == targetRevision &&
          client.call("GET", prefix + "/branches/http_feature/head").json.value("id").toString() == sourceRevision,
          "conflicting merge unexpectedly advanced a branch");
    QJsonObject resolvedRequest = mergeRequest;
    resolvedRequest.insert("fieldChoices", QJsonArray{QJsonObject{
        {"entityUuid", junctionId}, {"field", "elevation_m"}, {"side", "source"}}});
    const Response resolved = client.call("POST", prefix + "/merges", resolvedRequest);
    check(resolved.status == 200 && resolved.json.value("status").toString() == "merged",
          "explicit field conflict resolution failed");
    check(resolved.json.value("revision").toObject().value("parents").toArray().size() == 2,
          "merge did not preserve both parents");
    const Response resolvedRead = client.call("GET", networkRoute + "/entities/" + junctionId);
    check(resolvedRead.status == 200 &&
          resolvedRead.json.value("payloadBase64").toString() == sourcePayload,
          "merge field choice was not applied");
    service.withRepository(projectId.toStdString(), [](native::Repository& repository) {
        (void)repository.verifyRepository();
    });
}
#endif

void exercise(aowis::native::PhysicalStore backend) {
    TemporaryRoot root;
    QString firstId;
    QString firstRevision;
    QPointer<QTcpServer> observedListener;
    {
        aowis::native::ProjectService service(root.root());
        QHttpServer server;
        // bind() reparents the listener to the HTTP server, which owns its lifetime.
        QTcpServer* tcp = new QTcpServer(&server);
        observedListener = tcp;
        aowis::native::http::registerRoutes(server, service, backend);
        check(tcp->listen(QHostAddress::LocalHost, 0) && server.bind(tcp), "unable to listen");
        check(tcp->parent() == &server, "HTTP listener must belong to the HTTP server");
        Client client(tcp->serverPort());
        check(client.call("GET", "/api/v1/health").status == 200, "health failed");
        check(client.call("GET", "/api/v1/ready").status == 200, "readiness failed");
        check(client.call("GET", "/api/v1/projects").json.value("projects").toArray().isEmpty(),
              "catalogue initially nonempty");
        check(client.call("POST", "/api/v1/projects", QJsonObject{{"name", ""}}).status == 400,
              "invalid create accepted");
        check(client.call("POST", "/api/v1/projects", QJsonObject{{"name", "X"}, {"evil", 42}}).status == 400,
              "unknown field accepted");
        const Response first = client.call("POST", "/api/v1/projects", QJsonObject{{"name", "Hydraulik Ü"}});
        check(first.status == 201, "create project failed");
        firstId = first.json.value("id").toString();
        check(first.json.value("name").toString() == "Hydraulik Ü", "project name wrong");
        check(first.json.value("storage").toString() ==
            (backend == aowis::native::PhysicalStore::Stage7Pack ? "pack" : "blocks"),
            "incorrect selected backend");
        const Response second = client.call("POST", "/api/v1/projects", QJsonObject{{"name", "Independent"}});
        check(second.status == 201 && second.json.value("id").toString() != firstId,
              "second project creation failed");
        check(client.call("GET", "/api/v1/projects").json.value("projects").toArray().size() == 2,
              "project listing incorrect");
        check(client.call("GET", "/api/v1/projects/invalid-id").status == 400,
              "invalid project UUID was accepted");
        check(client.call("GET", "/api/v1/projects/00000000-0000-4000-8000-000000000000").status == 404,
              "unknown project accepted");
        check(client.call("GET", "/api/v1/projects/" + firstId).status == 200,
              "project details failed");
        const QString prefix = "/api/v1/projects/" + firstId;
        check(client.call("GET", prefix + "/branches").json.value("branches").toArray().size() == 1,
              "initial branch missing");
        const Response head = client.call("GET", prefix + "/branches/main/head");
        check(head.status == 200, "head request failed");
        firstRevision = head.json.value("id").toString();
        check(firstRevision.size() == 64, "head hash malformed");
        check(client.call("GET", prefix + "/branches/main/history?limit=0").status == 400,
              "invalid history limit accepted");
        check(client.call("GET", prefix + "/branches/main/history?limit=1").json.value("revisions").toArray().size() == 1,
              "history read failed");
        check(client.call("GET", prefix + "/branches/main/history?after=" + firstRevision)
                   .json.value("revisions").toArray().isEmpty(), "exclusive cursor failed");
        check(client.call("GET", prefix + "/revisions/" + firstRevision).status == 200,
              "immutable revision read failed");
        check(client.call("GET", prefix + "/revisions/garbage").status == 400,
              "invalid revision hash accepted");
        check(client.call("GET", prefix + "/revisions/" + QString(64, QChar('f'))).status == 404,
              "unknown revision should be 404");
        check(client.call("GET", prefix + "/branches/unknown/head").status == 404,
              "unknown branch accepted");
        const Response fork = client.call("POST", prefix + "/branches",
            QJsonObject{{"name", "design"}, {"fromBranch", "main"}, {"revision", firstRevision}});
        check(fork.status == 201 && fork.json.value("id").toString() == firstRevision,
              "pinned fork failed");
        check(client.call("POST", prefix + "/branches", QJsonObject{{"name", "design"}}).status == 409,
              "duplicate branch accepted");
        check(client.call("POST", prefix + "/branches", QJsonObject{{"name", "../bad"}}).status == 400,
              "malformed branch name accepted");
        check(client.call("POST", prefix + "/branches", QJsonObject{{"name", "missing-source"}, {"fromBranch", "missing"}}).status == 404,
              "missing source branch should be 404");
        check(client.call("POST", prefix + "/branches", QJsonObject{{"name", "bad"}, {"revision", firstRevision}}).status == 400,
              "fork without source accepted");
        check(client.call("POST", prefix + "/branches", QJsonObject{{"name", "scratch"}}).status == 201,
              "empty branch creation failed");
        check(client.call("GET", prefix + "/branches").json.value("branches").toArray().size() == 3,
              "branch listing wrong");
        check(client.call("GET", "/api/v1/projects/" + second.json.value("id").toString() + "/branches")
                   .json.value("branches").toArray().size() == 1, "branch leaked to another project");
#ifdef AOWIS_HTTP_WITH_HYDRAULIC
        exerciseHydraulicApi(client, service, firstId, second.json.value("id").toString(), firstRevision);
#endif
    }
    check(observedListener.isNull(), "HTTP server did not destroy its owned listener");
    {
        aowis::native::ProjectService reopened(root.root());
        check(reopened.project(firstId.toStdString()).name == "Hydraulik Ü", "restart lost project name");
        check(aowis::native::toHex(reopened.head(firstId.toStdString(), "design").id) ==
              firstRevision.toStdString(), "restart lost fork revision");
    }
}

// Exercise the real executable's startup/shutdown path, not just its route handlers.
// A listener incorrectly allocated on the stack crashes during a clean shutdown.
void exerciseServerShutdown(const QString& executable, const QString& backend) {
    TemporaryRoot root;
    QTcpServer reservation;
    check(reservation.listen(QHostAddress::LocalHost, 0), "unable to reserve test port");
    const quint16 port = reservation.serverPort();
    reservation.close();

    QProcess process;
    process.setProgram(executable);
    process.setArguments({QStringLiteral("--bind"), QStringLiteral("127.0.0.1"),
                          QStringLiteral("--port"), QString::number(port),
                          QStringLiteral("--data-root"), QString::fromStdString(root.root().string()),
                          QStringLiteral("--backend"), backend});
    process.start();
    try {
        check(process.waitForStarted(5000), "database HTTP executable did not start");
        QElapsedTimer elapsed;
        elapsed.start();
        bool listening = false;
        while (elapsed.elapsed() < 5000 && process.state() != QProcess::NotRunning) {
            QTcpSocket probe;
            probe.connectToHost(QHostAddress::LocalHost, port);
            if (probe.waitForConnected(100)) {
                listening = true;
                probe.disconnectFromHost();
                break;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        check(listening, "database HTTP executable did not accept connections");
        Client client(port);
        check(client.call("GET", "/api/v1/health").status == 200,
              "database HTTP executable health endpoint failed");
        const Response created = client.call("POST", "/api/v1/projects",
                                              QJsonObject{{"name", "Shutdown test"}});
        check(created.status == 201 && created.json.value("storage").toString() == backend,
              "database HTTP executable did not create a project on the selected backend");
        process.terminate();
        check(process.waitForFinished(5000), "database HTTP executable did not shut down");
        check(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
              "database HTTP executable crashed during shutdown");
    } catch (...) {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(3000);
        }
        throw;
    }
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        check(argc == 2, "expected path to aowis-server-db executable");
        exercise(aowis::native::PhysicalStore::Stage7Pack);
        exercise(aowis::native::PhysicalStore::ExperimentalBlocks);
        exerciseServerShutdown(QString::fromLocal8Bit(argv[1]), QStringLiteral("pack"));
        exerciseServerShutdown(QString::fromLocal8Bit(argv[1]), QStringLiteral("blocks"));
        std::cout << "HTTP project service tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HTTP service test failed: " << error.what() << '\n';
        return 1;
    }
}
