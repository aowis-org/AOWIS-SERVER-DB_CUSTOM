#include "aowis/native/db_http_routes.h"
#ifdef AOWIS_HTTP_WITH_HYDRAULIC
#include "aowis/native/hydraulic_network_persistence.h"
#endif

#include <httplib.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStringList>
#include <QUuid>

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>

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
    QByteArray retryAfter;
};
struct BinaryResponse {
    int status = 0;
    QByteArray body;
    QByteArray contentType;
    QByteArray retryAfter;
};

// Raw TCP connection to 127.0.0.1 for protocol-level tests (partial bodies,
// declared lengths without bodies, slow clients). Closes on destruction.
class RawConnection {
public:
    explicit RawConnection(quint16 port) {
        this->fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        check(this->fd_ >= 0, "socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        this->connected_ = ::connect(this->fd_, reinterpret_cast<sockaddr*>(&address),
                                     sizeof(address)) == 0;
    }
    ~RawConnection() { if (this->fd_ >= 0) ::close(this->fd_); }
    RawConnection(const RawConnection&) = delete;
    RawConnection& operator=(const RawConnection&) = delete;
    bool connected() const { return this->connected_; }
    void send(const QByteArray& bytes) {
        qsizetype done = 0;
        while (done < bytes.size()) {
            const ssize_t sent = ::send(this->fd_, bytes.constData() + done,
                                        static_cast<std::size_t>(bytes.size() - done), MSG_NOSIGNAL);
            check(sent > 0, "raw send failed");
            done += sent;
        }
    }
    // Everything the server sends until it closes the connection or the
    // timeout expires (whichever comes first).
    QByteArray receive(int timeoutMs) {
        QByteArray received;
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            const long remaining = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count());
            if (remaining <= 0) break;
            pollfd descriptor{this->fd_, POLLIN, 0};
            if (::poll(&descriptor, 1, static_cast<int>(remaining)) <= 0) break;
            char buffer[4096];
            const ssize_t count = ::recv(this->fd_, buffer, sizeof(buffer), 0);
            if (count <= 0) { this->closedByPeer_ = true; break; }
            received.append(buffer, count);
        }
        return received;
    }
    bool closedByPeer() const { return this->closedByPeer_; }
    void close() {
        if (this->fd_ >= 0) ::close(this->fd_);
        this->fd_ = -1;
    }
private:
    int fd_ = -1;
    bool connected_ = false;
    bool closedByPeer_ = false;
};

int statusOf(const QByteArray& response) {
    // "HTTP/1.1 413 Payload Too Large"
    if (!response.startsWith("HTTP/1.")) return 0;
    return response.mid(9, 3).toInt();
}

// An ephemeral loopback port that is free right now.
quint16 reservePort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(fd >= 0, "socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    const bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                    ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    ::close(fd);
    check(ok, "unable to reserve test port");
    return ntohs(address.sin_port);
}

class Client {
public:
    explicit Client(quint16 port) : client_("127.0.0.1", port), port_(port) {
        this->client_.set_connection_timeout(std::chrono::seconds(5));
        this->client_.set_read_timeout(std::chrono::seconds(30));
        this->client_.set_write_timeout(std::chrono::seconds(30));
    }
    quint16 port() const { return this->port_; }
    Response call(const QByteArray& verb, const QString& path, const QJsonObject& body = {}) {
        const BinaryResponse reply = callRaw(verb, path,
            QJsonDocument(body).toJson(QJsonDocument::Compact), "application/json");
        const QJsonDocument json = QJsonDocument::fromJson(reply.body);
        check(json.isObject(), "HTTP response is not JSON");
        return Response{reply.status, json.object(), reply.retryAfter};
    }
    BinaryResponse callRaw(const QByteArray& verb, const QString& path,
                           const QByteArray& bytes = QByteArray(),
                           const QByteArray& mime = "application/vnd.aowis.bundle") {
        const std::string target = path.toStdString();
        httplib::Result result = verb == "GET"
            ? this->client_.Get(target)
            : this->client_.Post(target, std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())),
                                 mime.toStdString());
        if (!result) {
            throw std::runtime_error("HTTP request failed: " + httplib::to_string(result.error()) +
                                     " (" + verb.toStdString() + ' ' + target + ')');
        }
        return BinaryResponse{result->status,
                              QByteArray(result->body.data(), static_cast<qsizetype>(result->body.size())),
                              QByteArray::fromStdString(result->get_header_value("Content-Type")),
                              QByteArray::fromStdString(result->get_header_value("Retry-After"))};
    }
    // Abort a bundle response after its first bytes. The server must release
    // the response's scratch file even on disconnect.
    void abortDownload(const QString& path) {
        bool receivedBody = false;
        const httplib::Result result = this->client_.Get(path.toStdString(),
            [&](const char*, std::size_t length) {
                receivedBody = length > 0;
                return false; // abort
            });
        check(!result && receivedBody, "streamed download did not produce bytes before abort");
    }
private:
    httplib::Client client_;
    quint16 port_;
};

// In-process server on a loopback port, serving from worker threads.
class TestServer {
public:
    TestServer(aowis::native::ProjectService& service, aowis::native::BundleLimits limits) {
        this->server_.new_task_queue = [] { return new httplib::ThreadPool(8, 8, 128); };
        aowis::native::http::registerRoutes(this->server_, service,
                                            aowis::native::PhysicalStore::Pack, limits);
        const int bound = this->server_.bind_to_any_port("127.0.0.1");
        check(bound > 0, "unable to listen");
        this->port_ = static_cast<quint16>(bound);
        this->thread_ = std::thread([this] { this->server_.listen_after_bind(); });
        this->server_.wait_until_ready();
    }
    ~TestServer() { this->stop(); }
    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;
    quint16 port() const { return this->port_; }
    void stop() {
        this->server_.stop();
        if (this->thread_.joinable()) this->thread_.join();
    }
private:
    httplib::Server server_;
    std::thread thread_;
    quint16 port_ = 0;
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
    // Typed errors (review F2): a UUID owned by another model entity is not a
    // network (404, formerly 500); editing a missing network is 404 (formerly 400).
    check(client.call("GET", main + "/networks/" + junctionId).status == 404,
          "non-network UUID was not reported as a missing network");
    const Response missingEdit = client.call("POST", main + "/networks/" +
        QUuid::createUuid().toString(QUuid::WithoutBraces) + "/edits",
        QJsonObject{{"expectedHead", createdHead},
                    {"changes", QJsonArray{QJsonObject{{"uuid", junctionId}, {"delete", true}}}}});
    check(missingEdit.status == 404 &&
          missingEdit.json.value("error").toObject().value("code").toString() == "not_found",
          "edit of a missing network was not 404 not_found");

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

// Real TCP bundle transfer: full/incremental between independent repositories, corrupt/truncated,
// stale/divergent branches, transfer limits, and idempotent retry.
void exerciseBundleApi(Client& client, aowis::native::ProjectService& service,
                       const QString& sourceId, aowis::native::PhysicalStore backend) {
    namespace native = aowis::native;
    const QString recipientFormat = QStringLiteral("pack");
    const Response destination = client.call("POST", "/api/v1/projects",
        QJsonObject{{"name", "Replication recipient"}, {"storage", recipientFormat}});
    check(destination.status == 201, "destination creation failed");
    const QString destinationId = destination.json.value("id").toString();
    const QString from = QStringLiteral("/api/v1/projects/") + sourceId + "/branches/sync_demo";
    const QString to = QStringLiteral("/api/v1/projects/") + destinationId + "/branches/tracking";
    native::Key item{};
    item[15] = 37;
    native::Key largeItem{};
    largeItem[15] = 38;
    native::Bytes largePayload(512 * 1024, static_cast<std::uint8_t>(0x5c));
    service.withRepository(sourceId.toStdString(), [&](native::Repository& repository) {
        repository.createBranch("sync_demo");
        (void)repository.commit("sync_demo", {{item, native::Bytes{'o', 'n', 'e'}},
                                               {largeItem, largePayload}});
    });
    const QString firstHead = client.call("GET", from + "/head").json.value("id").toString();
    const BinaryResponse bundle = client.callRaw("GET", from + "/bundle");
    check(bundle.status == 200 && bundle.body.size() > 512 * 1024 &&
          bundle.contentType.startsWith("application/vnd.aowis.bundle"),
          "large file-backed bundle download failed");
    check(client.callRaw("GET", from + "/bundle").body == bundle.body,
          "repeated file-backed bundle export changed bytes");
    client.abortDownload(from + "/bundle");
    check(client.callRaw("GET", from + "/bundle").body == bundle.body,
          "download after client cancellation changed bytes");
    check(client.callRaw("GET", from + "/bundle?base=invalid").status == 400,
          "malformed base hash accepted");
    check(client.callRaw("GET", from + "/bundle?base=" + QString(64, QChar('f'))).status == 409,
          "unknown export base accepted");
    check(client.callRaw("GET", from + "/bundle?unexpected=1").status == 400,
          "unknown bundle query argument accepted");
    check(client.callRaw("GET", "/api/v1/projects/invalid/branches/sync_demo/bundle").status == 400,
          "invalid project bundle URL accepted");
    check(client.callRaw("POST", to + "/bundle", QByteArray()).status == 400,
          "empty bundle import accepted");
    check(client.callRaw("POST", to + "/bundle", QByteArray(1024 * 1024 + 1, 'x')).status == 413,
          "oversized bundle upload accepted");
    // Partial request body followed by disconnect must never invoke bundle publication.
    {
        RawConnection interrupted(client.port());
        check(interrupted.connected(), "unable to connect interrupted upload");
        const QByteArray head = "POST " + to.toUtf8() +
            "/bundle HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/vnd.aowis.bundle\r\n" +
            "Content-Length: " + QByteArray::number(bundle.body.size()) + "\r\n\r\n";
        interrupted.send(head + bundle.body.left(bundle.body.size() / 2));
        interrupted.close();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    check(client.call("GET", to + "/head").status == 404,
          "disconnected partial request published a branch");
    check(client.callRaw("POST", to + "/bundle", bundle.body.left(bundle.body.size() - 1)).status == 400,
          "truncated bundle accepted");
    QByteArray corrupted = bundle.body;
    corrupted[corrupted.size() - 1] = static_cast<char>(corrupted.back() ^ 1);
    check(client.callRaw("POST", to + "/bundle", corrupted).status == 400,
          "corrupted bundle accepted");
    check(client.call("GET", to + "/head").status == 404,
          "invalid bundle published a branch");
    const Response firstImport = client.call("POST", to + "/bundle", QJsonObject{});
    check(firstImport.status == 400, "JSON is not a valid binary bundle");
    const BinaryResponse imported = client.callRaw("POST", to + "/bundle", bundle.body);
    const QJsonObject first = QJsonDocument::fromJson(imported.body).object();
    check(imported.status == 201 && first.value("status").toString() == QStringLiteral("created") &&
          first.value("head").toString() == firstHead &&
          first.value("objectsImported").toInteger() > 0,
          "initial bundle import failed");
    const BinaryResponse repeated = client.callRaw("POST", to + "/bundle", bundle.body);
    const QJsonObject repeatJson = QJsonDocument::fromJson(repeated.body).object();
    check(repeated.status == 200 && repeatJson.value("status").toString() == QStringLiteral("up_to_date") &&
          repeatJson.value("objectsImported").toInteger() == 0,
          "repeated bundle import was not idempotent");
    service.withRepository(sourceId.toStdString(), [&](native::Repository& repository) {
        (void)repository.commit("sync_demo", {{item, native::Bytes{'t', 'w', 'o'}}});
    });
    const QString nextHead = client.call("GET", from + "/head").json.value("id").toString();
    const BinaryResponse delta = client.callRaw("GET", from + "/bundle?base=" + firstHead);
    check(delta.status == 200 && delta.body.startsWith("AOWSYNC2"),
          "incremental binary bundle export failed");
    const BinaryResponse advanced = client.callRaw("POST", to + "/bundle", delta.body);
    const QJsonObject advancedJson = QJsonDocument::fromJson(advanced.body).object();
    check(advanced.status == 200 && advancedJson.value("status").toString() == QStringLiteral("fast_forward") &&
          advancedJson.value("head").toString() == nextHead,
          "incremental import failed");
    check(client.call("GET", to + "/head").json.value("id").toString() == nextHead,
          "destination branch did not fast-forward");
    service.withRepository(destinationId.toStdString(), [&](native::Repository& repository) {
        (void)repository.commit("tracking", {{item, native::Bytes{'d', 'e', 's', 't'}}});
    });
    const QString divergentHead = client.call("GET", to + "/head").json.value("id").toString();
    service.withRepository(sourceId.toStdString(), [&](native::Repository& repository) {
        (void)repository.commit("sync_demo", {{item, native::Bytes{'n', 'e', 'x', 't'}}});
    });
    const BinaryResponse more = client.callRaw("GET", from + "/bundle?base=" + nextHead);
    check(more.status == 200, "follow-up incremental export failed");
    check(client.callRaw("POST", to + "/bundle", more.body).status == 409,
          "divergent tracking history was overwritten");
    check(client.call("GET", to + "/head").json.value("id").toString() == divergentHead,
          "rejected bundle mutated divergent branch");
    service.withRepository(destinationId.toStdString(), [&](native::Repository& repository) {
        (void)repository.verifyRepository();
    });
}

// Review F2/F10b: classification by StorageErrorKind, never message text.
void exerciseErrorClassification(Client& client, aowis::native::ProjectService& service,
                                 const std::filesystem::path& root) {
    namespace native = aowis::native;
    const Response created = client.call("POST", "/api/v1/projects", QJsonObject{{"name", "Errors"}});
    check(created.status == 201, "error-classification project creation failed");
    const QString id = created.json.value("id").toString();
    const QString prefix = "/api/v1/projects/" + id;
    native::Revision committed;
    service.withRepository(id.toStdString(), [&](native::Repository& repository) {
        native::Key key{};
        key[15] = 7;
        committed = repository.commit("main", {native::Change{key, native::Bytes{1, 2, 3}}});
    });
    // A tree hash is an existing object, but not a revision: 404, not 500.
    const Response tree = client.call("GET", prefix + "/revisions/" +
                                      QString::fromStdString(native::toHex(committed.root)));
    check(tree.status == 404 &&
          tree.json.value("error").toObject().value("code").toString() == "not_found",
          "non-revision object hash was not reported as 404 not_found");
    const QString to = QString::fromStdString(native::toHex(committed.id));
    check(client.call("GET", prefix + "/diff?from=" +
                      QString::fromStdString(native::toHex(committed.root)) + "&to=" + to).status == 404,
          "diff accepted a non-revision object hash");

    // Damaged local storage is a server failure: 500 with a generic message
    // and no internal details (checksums, paths, errno text) in the response.
    const std::filesystem::path pack = root / "projects" / id.toStdString() / "repository" / "objects.pack";
    {
        std::fstream stream(pack, std::ios::in | std::ios::out | std::ios::binary);
        check(stream.good(), "cannot open project pack for corruption test");
        stream.seekg(50);
        char byte = 0;
        stream.read(&byte, 1);
        byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0xffU);
        stream.seekp(50);
        stream.write(&byte, 1);
        check(stream.good(), "cannot corrupt project pack");
    }
    const QString initial = QString::fromStdString(native::toHex(committed.parents.front()));
    const BinaryResponse damaged = client.callRaw("GET", prefix + "/revisions/" + initial);
    const QJsonObject error = QJsonDocument::fromJson(damaged.body).object().value("error").toObject();
    check(damaged.status == 500 && error.value("code").toString() == "internal_error" &&
          error.value("message").toString() == "database operation failed",
          "storage corruption was not reported as a generic 500");
    check(!damaged.body.contains("checksum") && !damaged.body.contains("pack"),
          "storage corruption details leaked to the HTTP client");
}

// Review F9/F10a: list-like responses are bounded before materialization, and
// branch-scoped routes only serve revisions of that branch.
void exerciseBoundsAndScoping(Client& client, aowis::native::ProjectService& service) {
    namespace native = aowis::native;
    const Response created = client.call("POST", "/api/v1/projects", QJsonObject{{"name", "Bounds"}});
    check(created.status == 201, "bounds project creation failed");
    const QString id = created.json.value("id").toString();
    const QString prefix = "/api/v1/projects/" + id;
    native::Hash empty{};
    native::Hash exactly1000{};
    native::Hash over1000{};
    native::Hash mainOnly{};
    native::Hash sideOnly{};
    service.withRepository(id.toStdString(), [&](native::Repository& repository) {
        empty = repository.head("main").id;
        std::vector<native::Change> changes;
        for (unsigned i = 0; i < 1000; ++i) {
            native::Key key{};
            key[0] = static_cast<std::uint8_t>(i * 37U);
            key[14] = static_cast<std::uint8_t>(i >> 8U);
            key[15] = static_cast<std::uint8_t>(i);
            changes.push_back(native::Change{key, native::Bytes{1}});
        }
        repository.createBranch("bulk", "main");
        exactly1000 = repository.commit("bulk", changes).id;
        native::Key extra{};
        extra[0] = 0xfe;
        extra[15] = 0xfe;
        over1000 = repository.commit("bulk", {native::Change{extra, native::Bytes{2}}}).id;
        native::Key a{};
        a[15] = 1;
        mainOnly = repository.commit("main", {native::Change{a, native::Bytes{3}}}).id;
        repository.createBranch("side", "main");
        native::Key b{};
        b[15] = 2;
        sideOnly = repository.commit("side", {native::Change{b, native::Bytes{4}}}).id;
    });
    const QString hex0 = QString::fromStdString(native::toHex(empty));
    const Response fits = client.call("GET", prefix + "/diff?from=" + hex0 + "&to=" +
                                      QString::fromStdString(native::toHex(exactly1000)));
    check(fits.status == 200 && fits.json.value("changes").toArray().size() == 1000,
          "diff with exactly 1000 records was not returned");
    const Response tooLarge = client.call("GET", prefix + "/diff?from=" + hex0 + "&to=" +
                                          QString::fromStdString(native::toHex(over1000)));
    check(tooLarge.status == 413 &&
          tooLarge.json.value("error").toObject().value("code").toString() == "limit_exceeded",
          "diff with 1001 records was not rejected with 413 limit_exceeded");

#ifdef AOWIS_HTTP_WITH_HYDRAULIC
    // Branch scoping is checked before any model access: a revision of another
    // branch is 404; the branch's own ancestor passes scoping and then fails the
    // (raw) model check with 400.
    const QString anyNetwork = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString side = QString::fromStdString(native::toHex(sideOnly));
    const QString main = QString::fromStdString(native::toHex(mainOnly));
    const Response foreign = client.call("GET", prefix + "/branches/main/networks/" + anyNetwork +
                                         "?revision=" + side);
    check(foreign.status == 404 &&
          foreign.json.value("error").toObject().value("code").toString() == "not_found",
          "revision of another branch was served on a branch-scoped route");
    check(client.call("GET", prefix + "/branches/main/networks/" + anyNetwork + "/entities?type=9&revision=" +
                      side).status == 404, "entity listing served another branch's revision");
    check(client.call("GET", prefix + "/branches/main/networks/" + anyNetwork + "/entities/" + anyNetwork +
                      "?revision=" + side).status == 404, "entity read served another branch's revision");
    check(client.call("GET", prefix + "/branches/side/networks/" + anyNetwork + "?revision=" + main)
              .status == 400, "ancestor revision was not accepted by branch scoping");

    // Entity listings: exactly 10000 UUIDs are returned, 10001 are 413.
    const QUuid networkUuid = QUuid::createUuid();
    QUuid removable;
    native::Hash bigHead{};
    service.withRepository(id.toStdString(), [&](native::Repository& repository) {
        repository.createBranch("large");
        NetworkHydraulic network;
        network.uuid = networkUuid;
        network.id = QStringLiteral("large");
        for (int i = 0; i < 10001; ++i) {
            HydraulicNodeJunction junction;
            junction.uuid = QUuid::createUuid();
            junction.id = QStringLiteral("J%1").arg(i);
            network.nodes_junctions.append(junction);
        }
        removable = network.nodes_junctions.last().uuid;
        bigHead = native::hydraulic::saveHydraulicNetwork(repository, "large", network).id;
    });
    const QString listing = prefix + "/branches/large/networks/" +
                            networkUuid.toString(QUuid::WithoutBraces) + "/entities?type=9";
    const Response overList = client.call("GET", listing);
    check(overList.status == 413 &&
          overList.json.value("error").toObject().value("code").toString() == "limit_exceeded",
          "10001-entity listing was not rejected with 413 limit_exceeded");
    service.withRepository(id.toStdString(), [&](native::Repository& repository) {
        native::hydraulic::editHydraulicNetwork(repository, "large", networkUuid,
            {native::hydraulic::HydraulicEntityChange{removable, std::nullopt}}, nullptr, bigHead);
    });
    const Response fullList = client.call("GET", listing);
    check(fullList.status == 200 && fullList.json.value("uuids").toArray().size() == 10000,
          "10000-entity listing was not returned completely");
    check(client.call("GET", listing + "&revision=" + QString::fromStdString(native::toHex(bigHead)))
              .status == 413, "historical 10001-entity listing was not bounded");
#endif
}

// Phase 5 (cpp-httplib): limits are enforced before bodies are read, uploads
// stream without holding project locks, and requests run concurrently.
void exerciseTransport(Client& client, aowis::native::ProjectService& service,
                       const std::filesystem::path& root, std::uint64_t transferLimit) {
    namespace native = aowis::native;
    const quint16 port = client.port();
    const Response sourceProject = client.call("POST", "/api/v1/projects", QJsonObject{{"name", "Transport source"}});
    const Response targetProject = client.call("POST", "/api/v1/projects", QJsonObject{{"name", "Transport target"}});
    check(sourceProject.status == 201 && targetProject.status == 201, "transport projects not created");
    const QString sourceId = sourceProject.json.value("id").toString();
    const QString targetId = targetProject.json.value("id").toString();
    service.withRepository(sourceId.toStdString(), [](native::Repository& repository) {
        std::vector<native::Change> changes;
        for (unsigned i = 0; i < 200; ++i) {
            native::Key key{};
            key[0] = static_cast<std::uint8_t>(i);
            key[15] = 1;
            changes.push_back(native::Change{key, native::Bytes(1024, static_cast<std::uint8_t>(i))});
        }
        repository.commit("main", changes);
    });
    const BinaryResponse bundle = client.callRaw("GET", "/api/v1/projects/" + sourceId + "/branches/main/bundle");
    check(bundle.status == 200 && bundle.body.size() > 100 * 1024 &&
          bundle.contentType == "application/vnd.aowis.bundle", "transport bundle export failed");
    const QByteArray uploadPath = "/api/v1/projects/" + targetId.toUtf8() + "/branches/incoming/bundle";

    // 1. A declared length above the transfer limit is refused before any body
    //    byte is sent; the server answers and closes the connection.
    {
        RawConnection raw(port);
        check(raw.connected(), "raw connect failed");
        raw.send("POST " + uploadPath + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " +
                 QByteArray::number(static_cast<qulonglong>(transferLimit + 1)) + "\r\n\r\n");
        const QByteArray reply = raw.receive(3000);
        check(statusOf(reply) == 413 && reply.contains("transfer_too_large"),
              "oversized declared upload not rejected with 413 before its body");
        check(raw.closedByPeer(), "connection not closed after rejecting an unread body");
    }
    // 2. Per-route JSON limits, also decided before the body is read.
    const std::vector<std::pair<QByteArray, std::size_t>> jsonRoutes{
        {"/api/v1/projects", aowis::native::http::maxJsonSmallBytes},
        {"/api/v1/projects/" + targetId.toUtf8() + "/branches", aowis::native::http::maxJsonSmallBytes},
        {"/api/v1/projects/" + targetId.toUtf8() + "/merges", aowis::native::http::maxJsonMergeBytes},
        {"/api/v1/projects/" + targetId.toUtf8() + "/branches/main/networks",
         aowis::native::http::maxJsonModelBytes}};
    for (const std::pair<QByteArray, std::size_t>& route : jsonRoutes) {
        RawConnection raw(port);
        raw.send("POST " + route.first + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                 "Content-Length: " + QByteArray::number(static_cast<qulonglong>(route.second + 1)) + "\r\n\r\n");
        const QByteArray reply = raw.receive(3000);
        check(statusOf(reply) == 413 && reply.contains("limit_exceeded") && raw.closedByPeer(),
              "JSON body over its route limit not rejected before reading");
    }
    {
        RawConnection raw(port);
        raw.send("POST /api/v1/projects HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                 "Transfer-Encoding: chunked\r\n\r\n10\r\n{\"name\":\"chunk\"}\r\n0\r\n\r\n");
        const QByteArray reply = raw.receive(3000);
        check(statusOf(reply) == 411 && reply.contains("length_required"),
              "chunked JSON request without Content-Length accepted");
    }
    // 3. A chunked upload that grows past the limit is cut off with 413 and
    //    publishes nothing.
    {
        RawConnection raw(port);
        raw.send("POST " + uploadPath + " HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\n");
        const QByteArray chunk(64 * 1024, 'x');
        const QByteArray framed = QByteArray::number(chunk.size(), 16) + "\r\n" + chunk + "\r\n";
        for (std::uint64_t sent = 0; sent <= transferLimit + chunk.size(); sent += chunk.size()) {
            try { raw.send(framed); } catch (const std::exception&) { break; } // server may close early
        }
        const QByteArray reply = raw.receive(3000);
        check(statusOf(reply) == 413, "chunked upload beyond the limit not rejected with 413");
    }
    check(client.call("GET", "/api/v1/projects/" + targetId + "/branches/incoming/head").status == 404,
          "rejected upload published a branch");
    // 4. A slow upload neither blocks other requests nor holds the catalogue
    //    or project lock while its body is still arriving.
    {
        RawConnection slow(port);
        slow.send("POST " + uploadPath + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " +
                  QByteArray::number(bundle.body.size()) + "\r\n\r\n" + bundle.body.left(bundle.body.size() / 2));
        QElapsedTimer elapsed;
        elapsed.start();
        check(client.call("GET", "/api/v1/health").status == 200, "health blocked by a slow upload");
        check(client.call("GET", "/api/v1/projects").status == 200, "catalogue blocked by a slow upload");
        check(client.call("GET", "/api/v1/projects/" + targetId + "/branches").status == 200,
              "target project blocked by a slow upload");
        check(elapsed.elapsed() < 2000, "requests during a slow upload were delayed");
        slow.send(bundle.body.mid(bundle.body.size() / 2));
        const QByteArray reply = slow.receive(10000);
        check(statusOf(reply) == 201 && reply.contains("\"created\""),
              "slow upload was not imported after completion");
    }
    // 5. An upload abandoned halfway publishes nothing and leaves no file.
    {
        RawConnection abandoned(port);
        abandoned.send("POST /api/v1/projects/" + targetId.toUtf8() + "/branches/abandoned/bundle HTTP/1.1\r\n"
                       "Host: 127.0.0.1\r\nContent-Length: " + QByteArray::number(bundle.body.size()) +
                       "\r\n\r\n" + bundle.body.left(bundle.body.size() / 3));
        abandoned.close();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        check(client.call("GET", "/api/v1/projects/" + targetId + "/branches/abandoned/head").status == 404,
              "abandoned upload published a branch");
        check(std::filesystem::is_empty(root / "projects" / targetId.toStdString() / "repository" / "tmp"),
              "abandoned upload left a scratch file");
    }
    // 6. Requests are served concurrently by the worker pool.
    {
        std::atomic<int> failures{0};
        std::vector<std::thread> workers;
        for (int worker = 0; worker < 8; ++worker) {
            workers.emplace_back([&, worker]() {
                try {
                    Client parallel(port);
                    for (int request = 0; request < 20; ++request) {
                        const QString path = request % 2 == 0
                            ? "/api/v1/projects/" + targetId + "/branches/incoming/head"
                            : "/api/v1/projects/" + sourceId + "/branches/main/history?limit=1";
                        if (parallel.call("GET", path).status != 200) failures.fetch_add(1);
                    }
                } catch (...) {
                    failures.fetch_add(1);
                }
                (void)worker;
            });
        }
        for (std::thread& thread : workers) thread.join();
        check(failures.load() == 0, "concurrent requests failed");
    }
    // 7. Protocol-level rejections by httplib also carry JSON error bodies.
    const Response unknownRoute = client.call("GET", "/api/v1/no-such-route");
    check(unknownRoute.status == 404 &&
          unknownRoute.json.value("error").toObject().value("code").toString() == "not_found",
          "unknown route did not return a JSON 404");
    // Header and URI sizes are bounded by httplib (8 KiB per header line and
    // URI, at most 100 header fields); rejections close the connection.
    QByteArray manyHeaders;
    for (int i = 0; i < 200; ++i) manyHeaders += "X-" + QByteArray::number(i) + ": a\r\n";
    const std::vector<std::tuple<QByteArray, int, QByteArray>> protocolCases{
        {"GET /api/v1/health HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Large: " + QByteArray(16 * 1024, 'a') +
             "\r\n\r\n", 400, "invalid_request"},
        {"GET /api/v1/health HTTP/1.1\r\nHost: 127.0.0.1\r\n" + manyHeaders + "\r\n", 400, "invalid_request"},
        {"GET /api/v1/" + QByteArray(9000, 'a') + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", 414, "uri_too_long"}};
    for (const std::tuple<QByteArray, int, QByteArray>& protocolCase : protocolCases) {
        RawConnection raw(port);
        raw.send(std::get<0>(protocolCase));
        const QByteArray reply = raw.receive(3000);
        check(statusOf(reply) == std::get<1>(protocolCase) && reply.contains(std::get<2>(protocolCase)) &&
              raw.closedByPeer(), "oversized header/URI not rejected with a JSON error");
    }
}

// Holds one scoped operation of a project (as a long import would) until
// release(), so tests can observe what still runs meanwhile.
class ProjectHold {
public:
    ProjectHold(aowis::native::ProjectService& service, const std::string& projectId) {
        this->thread_ = std::thread([this, &service, projectId] {
            service.withRepository(projectId, [this](aowis::native::Repository&) {
                std::unique_lock<std::mutex> lock(this->mutex_);
                this->entered_ = true;
                this->changed_.notify_all();
                this->changed_.wait(lock, [this] { return this->released_; });
            });
        });
        std::unique_lock<std::mutex> lock(this->mutex_);
        check(this->changed_.wait_for(lock, std::chrono::seconds(10), [this] { return this->entered_; }),
              "project hold did not start");
    }
    ~ProjectHold() { this->release(); }
    ProjectHold(const ProjectHold&) = delete;
    ProjectHold& operator=(const ProjectHold&) = delete;
    void release() {
        {
            std::lock_guard<std::mutex> lock(this->mutex_);
            this->released_ = true;
        }
        this->changed_.notify_all();
        if (this->thread_.joinable()) this->thread_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool released_ = false;
    std::thread thread_;
};

// Joins a helper thread on scope exit, so a failed check reports its
// message instead of terminating with a joinable thread.
struct Joiner {
    explicit Joiner(std::thread& target) : thread(target) {}
    ~Joiner() {
        if (this->thread.joinable()) this->thread.join();
    }
    Joiner(const Joiner&) = delete;
    Joiner& operator=(const Joiner&) = delete;
    std::thread& thread;
};

void waitForPending(aowis::native::ProjectService& service, const QString& id, std::size_t count) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (service.pendingOperations(id.toStdString()) != count) {
        check(std::chrono::steady_clock::now() < deadline, "pending request count not reached");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

std::chrono::milliseconds timed(const std::function<void()>& action) {
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    action();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}

// Review F4: a long operation of one project blocks neither /health nor
// other projects; a full project queue answers 503 with Retry-After before
// reading an upload; racing writes of one project serialize correctly.
void exerciseConcurrency() {
    namespace native = aowis::native;
    TemporaryRoot root;
    native::ProjectService service(root.root(), 3);
    TestServer server(service, native::BundleLimits{64ULL * 1024ULL * 1024ULL, 100000ULL,
                                                    8ULL * 1024ULL * 1024ULL});
    Client client(server.port());
    const QString a = client.call("POST", "/api/v1/projects", QJsonObject{{"name", "A"}}).json.value("id").toString();
    const QString b = client.call("POST", "/api/v1/projects", QJsonObject{{"name", "B"}}).json.value("id").toString();
    const QString aPrefix = "/api/v1/projects/" + a;
    const QString bPrefix = "/api/v1/projects/" + b;

    // A bundle of ~4 MiB for project B, built in an independent repository.
    QByteArray bundle;
    {
        native::Repository source(root.root() / "source");
        source.createBranch("main");
        std::vector<native::Change> changes;
        for (unsigned i = 0; i < 2000; ++i) {
            native::Key item{};
            item[0] = static_cast<std::uint8_t>(i >> 8U);
            item[1] = static_cast<std::uint8_t>(i);
            item[15] = 7;
            changes.push_back(native::Change{item, native::Bytes(2048, static_cast<std::uint8_t>(i))});
        }
        source.commit("main", changes);
        source.exportBranchBundle("main", root.root() / "source.bundle");
        std::ifstream input(root.root() / "source.bundle", std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        bundle = QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size()));
    }

    std::thread importer;
    std::thread first;
    std::thread second;
    const Joiner joinImporter(importer);
    const Joiner joinFirst(first);
    const Joiner joinSecond(second);
    int firstStatus = 0;
    int secondStatus = 0;
    {
        ProjectHold hold(service, a.toStdString());
        // Everything else answers promptly while A's operation runs.
        check(timed([&] { check(client.call("GET", "/api/v1/health").status == 200, "health failed"); }) <
              std::chrono::milliseconds(200), "/health waited for a project operation");
        check(timed([&] { check(client.call("GET", "/api/v1/ready").status == 200, "ready failed"); }) <
              std::chrono::milliseconds(200), "/ready waited for a project operation");
        check(client.call("GET", aPrefix + "/branches/main/head").status == 200, "read of held project failed");
        check(client.call("POST", bPrefix + "/branches", QJsonObject{{"name", "side"}}).status == 201,
              "other project's write waited");
        // A real import into B, with /health probed throughout.
        std::atomic<bool> importing{true};
        int importStatus = 0;
        importer = std::thread([&] {
            Client own(server.port());
            importStatus = own.callRaw("POST", bPrefix + "/branches/imported/bundle", bundle).status;
            importing = false;
        });
        std::chrono::milliseconds slowest{0};
        unsigned probes = 0;
        while (importing.load() || probes == 0) {
            slowest = std::max(slowest, timed([&] {
                check(client.call("GET", "/api/v1/health").status == 200, "health failed during import");
            }));
            ++probes;
        }
        importer.join();
        check(importStatus == 201, "import into another project failed while A was busy");
        check(slowest < std::chrono::milliseconds(200), "/health was slow during an import");

        // Two racing creations of one branch on A queue behind the hold.
        first = std::thread([&] {
            Client own(server.port());
            firstStatus = own.call("POST", aPrefix + "/branches", QJsonObject{{"name", "race"}}).status;
        });
        second = std::thread([&] {
            Client own(server.port());
            secondStatus = own.call("POST", aPrefix + "/branches", QJsonObject{{"name", "race"}}).status;
        });
        waitForPending(service, a, 3);
        // The queue of A is full: 503 + Retry-After, also before an upload body.
        const Response busy = client.call("POST", aPrefix + "/branches", QJsonObject{{"name", "late"}});
        check(busy.status == 503 && busy.retryAfter == "1" &&
              busy.json.value("error").toObject().value("code").toString() == "busy",
              "full project queue not answered with 503 busy and Retry-After");
        const BinaryResponse busyUpload = client.callRaw("POST", aPrefix + "/branches/up/bundle", bundle);
        check(busyUpload.status == 503 && busyUpload.retryAfter == "1", "upload to a full project queue accepted");
        {
            // Headers only: the 503 must come without waiting for the body.
            RawConnection raw(server.port());
            raw.send("POST " + aPrefix.toUtf8() + "/branches/up/bundle HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                     "Content-Type: application/vnd.aowis.bundle\r\nContent-Length: 1000000\r\n\r\n");
            const QByteArray reply = raw.receive(2000);
            check(statusOf(reply) == 503 && reply.contains("Retry-After: 1") && raw.closedByPeer(),
                  "full project queue did not refuse the upload before its body");
        }
        check(client.call("POST", bPrefix + "/branches", QJsonObject{{"name", "still"}}).status == 201,
              "other project affected by A's full queue");
        hold.release();
        first.join();
        second.join();
        check((firstStatus == 201 && secondStatus == 409) || (firstStatus == 409 && secondStatus == 201),
              "racing branch creations not exactly one 201 and one 409");
    }
    check(service.pendingOperations(a.toStdString()) == 0, "project queue not drained");
    check(client.call("POST", aPrefix + "/branches", QJsonObject{{"name", "late"}}).status == 201,
          "project still busy after its queue drained");

#ifdef AOWIS_HTTP_WITH_HYDRAULIC
    // Racing hydraulic saves with one expectedHead: one 201, one 409.
    {
        const QString head = client.call("GET", aPrefix + "/branches/main/head").json.value("id").toString();
        const std::function<QString()> payload = [] {
            NetworkHydraulic network;
            network.uuid = QUuid::createUuid();
            network.id = QStringLiteral("race");
            return QString::fromLatin1(aowis::native::ModelCodec<NetworkHydraulic>::encode(network).toBase64());
        };
        const QString one = payload();
        const QString two = payload();
        int firstStatus = 0;
        int secondStatus = 0;
        std::thread first;
        std::thread second;
        const Joiner joinFirst(first);
        const Joiner joinSecond(second);
        {
            ProjectHold hold(service, a.toStdString());
            first = std::thread([&] {
                Client own(server.port());
                firstStatus = own.call("POST", aPrefix + "/branches/main/networks",
                    QJsonObject{{"expectedHead", head}, {"payloadBase64", one}}).status;
            });
            second = std::thread([&] {
                Client own(server.port());
                secondStatus = own.call("POST", aPrefix + "/branches/main/networks",
                    QJsonObject{{"expectedHead", head}, {"payloadBase64", two}}).status;
            });
            waitForPending(service, a, 3);
        }
        first.join();
        second.join();
        check((firstStatus == 201 && secondStatus == 409) || (firstStatus == 409 && secondStatus == 201),
              "racing hydraulic saves not exactly one 201 and one 409");
    }
#endif

    // Stress: 8 clients, mixed reads and writes over three projects. Every
    // answer is a success or a 503 for a full queue; nothing else.
    const QString c = client.call("POST", "/api/v1/projects", QJsonObject{{"name", "C"}}).json.value("id").toString();
    const std::vector<QString> projects{a, b, c};
    std::atomic<unsigned> created{0};
    std::atomic<unsigned> busyAnswers{0};
    std::atomic<unsigned> unexpected{0};
    std::vector<std::thread> clients;
    for (unsigned worker = 0; worker < 8; ++worker) {
        clients.emplace_back([&, worker] {
            Client own(server.port());
            for (unsigned step = 0; step < 30; ++step) {
                const QString prefix = "/api/v1/projects/" + projects[(worker + step) % projects.size()];
                int status = 0;
                switch (step % 4) {
                case 0:
                    status = own.call("POST", prefix + "/branches", QJsonObject{
                        {"name", QStringLiteral("s%1_%2").arg(worker).arg(step)}}).status;
                    if (status == 201) ++created;
                    break;
                case 1: status = own.call("GET", prefix + "/branches/main/head").status; break;
                case 2: status = own.callRaw("GET", prefix + "/branches/main/bundle").status; break;
                default: status = own.call("GET", "/api/v1/health").status; break;
                }
                if (status == 503) ++busyAnswers;
                else if (status != 200 && status != 201) ++unexpected;
            }
        });
    }
    for (std::thread& thread : clients) thread.join();
    check(unexpected == 0, "unexpected status under concurrent load");
    std::size_t branches = 0;
    for (const QString& project : projects) {
        branches += static_cast<std::size_t>(client.call("GET", "/api/v1/projects/" + project + "/branches")
                                                 .json.value("branches").toArray().size());
        service.withRepository(project.toStdString(), [](native::Repository& repository) {
            (void)repository.verifyRepository();
        });
    }
    // Before the stress: A main/race/late, B main/side/imported/still, C main.
    check(branches == 8U + created.load(), "branch count does not match successful creations");
    std::cout << "  concurrent load: " << created.load() << " branches created, "
              << busyAnswers.load() << " busy answers\n";
}

void exercise(aowis::native::PhysicalStore backend) {
    TemporaryRoot root;
    QString firstId;
    QString firstRevision;
    quint16 listenerPort = 0;
    {
        aowis::native::ProjectService service(root.root());
        const aowis::native::BundleLimits limits{1024ULL * 1024ULL, 10000ULL, 1024ULL * 1024ULL};
        TestServer server(service, limits);
        listenerPort = server.port();
        Client client(server.port());
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
            "pack",
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
        {
            // Review F6: bundle spools and exports use repository scratch files,
            // so the whole transfer API works without a usable TMPDIR.
            const char* previous = ::getenv("TMPDIR");
            const std::string saved = previous == nullptr ? std::string() : std::string(previous);
            check(::setenv("TMPDIR", (root.root() / "no-such-tmpdir").c_str(), 1) == 0, "setenv failed");
            try {
                exerciseBundleApi(client, service, firstId, backend);
            } catch (...) {
                if (previous == nullptr) ::unsetenv("TMPDIR"); else ::setenv("TMPDIR", saved.c_str(), 1);
                throw;
            }
            if (previous == nullptr) ::unsetenv("TMPDIR"); else ::setenv("TMPDIR", saved.c_str(), 1);
            check(std::filesystem::is_empty(root.root() / "projects" / firstId.toStdString() /
                                            "repository" / "tmp"),
                  "bundle transfers left files in the project scratch directory");
        }
        exerciseErrorClassification(client, service, root.root());
        exerciseBoundsAndScoping(client, service);
        exerciseTransport(client, service, root.root(), limits.maxBytes);
        server.stop();
        check(!RawConnection(listenerPort).connected(), "stopped HTTP server still accepts connections");
    }
    {
        aowis::native::ProjectService reopened(root.root());
        check(reopened.project(firstId.toStdString()).name == "Hydraulik Ü", "restart lost project name");
        check(aowis::native::toHex(reopened.head(firstId.toStdString(), "design").id) ==
              firstRevision.toStdString(), "restart lost fork revision");
    }
}

struct ServerRun {
    bool listened = false;
    int exitCode = -1;
    QString output;
    QString diagnostics;
};

// Starts the real executable, probes /api/v1/health on 127.0.0.1, optionally
// runs `whileRunning`, then stops it with SIGTERM. A process that exits on
// its own is reported with its exit code.
ServerRun runServer(const QString& executable, QStringList arguments,
                    const std::function<void(quint16)>& whileRunning = {}) {
    TemporaryRoot root;
    const quint16 port = reservePort();
    arguments << QStringLiteral("--port") << QString::number(port)
              << QStringLiteral("--data-root") << QString::fromStdString(root.root().string());
    QProcess process;
    process.setProgram(executable);
    process.setArguments(arguments);
    process.start();
    check(process.waitForStarted(5000), "database HTTP executable did not start");
    ServerRun run;
    try {
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < 5000 && process.state() != QProcess::NotRunning) {
            RawConnection probe(port);
            if (probe.connected()) {
                run.listened = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (run.listened) {
            Client client(port);
            check(client.call("GET", "/api/v1/health").status == 200, "executable health failed");
            if (whileRunning) whileRunning(port);
            process.terminate(); // SIGTERM
        }
        if (!process.waitForFinished(5000)) {
            process.kill();
            process.waitForFinished(3000);
            throw std::runtime_error("database HTTP executable did not stop on SIGTERM");
        }
    } catch (...) {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(3000);
        }
        throw;
    }
    run.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    run.output = QString::fromLocal8Bit(process.readAllStandardOutput());
    run.diagnostics = QString::fromLocal8Bit(process.readAllStandardError());
    return run;
}

// The real executable: startup, request handling and a clean SIGTERM
// shutdown (exit code 0), plus configuration validation.
void exerciseExecutable(const QString& executable) {
    const ServerRun normal = runServer(executable, {QStringLiteral("--bind"), QStringLiteral("127.0.0.1")},
        [](quint16 port) {
            Client client(port);
            const Response created = client.call("POST", "/api/v1/projects",
                                                 QJsonObject{{"name", "Shutdown test"}});
            check(created.status == 201 && created.json.value("storage").toString() == "pack",
                  "executable did not create a project");
        });
    check(normal.listened && normal.exitCode == 0, "executable did not shut down cleanly on SIGTERM");
    check(normal.output.contains("AOWIS database listening on 127.0.0.1:"), "startup message missing");

    // All addresses are served without a proxy policy: limits are enforced
    // by the server itself on every platform.
    const ServerRun anyAddress = runServer(executable, {QStringLiteral("--bind"), QStringLiteral("0.0.0.0"),
                                                        QStringLiteral("--worker-threads"), QStringLiteral("2"),
                                                        QStringLiteral("--max-pending-per-project"), QStringLiteral("2")});
    check(anyAddress.listened && anyAddress.exitCode == 0 && !anyAddress.diagnostics.contains("AOWIS database"),
          "non-loopback listener with 2 workers failed");

    const std::vector<QStringList> invalid{
        {QStringLiteral("--bind"), QStringLiteral("localhost")},
        {QStringLiteral("--bind"), QStringLiteral("127.0.0.1"), QStringLiteral("--worker-threads"), QStringLiteral("0")},
        {QStringLiteral("--bind"), QStringLiteral("127.0.0.1"), QStringLiteral("--worker-threads"), QStringLiteral("257")},
        {QStringLiteral("--bind"), QStringLiteral("127.0.0.1"), QStringLiteral("--sync-max-mib"), QStringLiteral("0")},
        {QStringLiteral("--bind"), QStringLiteral("127.0.0.1"), QStringLiteral("--max-pending-per-project"), QStringLiteral("0")},
        {QStringLiteral("--bind"), QStringLiteral("127.0.0.1"), QStringLiteral("--worker-threads"), QStringLiteral("4"),
         QStringLiteral("--max-pending-per-project"), QStringLiteral("5")},
        {QStringLiteral("--bind"), QStringLiteral("127.0.0.1"), QStringLiteral("--max-pending-per-project"), QStringLiteral("x")}};
    for (const QStringList& arguments : invalid) {
        const ServerRun refused = runServer(executable, arguments);
        check(!refused.listened && refused.exitCode == 2, "invalid configuration was accepted");
    }
    TemporaryRoot configuration;
    const std::filesystem::path ini = configuration.root() / "server.ini";
    {
        std::ofstream output(ini);
        output << "[server]\nbind=127.0.0.1\nworker_threads=0\n";
    }
    const ServerRun fromIni = runServer(executable, {QStringLiteral("--config"),
                                                     QString::fromStdString(ini.string())});
    check(!fromIni.listened && fromIni.exitCode == 2, "invalid INI worker_threads accepted");
    {
        std::ofstream output(ini, std::ios::trunc);
        output << "[server]\nbind=127.0.0.1\nworker_threads=2\nmax_pending_per_project=3\n";
    }
    const ServerRun pendingIni = runServer(executable, {QStringLiteral("--config"),
                                                        QString::fromStdString(ini.string())});
    check(!pendingIni.listened && pendingIni.exitCode == 2, "INI max_pending_per_project above workers accepted");
    {
        std::ofstream output(ini, std::ios::trunc);
        output << "[server]\nbind=127.0.0.1\nworker_threads=4\nmax_pending_per_project=4\n";
    }
    const ServerRun validIni = runServer(executable, {QStringLiteral("--config"),
                                                      QString::fromStdString(ini.string())});
    check(validIni.listened && validIni.exitCode == 0, "valid INI max_pending_per_project refused");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        check(argc == 2, "expected path to aowis-server-db executable");
        exercise(aowis::native::PhysicalStore::Pack);
        exerciseConcurrency();
        exerciseExecutable(QString::fromLocal8Bit(argv[1]));
        std::cout << "HTTP project service tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HTTP service test failed: " << error.what() << '\n';
        return 1;
    }
}
