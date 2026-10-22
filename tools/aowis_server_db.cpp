#include "aowis/native/db_http_routes.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QHostAddress>
#include <QHttpServer>
#include <QSettings>
#include <QTcpServer>
#include <QTimer>

#include <fstream>
#include <csignal>
#include <streambuf>
#include <stdexcept>
#include <exception>
#include <iostream>

namespace {
volatile std::sig_atomic_t shutdownRequested = 0;
void requestShutdown(int) { shutdownRequested = 1; }

class ScopedErrorLog {
public:
    explicit ScopedErrorLog(const QString& path) {
        if (path.isEmpty()) return;
        this->stream_.open(path.toStdString(), std::ios::app);
        if (!this->stream_.good()) throw std::runtime_error("unable to open configured log file");
        this->previous_ = std::cerr.rdbuf(this->stream_.rdbuf());
    }
    ~ScopedErrorLog() {
        if (this->previous_ != nullptr) std::cerr.rdbuf(this->previous_);
    }
    ScopedErrorLog(const ScopedErrorLog&) = delete;
    ScopedErrorLog& operator=(const ScopedErrorLog&) = delete;
private:
    std::ofstream stream_;
    std::streambuf* previous_ = nullptr;
};
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("aowis-server-db"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("AOWIS multi-project native database HTTP service"));
    parser.addHelpOption();
    const QCommandLineOption config({"c", "config"}, "INI configuration file", "file");
    const QCommandLineOption bind({"b", "bind"}, "TCP bind address", "address");
    const QCommandLineOption port({"p", "port"}, "TCP port (1..65535)", "port");
    const QCommandLineOption dataRoot({"d", "data-root"}, "Project data directory", "directory");
    const QCommandLineOption backend("backend", "Default project backend: pack or blocks", "backend");
    const QCommandLineOption logFile("log-file", "Append server errors to a log file", "file");
    parser.addOption(config);
    parser.addOption(bind);
    parser.addOption(port);
    parser.addOption(dataRoot);
    parser.addOption(backend);
    parser.addOption(logFile);
    parser.process(app);

    try {
        QSettings settings(parser.isSet(config) ? parser.value(config) :
                           QStringLiteral("aowis-server-db.ini"), QSettings::IniFormat);
        const QString bindValue = parser.isSet(bind) ? parser.value(bind) :
            settings.value("server/bind", "127.0.0.1").toString();
        const QString portValue = parser.isSet(port) ? parser.value(port) :
            settings.value("server/port", 8755).toString();
        const QString rootValue = parser.isSet(dataRoot) ? parser.value(dataRoot) :
            settings.value("storage/root", "./aowis-db-data").toString();
        const QString storageValue = parser.isSet(backend) ? parser.value(backend) :
            settings.value("storage/default_backend", "pack").toString();
        const QString logPath = parser.isSet(logFile) ? parser.value(logFile) :
            settings.value("logging/file", "").toString();

        QHostAddress address;
        bool validPort = false;
        const unsigned requestedPort = portValue.toUInt(&validPort);
        if (!address.setAddress(bindValue) || !validPort || requestedPort == 0 || requestedPort > 65535 ||
            (storageValue != "pack" && storageValue != "blocks") || rootValue.trimmed().isEmpty()) {
            std::cerr << "Invalid bind address, port, data root or storage backend configuration\n";
            return 2;
        }
        const aowis::native::PhysicalStore storage = storageValue == "pack" ?
            aowis::native::PhysicalStore::Stage7Pack : aowis::native::PhysicalStore::ExperimentalBlocks;
        // Default: errors go to stderr/journald. Optional file logging
        // redirects the C++ service diagnostics, restoring stderr on exit.
        ScopedErrorLog errorLog(logPath);
        aowis::native::ProjectService service(rootValue.toStdString());
        QHttpServer httpServer;
        aowis::native::http::registerRoutes(httpServer, service, storage);
        // QHttpServer::bind() takes QObject ownership of the listener.
        // Never bind a stack-allocated QTcpServer: the HTTP server destroys its children.
        QTcpServer* tcpServer = new QTcpServer(&httpServer);
        if (!tcpServer->listen(address, static_cast<quint16>(requestedPort)) ||
            !httpServer.bind(tcpServer)) {
            std::cerr << "Unable to bind HTTP listener: " << tcpServer->errorString().toStdString() << '\n';
            return 1;
        }
        std::signal(SIGINT, requestShutdown);
        std::signal(SIGTERM, requestShutdown);
        QTimer shutdownCheck;
        QObject::connect(&shutdownCheck, &QTimer::timeout, &app, [&app]() {
            if (shutdownRequested != 0) app.quit();
        });
        shutdownCheck.start(150);
        std::cout << "AOWIS database listening on " << tcpServer->serverAddress().toString().toStdString()
                  << ':' << tcpServer->serverPort() << std::endl;
        return app.exec();
    } catch (const std::exception& error) {
        std::cerr << "AOWIS database server startup failed: " << error.what() << '\n';
        return 1;
    }
}
