#include "aowis/native/db_http_routes.h"

#include <httplib.h>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QSettings>

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <iostream>
#include <pthread.h>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>

namespace {
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

// Numeric IPv4 or IPv6 literal only: host names would make the listen
// address depend on name resolution at startup.
bool numericAddress(const std::string& address) {
    unsigned char buffer[sizeof(in6_addr)]{};
    return ::inet_pton(AF_INET, address.c_str(), buffer) == 1 ||
           ::inet_pton(AF_INET6, address.c_str(), buffer) == 1;
}

// SIGINT/SIGTERM are blocked in every thread and received synchronously here;
// the watcher stops the server, whose listen loop then returns in main().
class ShutdownWatcher {
public:
    explicit ShutdownWatcher(httplib::Server& server) : server_(server) {
        this->thread_ = std::thread([this]() { this->run(); });
    }
    ~ShutdownWatcher() {
        this->finished_.store(true);
        if (this->thread_.joinable()) this->thread_.join();
    }
    ShutdownWatcher(const ShutdownWatcher&) = delete;
    ShutdownWatcher& operator=(const ShutdownWatcher&) = delete;
private:
    void run() {
        sigset_t shutdownSet;
        sigemptyset(&shutdownSet);
        sigaddset(&shutdownSet, SIGINT);
        sigaddset(&shutdownSet, SIGTERM);
        const timespec interval{0, 200 * 1000 * 1000};
        while (!this->finished_.load()) {
            if (::sigtimedwait(&shutdownSet, nullptr, &interval) > 0) {
                this->server_.stop();
                return;
            }
        }
    }
    httplib::Server& server_;
    std::atomic<bool> finished_{false};
    std::thread thread_;
};
} // namespace

int main(int argc, char** argv) {
    // Block shutdown signals before any thread exists, so every worker
    // inherits the mask and only the watcher receives them.
    sigset_t shutdownSignals;
    sigemptyset(&shutdownSignals);
    sigaddset(&shutdownSignals, SIGINT);
    sigaddset(&shutdownSignals, SIGTERM);
    ::pthread_sigmask(SIG_BLOCK, &shutdownSignals, nullptr);
    std::signal(SIGPIPE, SIG_IGN); // a closed client socket must not kill the server

    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("aowis-server-db"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("AOWIS multi-project native database HTTP service"));
    parser.addHelpOption();
    const QCommandLineOption config({"c", "config"}, "INI configuration file", "file");
    const QCommandLineOption bind({"b", "bind"}, "TCP bind address (numeric IPv4/IPv6)", "address");
    const QCommandLineOption port({"p", "port"}, "TCP port (1..65535)", "port");
    const QCommandLineOption dataRoot({"d", "data-root"}, "Project data directory", "directory");
    const QCommandLineOption logFile("log-file", "Append server errors to a log file", "file");
    const QCommandLineOption workers("worker-threads", "HTTP worker threads (1..256)", "count");
    parser.addOption(config);
    parser.addOption(bind);
    parser.addOption(port);
    parser.addOption(dataRoot);
    parser.addOption(logFile);
    parser.addOption(workers);
    const QCommandLineOption bundleMib("sync-max-mib", "Maximum transfer size in MiB (1..256)", "MiB");
    const QCommandLineOption bundleObjects("sync-max-objects", "Maximum objects in a bundle (1..1000000)", "count");
    const QCommandLineOption objectMib("sync-max-object-mib", "Maximum object size in MiB (1..64)", "MiB");
    parser.addOption(bundleMib);
    parser.addOption(bundleObjects);
    parser.addOption(objectMib);
    parser.process(app);

    try {
        QSettings settings(parser.isSet(config) ? parser.value(config) :
                           QStringLiteral("aowis-server-db.ini"), QSettings::IniFormat);
        const QString bindValue = parser.isSet(bind) ? parser.value(bind) :
            settings.value("server/bind", "127.0.0.1").toString();
        const QString portValue = parser.isSet(port) ? parser.value(port) :
            settings.value("server/port", 8755).toString();
        const QString workerValue = parser.isSet(workers) ? parser.value(workers) :
            settings.value("server/worker_threads", 8).toString();
        const QString rootValue = parser.isSet(dataRoot) ? parser.value(dataRoot) :
            settings.value("storage/root", "./aowis-db-data").toString();
        const QString storageValue = settings.value("storage/default_backend", "pack").toString();
        const QString logPath = parser.isSet(logFile) ? parser.value(logFile) :
            settings.value("logging/file", "").toString();

        const QString transferMibValue = parser.isSet(bundleMib) ? parser.value(bundleMib) :
            settings.value("sync/max_transfer_mib", 32).toString();
        const QString countValue = parser.isSet(bundleObjects) ? parser.value(bundleObjects) :
            settings.value("sync/max_objects", 100000).toString();
        const QString objectMibValue = parser.isSet(objectMib) ? parser.value(objectMib) :
            settings.value("sync/max_object_mib", 8).toString();
        bool validTransfer = false;
        bool validCount = false;
        bool validObject = false;
        const unsigned transferMib = transferMibValue.toUInt(&validTransfer);
        const unsigned count = countValue.toUInt(&validCount);
        const unsigned individualMib = objectMibValue.toUInt(&validObject);
        if (!validTransfer || !validCount || !validObject || transferMib == 0 || transferMib > 256 ||
            count == 0 || count > 1000000 || individualMib == 0 || individualMib > 64 ||
            individualMib > transferMib) {
            std::cerr << "Invalid synchronization transfer limits\n";
            return 2;
        }
        const aowis::native::BundleLimits limits{
            static_cast<std::uint64_t>(transferMib) * 1024ULL * 1024ULL,
            static_cast<std::uint64_t>(count),
            static_cast<std::uint64_t>(individualMib) * 1024ULL * 1024ULL};
        bool validPort = false;
        bool validWorkers = false;
        const unsigned requestedPort = portValue.toUInt(&validPort);
        const unsigned workerCount = workerValue.toUInt(&validWorkers);
        const std::string address = bindValue.toStdString();
        if (!numericAddress(address) || !validPort || requestedPort == 0 || requestedPort > 65535 ||
            !validWorkers || workerCount == 0 || workerCount > 256 ||
            (storageValue != "pack") || rootValue.trimmed().isEmpty()) {
            std::cerr << "Invalid bind address, port, worker threads, data root or storage backend "
                         "configuration\n";
            return 2;
        }
        // Default: errors go to stderr/journald. Optional file logging
        // redirects the C++ service diagnostics, restoring stderr on exit.
        ScopedErrorLog errorLog(logPath);
        aowis::native::ProjectService service(rootValue.toStdString());
        httplib::Server server;
        // Fixed pool; at most 16 waiting connections per worker are queued,
        // further connections are closed instead of piling up.
        server.new_task_queue = [workerCount]() {
            const std::size_t threads = static_cast<std::size_t>(workerCount);
            return new httplib::ThreadPool(threads, threads, threads * 16U);
        };
        aowis::native::http::registerRoutes(server, service, aowis::native::PhysicalStore::Pack, limits);
        if (!server.bind_to_port(address, static_cast<int>(requestedPort))) {
            std::cerr << "Unable to bind HTTP listener on " << address << ':' << requestedPort
                      << ": " << std::strerror(errno) << '\n';
            return 1;
        }
        std::cout << "AOWIS database listening on " << address << ':' << requestedPort << std::endl;
        ShutdownWatcher watcher(server);
        const bool listened = server.listen_after_bind();
        return listened ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "AOWIS database server startup failed: " << error.what() << '\n';
        return 1;
    }
}
