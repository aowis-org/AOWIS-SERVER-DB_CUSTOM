#include "aowis/native/db_http_routes.h"

#include <QCoreApplication>
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
