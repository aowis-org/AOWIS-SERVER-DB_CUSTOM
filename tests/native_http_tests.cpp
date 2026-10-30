#include "model_edit_event_fixture.h"
#include "aowis/dvmdb/dvmdb_http_routes.h"
#ifdef AOWIS_HTTP_WITH_HYDRAULIC
#include "aowis/dvmdb/hydraulic_network_persistence.h"
#endif
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
#include "aowis/dvmdb/hydraulic_network_persistence.h"
#include <aowis/model/edit/model_edit_transaction.h>
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
#include <set>
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
        char name[] = "/tmp/aowis-dvmdb-http-XXXXXX";
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
            : verb == "PATCH"
                ? this->client_.Patch(target,
                    std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())),
                    mime.toStdString())
                : this->client_.Post(target,
                    std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())),
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
    TestServer(aowis::dvmdb::ProjectService& service, aowis::dvmdb::BundleLimits limits,
               std::shared_ptr<aowis::dvmdb::http::TransferStagingBudget> stagingBudget = nullptr) {
        this->server_.new_task_queue = [] { return new httplib::ThreadPool(8, 8, 128); };
        aowis::dvmdb::http::registerRoutes(this->server_, service, limits, stagingBudget);
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
void exerciseHydraulicApi(Client& client, aowis::dvmdb::ProjectService& service,
                          const QString& projectId, const QString& otherProject,
                          const QString& rawHead) {
    namespace native = aowis::dvmdb;
    const QString prefix = QStringLiteral("/dvmdb/v1/projects/") + projectId;
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
    check(client.call("GET", "/dvmdb/v1/projects/" + otherProject +
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
    // Review F8: impossible values and non-finite numbers are 400.
    {
        HydraulicNodeJunction impossible = junction;
        impossible.emitter.coefficient = -1;
        const Response rejected = client.call("POST", networkRoute + "/edits",
            QJsonObject{{"expectedHead", createdHead}, {"changes", QJsonArray{QJsonObject{
                {"uuid", junctionId}, {"type", 9},
                {"payloadBase64", QString::fromLatin1(native::ModelCodec<HydraulicNodeJunction>::encode(impossible).toBase64())}}}}});
        check(rejected.status == 400 &&
              rejected.json.value("error").toObject().value("message").toString().contains("emitter.coefficient"),
              "negative emitter coefficient accepted over HTTP");
        // A NaN elevation cannot be encoded; patch it into valid bytes.
        HydraulicNodeJunction patched = junction;
        patched.elevation_m = 2.0;
        QByteArray bytes = native::ModelCodec<HydraulicNodeJunction>::encode(patched);
        patched.elevation_m = -2.0;
        const QByteArray negative = native::ModelCodec<HydraulicNodeJunction>::encode(patched);
        qsizetype start = 0;
        while (bytes[start] == negative[start]) ++start;
        const char nanBytes[8] = {0x7f, static_cast<char>(0xf8), 0, 0, 0, 0, 0, 0};
        bytes.replace(start, 8, nanBytes, 8);
        check(client.call("POST", networkRoute + "/edits",
            QJsonObject{{"expectedHead", createdHead}, {"changes", QJsonArray{QJsonObject{
                {"uuid", junctionId}, {"type", 9}, {"payloadBase64", QString::fromLatin1(bytes.toBase64())}}}}}).status == 400,
              "NaN elevation accepted over HTTP");
        NetworkHydraulic impossibleNetwork = network;
        impossibleNetwork.options_hydraulic.specific_gravity = 0;
        check(client.call("POST", main + "/networks", QJsonObject{{"expectedHead", createdHead},
            {"payloadBase64", QString::fromLatin1(native::ModelCodec<NetworkHydraulic>::encode(impossibleNetwork).toBase64())}}).status == 400,
              "network with zero specific gravity accepted over HTTP");
        check(client.call("GET", main + "/head").json.value("id").toString() == createdHead,
              "rejected value edit changed the branch");
    }
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
    // HTTP fast-forwards must validate source records even when they were
    // committed through the generic native library instead of hydraulic APIs.
    check(client.call("POST", prefix + "/branches",
        QJsonObject{{"name", "http-ff-target"}, {"fromBranch", "main"}}).status == 201,
        "cannot create HTTP fast-forward target");
    check(client.call("POST", prefix + "/branches",
        QJsonObject{{"name", "http-ff-source"}, {"fromBranch", "main"}}).status == 201,
        "cannot create HTTP fast-forward source");
    HydraulicNodeJunction ffJunction = junction;
    ffJunction.elevation_m = 527;
    const QString ffPayload = QString::fromLatin1(
        native::ModelCodec<HydraulicNodeJunction>::encode(ffJunction).toBase64());
    const Response ffEdit = client.call("POST",
        prefix + "/branches/http-ff-source/networks/" + networkId + "/edits",
        QJsonObject{{"expectedHead", editedHead}, {"changes", QJsonArray{QJsonObject{
            {"uuid", junctionId}, {"type", 9}, {"payloadBase64", ffPayload}}}}});
    check(ffEdit.status == 200, "cannot prepare valid hydraulic HTTP fast-forward");
    const QString ffTip = ffEdit.json.value("revision").toObject().value("id").toString();
    check(ffTip.size() == 64 && ffTip != editedHead,
          "valid hydraulic HTTP fast-forward source did not advance");
    check(client.call("POST", prefix + "/merges", QJsonObject{
        {"target", "http-ff-target"}, {"source", "http-ff-source"},
        {"expectedTargetHead", editedHead}, {"expectedSourceHead", editedHead}}).status == 409,
        "stale HTTP fast-forward source pin accepted");
    const Response ffMerge = client.call("POST", prefix + "/merges", QJsonObject{
        {"target", "http-ff-target"}, {"source", "http-ff-source"},
        {"expectedTargetHead", editedHead}, {"expectedSourceHead", ffTip}});
    check(ffMerge.status == 200 && ffMerge.json.value("status").toString() == "fast_forward" &&
          client.call("GET", prefix + "/branches/http-ff-target/head")
              .json.value("id").toString() == ffTip,
          "valid hydraulic HTTP fast-forward failed");
    check(client.call("POST", prefix + "/merges", QJsonObject{
        {"target", "http-ff-target"}, {"source", "http-ff-source"},
        {"expectedTargetHead", ffTip}, {"expectedSourceHead", ffTip}})
          .json.value("status").toString() == "up_to_date",
          "equal-tip HTTP fast-forward was not a no-op");

    check(client.call("POST", prefix + "/branches",
        QJsonObject{{"name", "http-bad-target"}, {"fromBranch", "main"}}).status == 201,
        "cannot create invalid HTTP fast-forward target");
    check(client.call("POST", prefix + "/branches",
        QJsonObject{{"name", "http-bad-source"}, {"fromBranch", "main"}}).status == 201,
        "cannot create invalid HTTP fast-forward source");
    QString invalidTip;
    std::uint64_t invalidBytes = 0;
    service.withRepository(projectId.toStdString(), [&](native::Repository& repository) {
        const std::optional<native::ModelRecord> record = repository.getModel(
            "http-bad-source", native::hydraulic::keyFromUuid(junctionUuid));
        check(record.has_value(), "missing network member for invalid HTTP merge fixture");
        const native::Key orphan = native::hydraulic::keyFromUuid(QUuid::createUuid());
        invalidTip = QString::fromStdString(native::toHex(repository.commitModel(
            "http-bad-source", {{orphan, *record}}).id));
        invalidBytes = repository.packSize();
    });
    check(client.call("POST", prefix + "/merges", QJsonObject{
        {"target", "http-bad-target"}, {"source", "http-bad-source"},
        {"expectedTargetHead", ffTip}, {"expectedSourceHead", invalidTip}}).status == 409,
        "stale HTTP fast-forward target pin accepted");
    const Response badMerge = client.call("POST", prefix + "/merges", QJsonObject{
        {"target", "http-bad-target"}, {"source", "http-bad-source"},
        {"expectedTargetHead", editedHead}, {"expectedSourceHead", invalidTip}});
    check(badMerge.status == 409 && badMerge.json.value("status").toString() == "conflicts" &&
          !badMerge.json.value("conflicts").toArray().isEmpty() &&
          client.call("GET", prefix + "/branches/http-bad-target/head")
              .json.value("id").toString() == editedHead &&
          client.call("GET", prefix + "/branches/http-bad-source/head")
              .json.value("id").toString() == invalidTip,
          "invalid hydraulic HTTP fast-forward advanced a branch");
    service.withRepository(projectId.toStdString(), [&](native::Repository& repository) {
        check(repository.packSize() == invalidBytes,
              "rejected HTTP fast-forward appended pack objects");
    });
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
void exerciseBundleApi(Client& client, aowis::dvmdb::ProjectService& service,
                       const QString& sourceId) {
    namespace native = aowis::dvmdb;
    const Response destination = client.call("POST", "/dvmdb/v1/projects",
        QJsonObject{{"name", "Replication recipient"}});
    check(destination.status == 201, "destination creation failed");
    const QString destinationId = destination.json.value("id").toString();
    const QString from = QStringLiteral("/dvmdb/v1/projects/") + sourceId + "/branches/sync_demo";
    const QString to = QStringLiteral("/dvmdb/v1/projects/") + destinationId + "/branches/tracking";
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
    const BinaryResponse identifiedFull = client.callRaw("GET", from + "/bundle?identity=1");
    check(identifiedFull.status == 200 && identifiedFull.body.startsWith("AOWSYNC3"),
          "identified HTTP export did not use UUIDv7 bundle format");
    check(client.callRaw("GET", from + "/bundle?identity=0").status == 400,
          "invalid identified bundle option accepted");
    const QString identifiedTo = QStringLiteral("/dvmdb/v1/projects/") +
                                 destinationId + "/branches/with_identity";
    const BinaryResponse identifiedImport = client.callRaw(
        "POST", identifiedTo + "/bundle", identifiedFull.body);
    check(identifiedImport.status == 201, "identified HTTP bundle import failed");
    service.withRepository(sourceId.toStdString(), [&](native::Repository& source) {
        service.withRepository(destinationId.toStdString(), [&](native::Repository& dest) {
            check(source.branchId("sync_demo") == dest.branchId("with_identity"),
                  "identified HTTP transfer did not preserve branch UUIDv7");
        });
    });
    client.abortDownload(from + "/bundle");
    check(client.callRaw("GET", from + "/bundle").body == bundle.body,
          "download after client cancellation changed bytes");
    check(client.callRaw("GET", from + "/bundle?base=invalid").status == 400,
          "malformed base hash accepted");
    check(client.callRaw("GET", from + "/bundle?base=" + QString(64, QChar('f'))).status == 409,
          "unknown export base accepted");
    check(client.callRaw("GET", from + "/bundle?unexpected=1").status == 400,
          "unknown bundle query argument accepted");
    check(client.callRaw("GET", "/dvmdb/v1/projects/invalid/branches/sync_demo/bundle").status == 400,
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
    const BinaryResponse identifiedDelta = client.callRaw(
        "GET", from + "/bundle?base=" + firstHead + "&identity=1");
    check(identifiedDelta.status == 200 && identifiedDelta.body.startsWith("AOWSYNC4"),
          "identified HTTP incremental export did not use AOWSYNC4");
    check(client.callRaw("POST", identifiedTo + "/bundle", identifiedDelta.body).status == 200,
          "identified HTTP incremental import failed");
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
void exerciseErrorClassification(Client& client, aowis::dvmdb::ProjectService& service,
                                 const std::filesystem::path& root) {
    namespace native = aowis::dvmdb;
    const Response created = client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "Errors"}});
    check(created.status == 201, "error-classification project creation failed");
    const QString id = created.json.value("id").toString();
    const QString prefix = "/dvmdb/v1/projects/" + id;
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
void exerciseBoundsAndScoping(Client& client, aowis::dvmdb::ProjectService& service) {
    namespace native = aowis::dvmdb;
    const Response created = client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "Bounds"}});
    check(created.status == 201, "bounds project creation failed");
    const QString id = created.json.value("id").toString();
    const QString prefix = "/dvmdb/v1/projects/" + id;
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
void exerciseTransport(Client& client, aowis::dvmdb::ProjectService& service,
                       const std::filesystem::path& root, std::uint64_t transferLimit) {
    namespace native = aowis::dvmdb;
    const quint16 port = client.port();
    const Response sourceProject = client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "Transport source"}});
    const Response targetProject = client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "Transport target"}});
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
    const BinaryResponse bundle = client.callRaw("GET", "/dvmdb/v1/projects/" + sourceId + "/branches/main/bundle");
    check(bundle.status == 200 && bundle.body.size() > 100 * 1024 &&
          bundle.contentType == "application/vnd.aowis.bundle", "transport bundle export failed");
    const QByteArray uploadPath = "/dvmdb/v1/projects/" + targetId.toUtf8() + "/branches/incoming/bundle";

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
        {"/dvmdb/v1/projects", aowis::dvmdb::http::maxJsonSmallBytes},
        {"/dvmdb/v1/projects/" + targetId.toUtf8() + "/branches", aowis::dvmdb::http::maxJsonSmallBytes},
        {"/dvmdb/v1/projects/" + targetId.toUtf8() + "/merges", aowis::dvmdb::http::maxJsonMergeBytes},
        {"/dvmdb/v1/projects/" + targetId.toUtf8() + "/branches/main/networks",
         aowis::dvmdb::http::maxJsonModelBytes}};
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
        raw.send("POST /dvmdb/v1/projects HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
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
    check(client.call("GET", "/dvmdb/v1/projects/" + targetId + "/branches/incoming/head").status == 404,
          "rejected upload published a branch");
    // 4. A slow upload neither blocks other requests nor holds the catalogue
    //    or project lock while its body is still arriving.
    {
        RawConnection slow(port);
        slow.send("POST " + uploadPath + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " +
                  QByteArray::number(bundle.body.size()) + "\r\n\r\n" + bundle.body.left(bundle.body.size() / 2));
        QElapsedTimer elapsed;
        elapsed.start();
        check(client.call("GET", "/dvmdb/v1/health").status == 200, "health blocked by a slow upload");
        check(client.call("GET", "/dvmdb/v1/projects").status == 200, "catalogue blocked by a slow upload");
        check(client.call("GET", "/dvmdb/v1/projects/" + targetId + "/branches").status == 200,
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
        abandoned.send("POST /dvmdb/v1/projects/" + targetId.toUtf8() + "/branches/abandoned/bundle HTTP/1.1\r\n"
                       "Host: 127.0.0.1\r\nContent-Length: " + QByteArray::number(bundle.body.size()) +
                       "\r\n\r\n" + bundle.body.left(bundle.body.size() / 3));
        abandoned.close();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        check(client.call("GET", "/dvmdb/v1/projects/" + targetId + "/branches/abandoned/head").status == 404,
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
                            ? "/dvmdb/v1/projects/" + targetId + "/branches/incoming/head"
                            : "/dvmdb/v1/projects/" + sourceId + "/branches/main/history?limit=1";
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
    const Response unknownRoute = client.call("GET", "/dvmdb/v1/no-such-route");
    check(unknownRoute.status == 404 &&
          unknownRoute.json.value("error").toObject().value("code").toString() == "not_found",
          "unknown route did not return a JSON 404");
    // Header and URI sizes are bounded by httplib (8 KiB per header line and
    // URI, at most 100 header fields); rejections close the connection.
    QByteArray manyHeaders;
    for (int i = 0; i < 200; ++i) manyHeaders += "X-" + QByteArray::number(i) + ": a\r\n";
    const std::vector<std::tuple<QByteArray, int, QByteArray>> protocolCases{
        {"GET /dvmdb/v1/health HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Large: " + QByteArray(16 * 1024, 'a') +
             "\r\n\r\n", 400, "invalid_request"},
        {"GET /dvmdb/v1/health HTTP/1.1\r\nHost: 127.0.0.1\r\n" + manyHeaders + "\r\n", 400, "invalid_request"},
        {"GET /dvmdb/v1/" + QByteArray(9000, 'a') + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", 414, "uri_too_long"}};
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
    ProjectHold(aowis::dvmdb::ProjectService& service, const std::string& projectId) {
        this->thread_ = std::thread([this, &service, projectId] {
            service.withRepository(projectId, [this](aowis::dvmdb::Repository&) {
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

void waitForPending(aowis::dvmdb::ProjectService& service, const QString& id, std::size_t count) {
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
    namespace native = aowis::dvmdb;
    TemporaryRoot root;
    native::ProjectService service(root.root(), 3);
    TestServer server(service, native::BundleLimits{64ULL * 1024ULL * 1024ULL, 100000ULL,
                                                    8ULL * 1024ULL * 1024ULL});
    Client client(server.port());
    const QString a = client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "A"}}).json.value("id").toString();
    const QString b = client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "B"}}).json.value("id").toString();
    const QString aPrefix = "/dvmdb/v1/projects/" + a;
    const QString bPrefix = "/dvmdb/v1/projects/" + b;

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
        check(timed([&] { check(client.call("GET", "/dvmdb/v1/health").status == 200, "health failed"); }) <
              std::chrono::milliseconds(200), "/health waited for a project operation");
        check(timed([&] { check(client.call("GET", "/dvmdb/v1/ready").status == 200, "ready failed"); }) <
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
                check(client.call("GET", "/dvmdb/v1/health").status == 200, "health failed during import");
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
            return QString::fromLatin1(aowis::dvmdb::ModelCodec<NetworkHydraulic>::encode(network).toBase64());
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
    const QString c = client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "C"}}).json.value("id").toString();
    const std::vector<QString> projects{a, b, c};
    std::atomic<unsigned> created{0};
    std::atomic<unsigned> busyAnswers{0};
    std::atomic<unsigned> unexpected{0};
    std::vector<std::thread> clients;
    for (unsigned worker = 0; worker < 8; ++worker) {
        clients.emplace_back([&, worker] {
            Client own(server.port());
            for (unsigned step = 0; step < 30; ++step) {
                const QString prefix = "/dvmdb/v1/projects/" + projects[(worker + step) % projects.size()];
                int status = 0;
                switch (step % 4) {
                case 0:
                    status = own.call("POST", prefix + "/branches", QJsonObject{
                        {"name", QStringLiteral("s%1_%2").arg(worker).arg(step)}}).status;
                    if (status == 201) ++created;
                    break;
                case 1: status = own.call("GET", prefix + "/branches/main/head").status; break;
                case 2: status = own.callRaw("GET", prefix + "/branches/main/bundle").status; break;
                default: status = own.call("GET", "/dvmdb/v1/health").status; break;
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
        branches += static_cast<std::size_t>(client.call("GET", "/dvmdb/v1/projects/" + project + "/branches")
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

void exercise() {
    TemporaryRoot root;
    QString firstId;
    QString firstRevision;
    quint16 listenerPort = 0;
    {
        aowis::dvmdb::ProjectService service(root.root());
        const aowis::dvmdb::BundleLimits limits{1024ULL * 1024ULL, 10000ULL, 1024ULL * 1024ULL};
        TestServer server(service, limits);
        listenerPort = server.port();
        Client client(server.port());
        check(client.call("GET", "/dvmdb/v1/health").status == 200, "health failed");
        check(client.call("GET", "/dvmdb/v1/ready").status == 200, "readiness failed");
        check(client.call("GET", "/dvmdb/v1/projects").json.value("projects").toArray().isEmpty(),
              "catalogue initially nonempty");
        check(client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", ""}}).status == 400,
              "invalid create accepted");
        check(client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "X"}, {"evil", 42}}).status == 400,
              "unknown field accepted");
        const Response first = client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "Hydraulik Ü"}});
        check(first.status == 201, "create project failed");
        firstId = first.json.value("id").toString();
        check(first.json.value("name").toString() == "Hydraulik Ü", "project name wrong");
        check(first.json.keys() == QStringList({"id", "name"}), "project JSON has unexpected fields");
        check(client.call("POST", "/dvmdb/v1/projects",
                          QJsonObject{{"name", "X"}, {"storage", "pack"}}).status == 400,
              "removed storage field accepted");
        const Response second = client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "Independent"}});
        check(second.status == 201 && second.json.value("id").toString() != firstId,
              "second project creation failed");
        check(client.call("GET", "/dvmdb/v1/projects").json.value("projects").toArray().size() == 2,
              "project listing incorrect");
        check(client.call("GET", "/dvmdb/v1/projects/invalid-id").status == 400,
              "invalid project UUID was accepted");
        check(client.call("GET", "/dvmdb/v1/projects/00000000-0000-4000-8000-000000000000").status == 404,
              "unknown project accepted");
        check(client.call("GET", "/dvmdb/v1/projects/" + firstId).status == 200,
              "project details failed");
        const QString prefix = "/dvmdb/v1/projects/" + firstId;
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
        check(client.call("GET", "/dvmdb/v1/projects/" + second.json.value("id").toString() + "/branches")
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
                exerciseBundleApi(client, service, firstId);
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
        aowis::dvmdb::ProjectService reopened(root.root());
        check(reopened.project(firstId.toStdString()).name == "Hydraulik Ü", "restart lost project name");
        check(aowis::dvmdb::toHex(reopened.headShaFixture(firstId.toStdString(), "design").id) ==
              firstRevision.toStdString(), "restart lost fork revision");
    }
}

struct ServerRun {
    bool listened = false;
    int exitCode = -1;
    QString output;
    QString diagnostics;
};

// Holding an incomplete upload on project A must reserve the global HTTP
// staging budget and reject new staging on project B, but not block /health.
// Closing A must release capacity without changing any branch.
void exerciseTransferStagingBudget() {
    namespace native = aowis::dvmdb;
    TemporaryRoot root;
    native::ProjectService service(root.root());
    constexpr std::uint64_t transferBytes = 1024ULL * 1024ULL;
    std::shared_ptr<native::http::TransferStagingBudget> staging =
        std::make_shared<native::http::TransferStagingBudget>(transferBytes);
    TestServer server(service, native::BundleLimits{transferBytes, 10000, transferBytes}, staging);
    Client client(server.port());
    const Response first = client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "Slow source"}});
    const Response second = client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "Other project"}});
    check(first.status == 201 && second.status == 201, "staging test projects not created");
    const QString firstId = first.json.value("id").toString();
    const QString secondId = second.json.value("id").toString();
    const QString otherBundle = "/dvmdb/v1/projects/" + secondId + "/branches/main/bundle";
    {
        RawConnection slow(server.port());
        check(slow.connected(), "slow staging client failed to connect");
        slow.send("POST /dvmdb/v1/projects/" + firstId.toUtf8() +
                  "/branches/unpublished/bundle HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                  "Content-Type: application/vnd.aowis.revision-bundle-v2\r\nContent-Length: 100000\r\n\r\n"
                  "partially-sent");
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (staging->reservedBytes() == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(staging->reservedBytes() == transferBytes, "slow upload did not reserve staging budget");
        const BinaryResponse rejected = client.callRaw("GET", otherBundle);
        check(rejected.status == 503 && rejected.body.contains("staging_busy") &&
              rejected.retryAfter == "1", "other project staging was not rejected with retry hint");
        check(client.call("GET", "/dvmdb/v1/health").status == 200,
              "health endpoint blocked by exhausted staging budget");
        slow.close();
    }
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (staging->reservedBytes() != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(staging->reservedBytes() == 0, "disconnected upload retained staging reservation");
    check(client.callRaw("GET", otherBundle).status == 200,
          "transfer not admitted after disconnected upload released quota");
    check(staging->reservedBytes() == 0, "completed download retained staging reservation");
    check(client.call("GET", "/dvmdb/v1/projects/" + firstId + "/branches/unpublished/head").status == 404,
          "abandoned upload published an incomplete branch");
}

// A separate full-DAG protocol test keeps the ordinary CRUD fixture intact.
void exerciseRevisionGraphHttp() {
    TemporaryRoot root;
    aowis::dvmdb::ProjectService service(root.root());
    const std::string id = service.createProjectShaFixture("Graph API").id;
    const std::string other = service.createProjectShaFixture("Other graph").id;
    aowis::dvmdb::Hash initial{};
    aowis::dvmdb::Hash merged{};
    service.withRepository(id, [&](aowis::dvmdb::Repository& repository) {
        initial = repository.head("main").id;
        repository.commit("main", {{aowis::dvmdb::Key{1}, aowis::dvmdb::Bytes{1}}});
        repository.createBranch("feature", "main");
        repository.createBranch("alias", "feature");
        repository.commit("main", {{aowis::dvmdb::Key{2}, aowis::dvmdb::Bytes{2}}});
        repository.commit("feature", {{aowis::dvmdb::Key{3}, aowis::dvmdb::Bytes{3}}});
        const aowis::dvmdb::MergeResult result = repository.mergeBranches("main", "feature");
        check(result.status == aowis::dvmdb::MergeStatus::Merged &&
              result.revision.parents.size() == 2, "HTTP graph test merge fixture failed");
        merged = result.revision.id;
    });
    TestServer server(service, aowis::dvmdb::BundleLimits{1024ULL * 1024ULL, 10000ULL, 1024ULL * 1024ULL});
    Client client(server.port());
    const QString path = "/dvmdb/v1/projects/" + QString::fromStdString(id) + "/revision-graph";
    check(client.call("GET", path + "?limit=0").status == 400,
          "graph route accepted zero limit");
    check(client.call("GET", path + "?limit=1001").status == 400,
          "graph route accepted oversized page");
    check(client.call("GET", path + "?cursor=bogus").status == 400,
          "graph route accepted invalid cursor");
    const Response first = client.call("GET", path + "?limit=1");
    check(first.status == 200 && !first.json.value("complete").toBool() &&
          first.json.value("heads").toArray().size() == 3 &&
          first.json.value("revisions").toArray().size() == 1 &&
          first.json.value("nextCursor").isString(), "graph initial response malformed");
    const QString initialCursor = first.json.value("nextCursor").toString();
    check(client.call("GET", "/dvmdb/v1/projects/" + QString::fromStdString(other) +
                      "/revision-graph?cursor=" + initialCursor).status == 400,
          "signed cursor could be replayed into another project");
    QString tampered = initialCursor;
    tampered[3] = tampered.at(3) == QChar('a') ? QChar('b') : QChar('a');
    check(client.call("GET", path + "?cursor=" + tampered).status == 400,
          "tampered graph cursor accepted");
    // Commit after the snapshot was pinned; old pages must still describe
    // the original graph, while a new request shows the advanced main head.
    aowis::dvmdb::Hash later{};
    service.withRepository(id, [&](aowis::dvmdb::Repository& repository) {
        later = repository.commit("main", {{aowis::dvmdb::Key{4}, aowis::dvmdb::Bytes{4}}}).id;
    });
    std::set<QString> seen;
    auto record = [&](const Response& response) {
        check(response.status == 200 && response.json.value("heads").toArray().size() == 3,
              "graph paging response malformed");
        const QJsonArray heads = response.json.value("heads").toArray();
        check(heads.at(2).toObject().value("revision").toString() ==
              QString::fromStdString(aowis::dvmdb::toHex(merged)),
              "graph HEAD snapshot changed after concurrent commit");
        const QJsonArray revisions = response.json.value("revisions").toArray();
        check(revisions.size() == 1, "graph page size incorrect");
        const QString hash = revisions.at(0).toObject().value("id").toString();
        check(seen.insert(hash).second, "duplicate graph revision across HTTP pages");
    };
    record(first);
    QString cursor = initialCursor;
    for (int i = 0; !cursor.isEmpty(); ++i) {
        check(i < 20, "HTTP revision graph cursor loop");
        const Response response = client.call("GET", path + "?limit=1&cursor=" + cursor);
        record(response);
        cursor = response.json.value("nextCursor").toString();
        check(response.json.value("complete").toBool() == cursor.isEmpty(),
              "graph completeness flag disagrees with cursor");
    }
    check(seen.size() == 5 && seen.contains(QString::fromStdString(aowis::dvmdb::toHex(initial))) &&
          !seen.contains(QString::fromStdString(aowis::dvmdb::toHex(later))),
          "HTTP graph omitted ancestry or leaked a later revision");
    const Response updated = client.call("GET", path);
    check(updated.status == 200 && updated.json.value("complete").toBool() &&
          updated.json.value("revisions").toArray().size() == 6 &&
          updated.json.value("heads").toArray().at(2).toObject().value("revision").toString() ==
              QString::fromStdString(aowis::dvmdb::toHex(later)),
          "fresh HTTP graph missing advanced branch tip");
}

// Starts the real executable, probes /dvmdb/v1/health on 127.0.0.1, optionally
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
            check(client.call("GET", "/dvmdb/v1/health").status == 200, "executable health failed");
            check(client.call("GET", "/api/v1/health").status == 404,
                  "legacy HTTP route unexpectedly accepted");
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
            const Response created = client.call("POST", "/dvmdb/v1/projects",
                                                 QJsonObject{{"name", "Shutdown test"}});
            check(created.status == 201 && created.json.value("name").toString() == "Shutdown test",
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
        {QStringLiteral("--sync-max-mib"), QStringLiteral("32"), QStringLiteral("--sync-max-inflight-mib"), QStringLiteral("16")},
        {QStringLiteral("--sync-max-inflight-mib"), QStringLiteral("4097")},
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
void exerciseRevisionBundleV2Http() {
    namespace native = aowis::dvmdb;
    TemporaryRoot temp;
    const std::filesystem::path sourcePath = temp.root() / "v2-source";
    const std::filesystem::path replicaPath = temp.root() / "v2-replica";
    native::ProjectService source(sourcePath);
    const native::ProjectInfo project = source.createProjectV2("Revision transfer");
    const std::optional<native::ProjectId> projectId = native::ProjectId::parse(project.id);
    check(projectId.has_value(), "created v2 project is not UUIDv7");
    // Clone the initial, fully published project before either side commits.
    // The source and recipient therefore agree on the project and branch IDs,
    // and only the source later advances its HEAD.
    const std::filesystem::path catalogue = replicaPath / "projects";
    std::filesystem::create_directories(catalogue);
    std::filesystem::copy(sourcePath / "projects" / project.id, catalogue / project.id,
                          std::filesystem::copy_options::recursive);
    native::ProjectService replica(replicaPath);
    const std::optional<native::BranchId> identity = source.branchId(project.id, "main");
    check(identity.has_value() && replica.branchId(project.id, "main") == identity,
          "cloned v2 project lost branch identity");
    native::RevisionId tip = source.headV2(project.id, "main");
    native::Key key{};
    key[15] = 41;
    source.withRepository(project.id, [&](native::Repository& repository) {
        tip = repository.commitV2(*projectId, "main", {{key, native::Bytes{1, 2, 3}}}, tip);
    });
    const native::BundleLimits limits{4ULL * 1024ULL * 1024ULL, 10000ULL, 1024ULL * 1024ULL};
    TestServer sourceServer(source, limits);
    TestServer replicaServer(replica, limits);
    Client sender(sourceServer.port());
    Client receiver(replicaServer.port());
    const QString prefix = QStringLiteral("/dvmdb/v1/projects/") +
                           QString::fromStdString(project.id) +
                           QStringLiteral("/branches/main/bundle");
    const BinaryResponse full = sender.callRaw("GET", prefix);
    check(full.status == 200 && full.body.startsWith("AOWREV21") &&
          full.contentType == "application/vnd.aowis.revision-bundle-v2",
          "HTTP UUIDv7 full export is not AOWREV21");
    // First-clone path: provision the exact logical project UUID in a
    // different root, then import the verified bundle without copying files.
    const std::filesystem::path freshPath = temp.root() / "v2-http-fresh-clone";
    native::ProjectService fresh(freshPath);
    TestServer freshServer(fresh, limits);
    Client freshClient(freshServer.port());
    const QString clonePath = QStringLiteral("/dvmdb/v1/projects/clone-target");
    const QJsonObject provisionBody{{"projectId", QString::fromStdString(project.id)},
                                    {"name", "Revision transfer"}};
    check(freshClient.call("POST", clonePath, provisionBody).status == 200 &&
          freshClient.call("POST", clonePath, provisionBody).status == 200 &&
          fresh.listBranches(project.id).empty(),
          "first clone failed to provision an empty project with the source UUID");
    check(freshClient.call("POST", clonePath,
          QJsonObject{{"projectId", QString::fromStdString(project.id)},
                      {"name", "Wrong project name"}}).status == 409,
          "clone target silently renamed a provisioned project");
    const BinaryResponse firstImport = freshClient.callRaw(
        "POST", prefix, full.body, "application/vnd.aowis.revision-bundle-v2");
    check(firstImport.status == 201 && fresh.headV2(project.id, "main") == tip &&
          fresh.branchId(project.id, "main") == identity &&
          fresh.repositoryId() != source.repositoryId(),
          "first HTTP clone failed to preserve logical UUIDs and independent repository root");
    check(sender.callRaw("GET", prefix + "?base=not-a-uuid").status == 400,
          "invalid v2 incremental base accepted");
    check(sender.callRaw("GET", prefix + "?unused=1").status == 400,
          "unknown v2 export query argument accepted");
    const native::ProjectInfo unrelated = replica.createProjectV2("Unrelated project");
    const QString unrelatedPath = QStringLiteral("/dvmdb/v1/projects/") +
        QString::fromStdString(unrelated.id) + "/branches/main/bundle";
    check(receiver.callRaw("POST", unrelatedPath, full.body,
                           "application/vnd.aowis.revision-bundle-v2").status == 409,
          "cross-project HTTP revision bundle accepted");
    sender.abortDownload(prefix);
    const BinaryResponse advanced = receiver.callRaw(
        "POST", prefix, full.body, "application/vnd.aowis.revision-bundle-v2");
    check(advanced.status == 200 && replica.headV2(project.id, "main") == tip &&
          replica.branchId(project.id, "main") == identity,
          "HTTP UUIDv7 full import did not preserve HEAD/branch identity");
    check(receiver.callRaw("POST", prefix, full.body,
                           "application/vnd.aowis.revision-bundle-v2").status == 200,
          "HTTP full revision replay failed");
    const native::RevisionId base = tip;
    source.withRepository(project.id, [&](native::Repository& repository) {
        tip = repository.commitV2(*projectId, "main", {{key, native::Bytes{4, 5, 6}}}, base);
    });
    const BinaryResponse delta = sender.callRaw("GET", prefix + "?base=" +
                                              QString::fromStdString(base.toString()));
    check(delta.status == 200 && delta.body.startsWith("AOWREV22"),
          "HTTP UUIDv7 incremental export is not AOWREV22");
    QByteArray damaged = delta.body;
    check(damaged.size() > 144, "incremental fixture unexpectedly empty");
    damaged[damaged.size() - 1] = static_cast<char>(damaged.back() ^ 0x01);
    const BinaryResponse invalid = receiver.callRaw(
        "POST", prefix, damaged, "application/vnd.aowis.revision-bundle-v2");
    check(invalid.status >= 400 && invalid.status < 500 &&
          replica.headV2(project.id, "main") == base,
          "corrupt HTTP revision delta advanced recipient HEAD");
    const BinaryResponse imported = receiver.callRaw(
        "POST", prefix, delta.body, "application/vnd.aowis.revision-bundle-v2");
    check(imported.status == 200 && replica.headV2(project.id, "main") == tip &&
          replica.branchId(project.id, "main") == identity,
          "HTTP UUIDv7 incremental import lost identity");
    check(receiver.callRaw("POST", prefix, delta.body,
                           "application/vnd.aowis.revision-bundle-v2").status == 200,
          "HTTP UUIDv7 delta replay failed");
    check(receiver.callRaw("POST", prefix, QByteArray(4 * 1024 * 1024 + 1, 'x'),
                           "application/vnd.aowis.revision-bundle-v2").status == 413,
          "oversized revision upload was accepted");
}


void exerciseUnbornProjectHttp() {
    namespace native = aowis::dvmdb;
    TemporaryRoot temp;
    native::ProjectService service(temp.root());
    TestServer server(service, native::BundleLimits{4ULL * 1024ULL * 1024ULL, 10000, 1024ULL * 1024ULL});
    Client client(server.port());
    const Response repository = client.call("GET", "/dvmdb/v1/repository");
    check(repository.status == 200 && repository.json.keys() == QStringList({"repositoryId"})
          && repository.json.value("repositoryId").toString() ==
              QString::fromStdString(service.repositoryId()),
          "HTTP repository identity differs from native root UUID");
    const Response emptyTracking = client.call("GET", "/dvmdb/v1/repository/tracking");
    check(emptyTracking.status == 200 &&
          emptyTracking.json.value("repositoryId") == repository.json.value("repositoryId") &&
          emptyTracking.json.value("role").toString() == QStringLiteral("unconfigured") &&
          emptyTracking.json.value("remotes").toArray().isEmpty() &&
          emptyTracking.json.value("tracking").toArray().isEmpty(),
          "HTTP tracking metadata did not report an unconfigured repository");
    const std::string remoteId = native::UuidV7::generate().toString();
    const std::string trackedProject = native::ProjectId::generate().toString();
    const std::string trackedBranch = native::BranchId::generate().toString();
    service.configureRepositoryRoleV1(native::RepositoryRoleV1::Private);
    service.registerRemoteRepositoryV1(remoteId, "https://shared.example.test/dvmdb/v1");
    service.recordRemoteTrackingRefV1({remoteId, trackedProject, trackedBranch, std::nullopt},
                                      std::nullopt);
    const Response tracking = client.call("GET", "/dvmdb/v1/repository/tracking");
    const QJsonArray refs = tracking.json.value("tracking").toArray();
    check(tracking.status == 200 &&
          tracking.json.value("role").toString() == QStringLiteral("private") &&
          tracking.json.value("remotes").toArray().size() == 1 && refs.size() == 1 &&
          refs.at(0).toObject().value("remoteRepositoryId").toString() ==
              QString::fromStdString(remoteId) &&
          refs.at(0).toObject().value("projectId").toString() ==
              QString::fromStdString(trackedProject) &&
          refs.at(0).toObject().value("branchId").toString() ==
              QString::fromStdString(trackedBranch) &&
          refs.at(0).toObject().contains("observedHeadRevisionId") &&
          refs.at(0).toObject().value("observedHeadRevisionId").isNull(),
          "HTTP tracking metadata lost explicit unborn HEAD or remote identity");
    const QString projects = QStringLiteral("/dvmdb/v1/projects");
    check(client.call("POST", projects,
        QJsonObject{{"name", "bad unborn"}, {"unborn", "true"}}).status == 400,
        "HTTP accepted non-boolean unborn selector");
    check(client.call("POST", projects,
        QJsonObject{{"name", "null unborn"}, {"unborn", QJsonValue(QJsonValue::Null)}}).status == 400,
        "HTTP accepted null instead of an explicit unborn boolean");
    for (const QJsonObject& request : {
             QJsonObject{{"name", "legacy default"}},
             QJsonObject{{"name", "explicit committed"}, {"unborn", false}}}) {
        const Response legacy = client.call("POST", projects, request);
        check(legacy.status == 201, "HTTP committed project compatibility creation failed");
        const QString legacyHeadPath = projects + "/" + legacy.json.value("id").toString() +
                                       "/branches/main/head";
        const Response legacyHead = client.call("GET", legacyHeadPath);
        check(legacyHead.status == 200 &&
              native::RevisionId::parse(legacyHead.json.value("id").toString().toStdString()).has_value(),
              "HTTP committed project compatibility lost initial revision HEAD");
    }
    const Response created = client.call("POST", projects,
        QJsonObject{{"name", "Empty project"}, {"unborn", true}});
    const std::string project = created.json.value("id").toString().toStdString();
    check(created.status == 201 && native::ProjectId::parse(project).has_value(),
        "HTTP unborn project creation failed");
    const QString prefix = projects + "/" + QString::fromStdString(project);
    const QString headPath = prefix + "/branches/main/head";
    const QString historyPath = prefix + "/branches/main/history";
    const QString graphPath = prefix + "/revision-graph";
    const Response unbornHead = client.call("GET", headPath);
    check(unbornHead.status == 200 && unbornHead.json.contains("id") &&
          unbornHead.json.value("id").isNull(), "HTTP unborn HEAD is not explicit null");
    const Response history = client.call("GET", historyPath);
    check(history.status == 200 && history.json.value("revisions").toArray().isEmpty(),
          "HTTP unborn history must be empty");
    const Response graph = client.call("GET", graphPath + "?limit=1");
    const QJsonArray heads = graph.json.value("heads").toArray();
    check(graph.status == 200 && heads.size() == 1 &&
          heads.at(0).toObject().value("branch") == "main" &&
          heads.at(0).toObject().value("revision").isNull() &&
          heads.at(0).toObject().value("recordHash").isNull() &&
          native::BranchId::parse(heads.at(0).toObject().value("branchId").toString().toStdString()) &&
          graph.json.value("revisions").toArray().isEmpty() &&
          graph.json.value("complete").toBool() && graph.json.value("nextCursor").isNull(),
          "HTTP unborn graph must have one null HEAD and zero revisions");
    const std::optional<native::ProjectId> id = native::ProjectId::parse(project);
    native::Key key{};
    key[15] = 9;
    std::optional<native::RevisionId> first;
    service.withRepository(project, [&](native::Repository& repository) {
        first = repository.commitV2(*id, "main", {{key, native::Bytes{1, 2, 3}}}, std::nullopt);
    });
    check(first.has_value(), "HTTP first unborn commit returned no revision");
    const QString firstId = QString::fromStdString(first->toString());
    check(client.call("GET", headPath).json.value("id").toString() == firstId,
          "HTTP HEAD failed to transition from null after first commit");
    const Response firstRevision = client.call("GET", prefix + "/revisions/" + firstId);
    check(firstRevision.status == 200 &&
          firstRevision.json.value("parents").toArray().isEmpty(),
          "HTTP first unborn commit must have no parents");
    const Response after = client.call("GET", graphPath);
    check(after.status == 200 && after.json.value("heads").toArray().size() == 1 &&
          after.json.value("heads").toArray().at(0).toObject().value("revision") == firstId &&
          after.json.value("revisions").toArray().size() == 1,
          "HTTP graph did not expose first committed revision");
}

void exerciseUuidv7HistoryHttp() {
    namespace native = aowis::dvmdb;
    TemporaryRoot temp;
    native::ProjectService service(temp.root());
    TestServer server(service, native::BundleLimits{4ULL * 1024ULL * 1024ULL, 10000, 1024ULL * 1024ULL});
    Client client(server.port());
    check(client.call("POST", "/dvmdb/v1/projects",
                      QJsonObject{{"name", "invalid"}, {"revisionFormat", "sha"}}).status == 400,
          "unexpected revision format accepted");
    check(client.call("POST", "/dvmdb/v1/projects",
                      QJsonObject{{"name", "format override"}, {"revisionFormat", "uuidv7"}}).status == 400,
          "canonical UUIDv7 project creation accepted obsolete revisionFormat selector");
    const Response created = client.call("POST", "/dvmdb/v1/projects",
        QJsonObject{{"name", "V2 API"}});
    const std::string project = created.json.value("id").toString().toStdString();
    check(created.status == 201 && native::ProjectId::parse(project).has_value(),
          "HTTP did not create UUIDv7 project");
    const QString basePath = QStringLiteral("/dvmdb/v1/projects/") +
                             QString::fromStdString(project);
    const QString graphPath = basePath + "/revision-graph";
    const QString mainHeadPath = basePath + "/branches/main/head";
    const Response head = client.call("GET", mainHeadPath);
    check(head.status == 200 && native::RevisionId::parse(head.json.value("id").toString().toStdString()),
          "HTTP V2 branch HEAD is not UUIDv7");
    const QString rootId = head.json.value("id").toString();
    check(client.call("GET", basePath + "/branches/main/head-v2").status == 404,
          "obsolete V2 HEAD alias still registered");
    check(client.call("GET", basePath + "/revisions/" + QString(64, '0')).status == 400,
          "canonical revision lookup accepted a SHA-256 revision identity");
    check(client.call("GET", basePath + "/revisions/garbage").status == 400,
          "HTTP v2 revision lookup accepted non-UUID identity");
    const Response initial = client.call("GET", basePath + "/revisions/" + rootId);
    check(initial.status == 200 && initial.json.value("recordHash").toString().size() == 64 &&
          initial.json.value("parents").toArray().isEmpty(),
          "HTTP V2 root detail is invalid");
    const Response feature = client.call("POST", basePath + "/branches",
        QJsonObject{{"name", "feature"}, {"fromBranch", "main"}});
    const Response initialGraph = client.call("GET", graphPath + "?limit=1");
    QString mainBranchId;
    for (const QJsonValue& value : initialGraph.json.value("heads").toArray()) {
        const QJsonObject item = value.toObject();
        if (item.value("branch").toString() == QStringLiteral("main"))
            mainBranchId = item.value("branchId").toString();
    }
    check(initialGraph.status == 200 &&
          native::BranchId::parse(mainBranchId.toStdString()).has_value(),
          "HTTP V2 initial graph did not expose the main branch identity");
    check(feature.status == 201 && feature.json.value("id").toString() == rootId &&
          native::BranchId::parse(feature.json.value("branchId").toString().toStdString()) &&
          feature.json.value("originBranchId").toString() == mainBranchId &&
          feature.json.value("forkRevisionId").toString() == rootId,
          "HTTP V2 branch creation did not return persisted fork provenance");
    check(client.call("POST", basePath + "/branches",
        QJsonObject{{"name", "orphan"}}).status == 400,
          "source-less V2 fork allowed for nonempty project");
    check(client.call("POST", basePath + "/branches",
        QJsonObject{{"name", "bad"}, {"fromBranch", "main"}, {"revision", "1234"}}).status == 400,
          "invalid historical UUID was accepted");
    const std::optional<native::ProjectId> id = native::ProjectId::parse(project);
    native::RevisionId mainTip = *native::RevisionId::parse(rootId.toStdString());
    native::Key first{};
    first[15] = 7;
    service.withRepository(project, [&](native::Repository& repository) {
        mainTip = repository.commitV2(*id, "main", {{first, native::Bytes{1, 2}}}, mainTip);
    });
    const QString newer = QString::fromStdString(mainTip.toString());
    const Response graphFirst = client.call("GET", graphPath + "?limit=1");
    check(graphFirst.status == 200 && graphFirst.json.value("heads").toArray().size() == 2 &&
          graphFirst.json.value("revisions").toArray().size() == 1 &&
          !graphFirst.json.value("complete").toBool(), "UUIDv7 graph first page malformed");
    const QString cursor = graphFirst.json.value("nextCursor").toString();
    check(!cursor.isEmpty(), "UUIDv7 graph did not return pagination cursor");
    const QString differentProject = QString::fromStdString(service.createProjectV2("Other").id);
    check(client.call("GET", "/dvmdb/v1/projects/" + differentProject +
                      "/revision-graph?cursor=" + cursor).status == 400,
          "UUIDv7 graph cursor was accepted across projects");
    QString changedCursor = cursor;
    changedCursor[changedCursor.size() - 2] = changedCursor.at(changedCursor.size() - 2) == QChar('A')
        ? QChar('B') : QChar('A');
    check(client.call("GET", graphPath + "?cursor=" + changedCursor).status == 400,
          "forged UUIDv7 graph cursor was accepted");
    // Advance main during paging; the pinned first page must not be rewritten.
    service.withRepository(project, [&](native::Repository& repository) {
        mainTip = repository.commitV2(*id, "main", {{first, native::Bytes{3, 4}}}, mainTip);
    });
    const Response graphSecond = client.call("GET", graphPath + "?limit=1&cursor=" + cursor);
    check(graphSecond.status == 200 && graphSecond.json.value("heads") == graphFirst.json.value("heads") &&
          graphSecond.json.value("complete").toBool() &&
          graphSecond.json.value("revisions").toArray().size() == 1,
          "UUIDv7 graph cursor failed to preserve a pinned HEAD snapshot");
    const Response latest = client.call("GET", graphPath);
    check(latest.status == 200 && latest.json.value("revisions").toArray().size() == 3 &&
          latest.json.value("heads").toArray().size() == 2,
          "UUIDv7 graph did not enumerate updated ancestry");
    const Response history = client.call("GET", basePath + "/branches/main/history?limit=1");
    check(history.status == 200 && history.json.value("revisions").toArray().size() == 1 &&
          history.json.value("revisions").toArray().at(0).toObject().value("id").toString() ==
          QString::fromStdString(mainTip.toString()), "V2 history did not begin at current HEAD");
    const Response historic = client.call("POST", basePath + "/branches",
        QJsonObject{{"name", "historic"}, {"fromBranch", "main"}, {"revision", rootId}});
    check(historic.status == 201 && historic.json.value("id").toString() == rootId &&
          native::BranchId::parse(historic.json.value("branchId").toString().toStdString()) &&
          historic.json.value("originBranchId").toString() == mainBranchId &&
          historic.json.value("forkRevisionId").toString() == rootId,
          "V2 historical branching did not return exact persisted fork provenance");
    const Response renamed = client.call("PATCH", basePath + "/branches/historic",
        QJsonObject{{"name", "archived"}, {"expectedHead", rootId}});
    check(renamed.status == 200 &&
          client.call("GET", basePath + "/branches/archived/head").json.value("id").toString() == rootId,
          "V2 rename moved HEAD or lost branch");
    check(client.call("PATCH", basePath + "/branches/archived",
        QJsonObject{{"name", "bad-again"}, {"expectedHead", newer}}).status == 409,
          "V2 stale rename did not conflict");

    // A separate v2 project starts with an empty root, so the HTTP model
    // operations are exercised against actual UUIDv7 HEADs rather than
    // the SHA-only model API or a fixture-created raw revision.
    const Response modelProject = client.call("POST", "/dvmdb/v1/projects",
        QJsonObject{{"name", "V2 REST commits"}});
    check(modelProject.status == 201, "V2 model test project creation failed");
    const QString modelPath = QStringLiteral("/dvmdb/v1/projects/") +
                              modelProject.json.value("id").toString();
    const QString modelRoot = client.call("GET", modelPath + "/branches/main/head")
                              .json.value("id").toString();
    const Response modelFeature = client.call("POST", modelPath + "/branches",
        QJsonObject{{"name", "feature"}, {"fromBranch", "main"}});
    check(modelFeature.status == 201 && modelFeature.json.value("id").toString() == modelRoot,
          "V2 model feature branch creation failed");
    const QString keyA = QStringLiteral("00000000-0000-0000-0000-000000000007");
    const QString keyB = QStringLiteral("00000000-0000-0000-0000-000000000008");
    const QString mainCommitPath = modelPath + "/branches/main/model-commits";
    const QString featureCommitPath = modelPath + "/branches/feature/model-commits";
    const QJsonObject addA{{"uuid", keyA}, {"kind", 31},
                           {"references", QJsonArray{}}, {"payloadBase64", "Cw=="}};
    const QJsonObject addB{{"uuid", keyB}, {"kind", 31},
                           {"references", QJsonArray{}}, {"payloadBase64", "Fg=="}};
    const QJsonObject firstCommit{{"expectedHead", modelRoot},
                                  {"changes", QJsonArray{addA}}};
    check(client.call("POST", mainCommitPath,
        QJsonObject{{"expectedHead", modelRoot}, {"changes", QJsonArray{}}}).status == 400,
        "V2 REST accepted empty model change batch");
    check(client.call("POST", mainCommitPath,
        QJsonObject{{"expectedHead", modelRoot}, {"changes", QJsonArray{
            QJsonObject{{"uuid", keyA}, {"kind", 31}, {"references", QJsonArray{}},
                        {"payloadBase64", "invalid***"}}}}}).status == 400,
        "V2 REST accepted invalid model base64");
    const Response modelFirst = client.call("POST", mainCommitPath, firstCommit);
    const QString modelFirstId = modelFirst.json.value("id").toString();
    check(modelFirst.status == 201 && native::RevisionId::parse(modelFirstId.toStdString()),
          "V2 REST model commit did not create revision UUIDv7");
    check(client.call("POST", mainCommitPath, firstCommit).status == 409,
          "V2 REST accepted stale model expectedHead");
    const Response modelDiff = client.call("GET", modelPath + "/diff?from=" + modelRoot +
                                            "&to=" + modelFirstId);
    check(modelDiff.status == 200 && modelDiff.json.value("changes").toArray().size() == 1 &&
          modelDiff.json.value("changes").toArray().at(0).toObject().value("uuid").toString() == keyA,
          "V2 REST immutable diff did not include added model");
    check(client.call("GET", modelPath + "/diff?from=garbage&to=" + modelFirstId).status == 400,
          "V2 REST immutable diff accepted non-UUID identity");
    const Response modelSecond = client.call("POST", featureCommitPath,
        QJsonObject{{"expectedHead", modelRoot}, {"changes", QJsonArray{addB}}});
    const QString featureCommitId = modelSecond.json.value("id").toString();
    check(modelSecond.status == 201 && native::RevisionId::parse(featureCommitId.toStdString()),
          "V2 REST model branch commit failed");
    const QString mergePath = modelPath + "/merges";
    const QJsonObject mergeRequest{{"target", "main"}, {"source", "feature"},
        {"expectedTargetHead", modelFirstId}, {"expectedSourceHead", featureCommitId}};
    check(client.call("POST", mergePath,
        QJsonObject{{"target", "main"}, {"source", "feature"},
                    {"expectedTargetHead", modelRoot}, {"expectedSourceHead", featureCommitId}}).status == 409,
          "V2 REST merge accepted stale target HEAD");
    const Response merged = client.call("POST", mergePath, mergeRequest);
    const QString mergedId = merged.json.value("head").toString();
    check(merged.status == 200 && merged.json.value("status").toString() == "merged" &&
          native::RevisionId::parse(mergedId.toStdString()),
          "V2 REST disjoint model merge did not publish UUIDv7 merge revision");
    const Response mergeRecord = client.call("GET", modelPath + "/revisions/" + mergedId);
    check(mergeRecord.status == 200 && mergeRecord.json.value("parents").toArray().size() == 2,
          "V2 REST merge revision did not retain both parents");
    check(client.call("GET", modelPath + "/branches/feature/head")
           .json.value("id").toString() == featureCommitId,
          "V2 REST merge unexpectedly advanced source branch HEAD");
    const Response mergedDiff = client.call("GET", modelPath + "/diff?from=" +
                                              modelFirstId + "&to=" + mergedId);
    check(mergedDiff.status == 200 && mergedDiff.json.value("changes").toArray().size() == 1 &&
          mergedDiff.json.value("changes").toArray().at(0).toObject().value("uuid").toString() == keyB,
          "V2 REST merged revision diff omitted source change");
    const Response otherBranch = client.call("POST", modelPath + "/branches",
        QJsonObject{{"name", "competing"}, {"fromBranch", "main"}});
    check(otherBranch.status == 201 && otherBranch.json.value("id").toString() == mergedId,
          "V2 REST conflict test branch creation failed");
    const QJsonObject overwriteA{{"uuid", keyA}, {"kind", 31},
        {"references", QJsonArray{}}, {"payloadBase64", "Mw=="}};
    const QJsonObject overwriteOther{{"uuid", keyA}, {"kind", 31},
        {"references", QJsonArray{}}, {"payloadBase64", "RA=="}};
    const Response mainAdvanced = client.call("POST", mainCommitPath,
        QJsonObject{{"expectedHead", mergedId}, {"changes", QJsonArray{overwriteA}}});
    const Response otherAdvanced = client.call("POST",
        modelPath + "/branches/competing/model-commits",
        QJsonObject{{"expectedHead", mergedId}, {"changes", QJsonArray{overwriteOther}}});
    const QString mainAdvancedId = mainAdvanced.json.value("id").toString();
    const QString otherAdvancedId = otherAdvanced.json.value("id").toString();
    check(mainAdvanced.status == 201 && otherAdvanced.status == 201,
          "V2 REST conflict fixture model commits failed");
    const Response conflict = client.call("POST", mergePath,
        QJsonObject{{"target", "main"}, {"source", "competing"},
                    {"expectedTargetHead", mainAdvancedId},
                    {"expectedSourceHead", otherAdvancedId}});
    check(conflict.status == 409 && conflict.json.value("status").toString() == "conflicts" &&
          conflict.json.value("conflicts").toArray().size() == 1,
          "V2 REST concurrent model conflict not reported");
    check(client.call("GET", modelPath + "/branches/main/head")
                .json.value("id").toString() == mainAdvancedId &&
          client.call("GET", modelPath + "/branches/competing/head")
                .json.value("id").toString() == otherAdvancedId,
          "V2 REST conflict changed a branch HEAD");
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
    // The hydraulic merge endpoint must apply its domain validator, not
    // silently accept generic, non-hydraulic indexed models as networks.
    const QString hydraulicMergePath = modelPath + "/hydraulic-merges";
    const QJsonObject hydraulicRequest{{"target", "main"}, {"source", "competing"},
        {"expectedTargetHead", mainAdvancedId}, {"expectedSourceHead", otherAdvancedId}};
    const Response hydraulicConflict = client.call("POST", hydraulicMergePath, hydraulicRequest);
    check(hydraulicConflict.status == 409 &&
          hydraulicConflict.json.value("status").toString() == "conflicts",
          "UUIDv7 hydraulic REST merge failed to report conflicts");
    const Response invalidHydraulicUuid = client.call("POST", hydraulicMergePath,
        QJsonObject{{"target", "main"}, {"source", "competing"},
                    {"expectedTargetHead", "not-a-uuid"},
                    {"expectedSourceHead", otherAdvancedId}});
    check(invalidHydraulicUuid.status == 400,
          "UUIDv7 hydraulic REST merge accepted an invalid revision UUID");
    const Response duplicateObjects = client.call("POST", hydraulicMergePath,
        QJsonObject{{"target", "main"}, {"source", "competing"},
                    {"expectedTargetHead", mainAdvancedId},
                    {"expectedSourceHead", otherAdvancedId},
                    {"objectChoices", QJsonArray{
                        QJsonObject{{"entityUuid", keyA}, {"side", "source"}},
                        QJsonObject{{"entityUuid", keyA}, {"side", "target"}}}}});
    check(duplicateObjects.status == 400,
          "UUIDv7 hydraulic REST merge accepted contradictory object choices");
    const Response domainRejected = client.call("POST", hydraulicMergePath,
        QJsonObject{{"target", "main"}, {"source", "competing"},
                    {"expectedTargetHead", mainAdvancedId},
                    {"expectedSourceHead", otherAdvancedId},
                    {"objectChoices", QJsonArray{
                        QJsonObject{{"entityUuid", keyA}, {"side", "source"}}}}});
    check(domainRejected.status == 409 && domainRejected.json.value("status") == "conflicts",
          "UUIDv7 hydraulic merge skipped whole-network domain validation");
    check(client.call("GET", modelPath + "/branches/main/head")
              .json.value("id").toString() == mainAdvancedId &&
          client.call("GET", modelPath + "/branches/competing/head")
              .json.value("id").toString() == otherAdvancedId,
          "rejected hydraulic V2 REST merge advanced a branch HEAD");
#endif



}


void exerciseWorkingCopyHttp() {
    namespace native = aowis::dvmdb;
    TemporaryRoot root;
    std::string projectId;
    std::string draftId;
    const QString firstSession = QString::fromStdString(native::UuidV7::generate().toString());
    const QString otherSession = QString::fromStdString(native::UuidV7::generate().toString());
    const QString modelUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        native::ProjectService service(root.root());
        const native::ProjectInfo project = service.createUnbornProjectV2("private working-copy HTTP");
        projectId = project.id;
        TestServer server(service, native::BundleLimits{4ULL * 1024ULL * 1024ULL,
                                                         10000ULL, 1024ULL * 1024ULL});
        Client client(server.port());
        const QString listPath = QStringLiteral("/dvmdb/v1/working-copies");
        const QJsonObject create{{"projectId", QString::fromStdString(projectId)},
                                 {"branch", "main"}, {"baseRevision", QJsonValue::Null},
                                 {"sessionId", firstSession}};
        check(client.call("POST", listPath, create).status == 409,
              "non-private repository accepted draft creation");
        service.configureRepositoryRoleV1(native::RepositoryRoleV1::Private);
        check(client.call("POST", listPath,
              QJsonObject{{"projectId", QString::fromStdString(projectId)},
                          {"branch", "main"}, {"sessionId", firstSession}}).status == 400,
              "draft creation accepted missing explicit unborn base");
        const Response created = client.call("POST", listPath, create);
        const QString id = created.json.value("id").toString();
        check(created.status == 201 && native::UuidV7::parse(id.toStdString()).has_value() &&
              created.json.value("baseRevision").isNull() &&
              created.json.value("generation") == QStringLiteral("0") &&
              !created.json.value("dirty").toBool(),
              "HTTP unborn draft creation failed");
        draftId = id.toStdString();
        const QString detailPath = listPath + "/" + id;
        check(client.call("GET", listPath).json.value("workingCopies").toArray().size() == 1 &&
              client.call("GET", listPath).json.value("workingCopies").toArray().at(0).toObject()
                  .value("changes").isUndefined() &&
              client.call("GET", detailPath).json.value("id") == id &&
              client.call("GET", listPath + "/not-a-uuid").status == 400,
              "HTTP working-copy list/read/UUID validation failed");
        const QString leasePath = detailPath + "/lease";
        const Response leased = client.call("POST", leasePath,
            QJsonObject{{"sessionId", firstSession}, {"seconds", 30}});
        const QString token = leased.json.value("token").toString();
        check(leased.status == 200 && native::UuidV7::parse(token.toStdString()).has_value() &&
              leased.json.value("generation") == QStringLiteral("0") &&
              !leased.json.value("expiresAtMs").toString().isEmpty() &&
              client.call("POST", leasePath,
                  QJsonObject{{"sessionId", otherSession}, {"seconds", 30}}).status == 409,
              "HTTP draft writer lease was not exclusive");
        const QJsonObject entity{{"uuid", modelUuid}, {"kind", 4},
                                 {"references", QJsonArray{}}, {"payloadBase64", "AQID"}};
        const QString savePath = detailPath + "/save";
        const QJsonObject save{{"sessionId", firstSession}, {"fencingToken", token},
                               {"expectedGeneration", "0"}, {"changes", QJsonArray{entity}}};
        check(client.call("POST", savePath,
            QJsonObject{{"sessionId", firstSession}, {"fencingToken", token},
                        {"expectedGeneration", 0}, {"changes", QJsonArray{entity}}}).status == 400,
              "HTTP draft accepted unsafe numeric generation");
        check(client.call("POST", savePath,
            QJsonObject{{"sessionId", firstSession}, {"fencingToken", token},
                        {"expectedGeneration", "0"}, {"changes", QJsonArray{entity, entity}}}).status == 400,
              "HTTP draft accepted duplicate model UUIDs in one save");
        const Response persisted = client.call("POST", savePath, save);
        const QJsonArray persistedChanges = persisted.json.value("changes").toArray();
        check(persisted.status == 200 && persisted.json.value("generation") == QStringLiteral("1") &&
              persisted.json.value("dirty") == QJsonValue(true) && persistedChanges.size() == 1 &&
              persistedChanges.at(0).toObject().value("uuid") == modelUuid &&
              persistedChanges.at(0).toObject().value("payloadBase64") == QStringLiteral("AQID"),
              "HTTP draft save did not retain opaque model payload");
        check(client.call("POST", savePath, save).status == 409,
              "HTTP accepted stale draft generation");
        check(client.call("POST", savePath,
              QJsonObject{{"sessionId", firstSession}, {"fencingToken", otherSession},
                          {"expectedGeneration", "1"}, {"changes", QJsonArray{entity}}}).status == 409,
              "HTTP accepted invalid fencing token");
        check(client.call("GET", detailPath).json.value("generation") == QStringLiteral("1") &&
              client.call("GET", detailPath).json.value("changes").toArray().size() == 1 &&
              !client.call("GET", detailPath).json.contains("token") &&
              !service.optionalHeadV2(projectId, "main").has_value() &&
              service.revisionGraphV2(projectId).revisions.empty(),
              "HTTP draft save changed committed HEAD or lost candidate");
        const Response released = client.call("POST", detailPath + "/release",
            QJsonObject{{"sessionId", firstSession}, {"fencingToken", token}});
        check(released.status == 200 && released.json.value("released") == QJsonValue(true) &&
              client.call("POST", detailPath + "/release",
                  QJsonObject{{"sessionId", firstSession}, {"fencingToken", token}}).status == 409,
              "HTTP lease release did not fence stale token");
        const Response taken = client.call("POST", leasePath,
            QJsonObject{{"sessionId", otherSession}, {"seconds", 30}});
        check(taken.status == 200 && taken.json.value("token") != token &&
              taken.json.value("generation") == QStringLiteral("1") &&
              client.call("POST", savePath,
                  QJsonObject{{"sessionId", firstSession}, {"fencingToken", token},
                              {"expectedGeneration", "1"}, {"changes", QJsonArray{entity}}}).status == 409,
              "HTTP lease takeover did not fence original session");
        const native::Bytes event = native::test::editEvent(
            native::UuidV7::generate(), native::UuidV7::generate());
        const QByteArray eventBytes(reinterpret_cast<const char*>(event.data()),
                                    static_cast<qsizetype>(event.size()));
        const QString encodedEvent = QString::fromLatin1(eventBytes.toBase64());
        const QString eventSavePath = detailPath + "/save-events";
        const QJsonObject eventSave{{"sessionId", otherSession},
                                    {"fencingToken", taken.json.value("token")},
                                    {"expectedGeneration", "1"},
                                    {"changes", QJsonArray{entity}},
                                    {"events", QJsonArray{encodedEvent}}};
        const Response savedEvents = client.call("POST", eventSavePath, eventSave);
        check(savedEvents.status == 200 &&
              savedEvents.json.value("generation") == QStringLiteral("2") &&
              savedEvents.json.value("eventCount") == QStringLiteral("1") &&
              savedEvents.json.value("acceptedEventIds").toArray().size() == 1 &&
              !savedEvents.json.contains("changes") && !savedEvents.json.contains("events") &&
              client.call("GET", detailPath).json.value("events").toArray().at(0)
                  .toObject().value("payloadBase64") == encodedEvent,
              "HTTP atomic event Save did not persist event and candidate together");
        const Response replay = client.call("POST", eventSavePath, eventSave);
        check(replay.status == 200 && replay.json.value("generation") == QStringLiteral("2") &&
              replay.json.value("acceptedEventIds").toArray().size() == 1,
              "HTTP lost-acknowledgement event replay was not idempotent");
        check(client.call("POST", savePath,
              QJsonObject{{"sessionId", otherSession},
                          {"fencingToken", taken.json.value("token")},
                          {"expectedGeneration", "2"},
                          {"changes", QJsonArray{entity}}}).status == 409,
              "HTTP unjournalled save bypassed existing event journal");
        check(client.call("POST", eventSavePath,
              QJsonObject{{"sessionId", otherSession},
                          {"fencingToken", taken.json.value("token")},
                          {"expectedGeneration", "2"},
                          {"changes", QJsonArray{entity}},
                          {"events", QJsonArray{"AAAA"}}}).status == 400,
              "HTTP accepted malformed MODEL event bytes");
        server.stop();
    }
    {
        native::ProjectService reopened(root.root());
        const native::WorkingCopyInfoV1 draft = reopened.workingCopyV1(draftId);
        check(draft.generation == 2 && draft.dirty && draft.journal.size() == 1 &&
              draft.candidateChanges.size() == 1 &&
              draft.candidateChanges.front().record.has_value() &&
              draft.candidateChanges.front().record->payload == native::Bytes{1, 2, 3} &&
              !reopened.optionalHeadV2(projectId, "main").has_value(),
              "HTTP draft candidate did not survive repository restart");
        TestServer server(reopened, native::BundleLimits{4ULL * 1024ULL * 1024ULL,
                                                         10000ULL, 1024ULL * 1024ULL});
        Client client(server.port());
        const Response restored = client.call("GET",
            QStringLiteral("/dvmdb/v1/working-copies/") + QString::fromStdString(draftId));
        check(restored.status == 200 && restored.json.value("generation") == QStringLiteral("2") &&
              restored.json.value("events").toArray().size() == 1 &&
              restored.json.value("changes").toArray().size() == 1 &&
              restored.json.value("baseRevision").isNull(),
              "HTTP draft GET lost saved candidate after service restart");
        server.stop();
    }
}

#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
void exerciseHydraulicDraftMembershipHttp() {
    namespace native = aowis::dvmdb;
    TemporaryRoot root;
    native::ProjectService service(root.root());
    const native::ProjectInfo project = service.createUnbornProjectV2("draft-membership-test");
    const native::ProjectId projectUuid = *native::ProjectId::parse(project.id);
    const QUuid networkUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    const QUuid oldUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    const QUuid newUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    const QString networkId = networkUuid.toString(QUuid::WithoutBraces);
    const QString oldId = oldUuid.toString(QUuid::WithoutBraces);
    const QString newId = newUuid.toString(QUuid::WithoutBraces);
    NetworkHydraulic network;
    network.uuid = networkUuid;
    network.id = QStringLiteral("draft-membership");
    HydraulicNodeJunction initial;
    initial.uuid = oldUuid;
    initial.id = QStringLiteral("J1");
    initial.elevation_m = 20;
    network.nodes_junctions.append(initial);
    // New projects have an unborn HEAD: establish a real empty model revision
    // before exercising the committed-network fixture.
    check(!service.optionalHeadV2(project.id, "main").has_value(),
          "new hydraulic fixture project should have an unborn HEAD");
    const native::RevisionId head = service.commitModelV2(project.id, "main", {});
    check(service.optionalHeadV2(project.id, "main") == head,
          "first hydraulic fixture commit did not establish the HEAD");
    native::RevisionId base = head;
    service.withRepository(project.id, [&](native::Repository& repository) {
        base = native::hydraulic::saveHydraulicNetworkV2(repository, projectUuid,
                                                         "main", network, head);
    });
    service.configureRepositoryRoleV1(native::RepositoryRoleV1::Private);
    TestServer server(service, native::BundleLimits{4ULL * 1024ULL * 1024ULL,
                                                     10000ULL, 1024ULL * 1024ULL});
    Client client(server.port());
    const QString session = QString::fromStdString(native::UuidV7::generate().toString());
    const QString listPath = QStringLiteral("/dvmdb/v1/working-copies");
    const Response created = client.call("POST", listPath,
        QJsonObject{{"projectId", QString::fromStdString(project.id)},
                    {"branch", "main"}, {"baseRevision", QString::fromStdString(base.toString())},
                    {"sessionId", session}});
    check(created.status == 201, "hydraulic draft checkout failed");
    const QString path = listPath + "/" + created.json.value("id").toString();
    const Response leased = client.call("POST", path + "/lease",
                                        QJsonObject{{"sessionId", session}, {"seconds", 60}});
    check(leased.status == 200, "hydraulic draft lease failed");
    const QString token = leased.json.value("token").toString();
    HydraulicNodeJunction added;
    added.uuid = newUuid;
    added.id = QStringLiteral("J2");
    added.elevation_m = 55;
    const QByteArray encoded = native::ModelCodec<HydraulicNodeJunction>::encode(added);
    const QJsonObject addition{{"uuid", newId}, {"kind", 1}, {"references", QJsonArray{}},
                               {"payloadBase64", QString::fromLatin1(encoded.toBase64())}};
    const QJsonObject insertion{{"networkId", networkId}, {"entityId", newId},
                                {"kind", 1}, {"collectionIndex", 9}, {"inserted", true}};
    const std::function<QString(const QUuid&, const QString&,
                                std::optional<QByteArray>, std::optional<QByteArray>)> eventFrame =
        [](const QUuid& member, const QString& action,
           std::optional<QByteArray> before, std::optional<QByteArray> after) {
        aowis::model::ModelEditTransaction transaction;
        transaction.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
        transaction.operation = action;
        transaction.edits.append(aowis::model::ModelEntityEdit{
            member, QStringLiteral("HydraulicNodeJunction"), std::move(before), std::move(after)});
        QString encodeError;
        const QByteArray frame = aowis::model::ModelEditTransactionCodec::encode(
            transaction, &encodeError);
        if (frame.isEmpty())
            throw std::runtime_error("could not encode hydraulic member edit event: " +
                                     encodeError.toStdString());
        return QString::fromLatin1(frame.toBase64());
    };
    const QString firstEvent = eventFrame(newUuid, QStringLiteral("hydraulic.junction.create"),
                                          std::nullopt, encoded);
    const QJsonObject first{{"sessionId", session}, {"fencingToken", token},
                            {"expectedGeneration", "0"}, {"changes", QJsonArray{addition}},
                            {"events", QJsonArray{firstEvent}},
                            {"membership", QJsonArray{insertion}}};
    QJsonObject invalid = first;
    invalid.insert("membership", QJsonArray{QJsonObject{
        {"networkId", networkId}, {"entityId", newId}, {"kind", 4},
        {"collectionIndex", 9}, {"inserted", true}}});
    check(client.call("POST", path + "/save-events", invalid).status == 400,
          "mismatched membership kind was accepted");
    invalid = first;
    invalid.insert("membership", QJsonArray{QJsonObject{
        {"networkId", networkId}, {"entityId", newId}, {"kind", 1},
        {"collectionIndex", 9}, {"inserted", false}}});
    check(client.call("POST", path + "/save-events", invalid).status == 400,
          "membership deletion without model tombstone accepted");
    invalid = first;
    invalid.insert("events", QJsonArray{eventFrame(
        newUuid, QStringLiteral("hydraulic.junction.create"), std::nullopt,
        QByteArrayLiteral("not a canonical junction"))});
    check(client.call("POST", path + "/save-events", invalid).status == 400 &&
          client.call("GET", path).json.value("generation") == QStringLiteral("0"),
          "mismatched MODEL after-image was published");
    const Response saved = client.call("POST", path + "/save-events", first);
    check(saved.status == 200 && saved.json.value("generation") == QStringLiteral("1") &&
          saved.json.value("eventCount") == QStringLiteral("1"),
          "hydraulic entity and membership did not save atomically");
    const Response replay = client.call("POST", path + "/save-events", first);
    check(replay.status == 200 && replay.json.value("generation") == QStringLiteral("1"),
          "hydraulic membership lost-response replay duplicated event");
    const QJsonObject details = client.call("GET", path).json;
    bool nativeChunk = false;
    for (const QJsonValue& value : details.value("changes").toArray())
        nativeChunk |= value.toObject().value("kind").toInt() == 101;
    check(nativeChunk && details.value("events").toArray().size() == 1 &&
          !details.value("changes").toArray().isEmpty(),
          "hydraulic membership chunk not included in durable candidate overlay");
    const QJsonObject deletion{{"uuid", oldId}, {"delete", true}};
    const QJsonObject removal{{"networkId", networkId}, {"entityId", oldId},
                              {"kind", 1}, {"collectionIndex", 9}, {"inserted", false}};
    const QString secondEvent = eventFrame(oldUuid, QStringLiteral("hydraulic.junction.delete"),
                                           native::ModelCodec<HydraulicNodeJunction>::encode(initial),
                                           std::nullopt);
    const QJsonObject second{{"sessionId", session}, {"fencingToken", token},
                             {"expectedGeneration", "1"}, {"changes", QJsonArray{deletion}},
                             {"events", QJsonArray{secondEvent}},
                             {"membership", QJsonArray{removal}}};
    check(client.call("POST", path + "/save-events", second).status == 200 &&
          client.call("POST", path + "/save-events", second).status == 200 &&
          client.call("GET", path).json.value("generation") == QStringLiteral("2") &&
          service.optionalHeadV2(project.id, "main") == base,
          "hydraulic deletion or replay modified committed HEAD");
    server.stop();
    const native::WorkingCopyInfoV1 persisted = service.workingCopyV1(
        created.json.value("id").toString().toStdString());
    check(persisted.generation == 2 && persisted.journal.size() == 2 &&
          persisted.candidateChanges.size() >= 3,
          "hydraulic member changes not durable as one candidate and journal");
}
#endif

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        check(argc == 2, "expected path to aowis-server-dvmdb executable");
        // Canonical HTTP routes accept UUIDv7 revisions only. The historical
        // SHA-revision HTTP fixtures above are intentionally not executed:
        // they exercised removed endpoints and must not force legacy routes
        // back into the public API. Native SHA storage fixtures remain in
        // their dedicated lower-level regression suites until service cutover.
        exerciseTransferStagingBudget();
        exerciseRevisionBundleV2Http();
        exerciseUnbornProjectHttp();
        exerciseWorkingCopyHttp();
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
        exerciseHydraulicDraftMembershipHttp();
#endif
        exerciseUuidv7HistoryHttp();
        exerciseExecutable(QString::fromLocal8Bit(argv[1]));
        std::cout << "HTTP project service tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HTTP service test failed: " << error.what() << '\n';
        return 1;
    }
}
