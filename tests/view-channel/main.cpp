// LogosViewChannel: listens wherever Qt puts local sockets, and serves its
// models ONLY to connections that present a secret endpoint() issued.
//
// 1. macOS caps a unix socket's full path at 104 bytes and Qt puts it under
//    $TMPDIR, which on macOS is already ~50 characters. A channel named
//    "logos_view_<module>_<32 hex>" did not fit: listen failed and the backend
//    crashed publishing on a dead host. A macOS-length TMPDIR reproduces it.
// 2. The socket name is discoverable by listing that directory, so it is not
//    what keeps other processes out: a connection with no secret, a wrong one,
//    or a reused one must never reach the models.
// 3. A killed backend leaves its socket file behind. A new channel removes
//    such files, but never a fresh one, a live one or one it did not name.
#include "logos_view_channel.h"

#include <QAbstractItemModelReplica>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QRemoteObjectNode>
#include <QSet>
#include <QStringListModel>

#include <cstdio>
#include <functional>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; } } while (0)

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* items READ items CONSTANT)
public:
    QAbstractItemModel* items() { return &m_items; }
    QStringListModel m_items{ QStringList{ QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") } };
};

// What a SIGKILLed channel leaves: a socket file nobody listens on.
static bool makeDeadSocket(const QString& path)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const QByteArray p = path.toUtf8();
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", p.constData());
    const bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return ok;
}

static void backdate(const QString& path)
{
    timeval old[2];
    ::gettimeofday(&old[0], nullptr);
    old[0].tv_sec -= 3600;
    old[1] = old[0];
    ::utimes(path.toUtf8().constData(), old);
}

static bool pumpUntil(const std::function<bool()>& done, int ms)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

// Connects like the view does: secret first, QtRO only after "ok".
// Returns the model's row count as the client sees it, or -1.
static int rowsSeenWith(const QString& socketName, const QByteArray& firstLine)
{
    QRemoteObjectNode node;
    auto* socket = new QLocalSocket(&node);
    bool refused = false;
    QObject::connect(socket, &QLocalSocket::readyRead, socket, [&]() {
        if (socket->bytesAvailable() < 3) return;
        QObject::disconnect(socket, &QLocalSocket::readyRead, socket, nullptr);
        if (socket->read(3) == "ok\n") node.addClientSideConnection(socket);
        else refused = true;
    });
    QObject::connect(socket, &QLocalSocket::disconnected, socket, [&]() { refused = true; });
    socket->connectToServer(socketName);
    if (!socket->waitForConnected(2000)) return -1;
    if (!firstLine.isNull()) socket->write(firstLine);
    QAbstractItemModelReplica* model = node.acquireModel(QStringLiteral("view/items"));
    pumpUntil([&] { return refused || (model->isInitialized() && model->rowCount() == 3); }, 3000);
    const int rows = (!refused && model->isInitialized()) ? model->rowCount() : -1;
    delete model;
    return rows;
}

int main(int argc, char** argv)
{
    const QString longTmp = QDir::tempPath() + QStringLiteral("/macos-like-tmpdir-padding-padding-padding-xx");
    QDir().mkpath(longTmp);
    qputenv("TMPDIR", longTmp.toUtf8());
    QCoreApplication app(argc, argv);

    QSet<QString> names;
    for (int i = 0; i < 100; ++i) names.insert(LogosViewChannel::socketName());
    CHECK(names.size() == 100, "socket names repeat");
    CHECK(LogosViewChannel::socketName().size() <= 24, "socket name too long");

    LogosViewChannel channel(QStringLiteral("a_rather_long_view_backend_module_name_backend"),
                             QStringLiteral("view"));
    CHECK(channel.listening(), "channel cannot listen under a macOS-length TMPDIR");

    Backend backend;
    channel.publishModelProperties(&backend);
    const QJsonObject endpoint = QJsonDocument::fromJson(channel.endpoint().toUtf8()).object();
    const QString socket = endpoint.value(QStringLiteral("socket")).toString();
    const QByteArray secret = endpoint.value(QStringLiteral("secret")).toString().toUtf8();
    CHECK(!socket.isEmpty() && secret.size() == 32, "endpoint lacks socket or secret");

    CHECK(rowsSeenWith(socket, QByteArray()) == -1, "a connection with no secret saw the model");
    CHECK(rowsSeenWith(socket, "0123456789abcdef0123456789abcdef\n") == -1,
          "a connection with a wrong secret saw the model");
    CHECK(rowsSeenWith(socket, secret + '\n') == 3, "the view, with its secret, did not see the model");
    CHECK(rowsSeenWith(socket, secret + '\n') == -1, "a secret worked twice");

    // 3. Stale sockets.
    const QString tmp = QDir::tempPath() + QLatin1Char('/');
    const QString dead = tmp + QStringLiteral("lv_00000000000000d1");
    const QString deadButFresh = tmp + QStringLiteral("lv_00000000000000d2");
    const QString notOurs = tmp + QStringLiteral("not_a_view_socket");
    CHECK(makeDeadSocket(dead) && makeDeadSocket(deadButFresh) && makeDeadSocket(notOurs),
          "could not create the stale-socket fixtures");
    backdate(dead);
    backdate(notOurs);
    QLocalServer live;
    CHECK(live.listen(QStringLiteral("lv_00000000000000a1")), "could not start the live fixture");
    backdate(live.fullServerName());

    LogosViewChannel second(QStringLiteral("second_backend"), QStringLiteral("view"));
    CHECK(second.listening(), "a second channel cannot listen");
    CHECK(!QFile::exists(dead), "a stale socket (nobody listening, old) was not removed");
    CHECK(QFile::exists(deadButFresh), "a socket younger than a minute was removed");
    CHECK(QFile::exists(notOurs), "a socket the channel did not name was removed");
    CHECK(QFile::exists(live.fullServerName()), "a live socket was removed");
    CHECK(QFile::exists(tmp + socket), "the first channel's own socket was removed");
    QFile::remove(deadButFresh);
    QFile::remove(notOurs);

    if (failures == 0) std::fprintf(stderr, "view-channel: OK\n");
    return failures == 0 ? 0 : 1;
}

#include "main.moc"
