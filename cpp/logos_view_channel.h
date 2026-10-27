#pragma once

// The private QtRO channel between a Qt view backend running as a core module
// and its own view. What ui-host used to host, inside the module instead.
//
// The backend's .rep source and every QAbstractItemModel* property on it are
// published to connections this channel has ADMITTED. A connection is admitted
// only after it sends a one-time secret that endpoint() issued — and
// endpoint() is served by view_endpoint(), a logos-core call capability_module
// lets only the view make. The socket's name is not a secret (any process of
// the same user can list the directory it lives in); the secret is.
//
// Wire handshake, before any QtRO byte: client sends "<secret>\n", channel
// answers "ok\n" and hands the socket to QtRO, or closes it.
//
// Header-only; used by the glue `logos-view-generator --backend ui-module`
// emits.

#include <QAbstractItemModel>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMetaProperty>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QRemoteObjectHost>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QDebug>

class LogosViewChannel {
public:
    /// `viewName` is the view module's name: models are published as
    /// "<viewName>/<property>", the names a view's `logos.model()` asks for.
    LogosViewChannel(const QString& moduleName, const QString& viewName)
        : m_socket(socketName())
        , m_viewName(viewName)
    {
        // A host URL Qt has no transport for, so QtRO sets the host up WITHOUT
        // listening itself: every connection is handed over by admit().
        m_host.setHostUrl(QUrl(QStringLiteral("logosview:") + m_socket),
                          QRemoteObjectHost::AllowExternalRegistration);
        if (const int stale = removeStaleSockets())
            qInfo() << "LogosViewChannel: removed" << stale << "stale view socket(s)";
        m_server.setSocketOptions(QLocalServer::UserAccessOption);
        m_listening = m_server.listen(m_socket);
        if (!m_listening) {
            qWarning() << "LogosViewChannel:" << moduleName << "could not listen on" << m_socket
                       << "-" << m_server.errorString() << "; its view will not connect";
            return;
        }
        QObject::connect(&m_server, &QLocalServer::newConnection, &m_server, [this]() {
            while (QLocalSocket* socket = m_server.nextPendingConnection())
                admit(socket);
        });
    }

    /// A fresh socket name SHORT enough for every platform: macOS caps a unix
    /// socket's full path at 104 bytes and Qt puts it under $TMPDIR, which is
    /// already long there. 19 characters.
    static QString socketName()
    {
        return QStringLiteral("lv_") + QUuid::createUuid().toString(QUuid::Id128).left(16);
    }

    bool listening() const { return m_listening; }

    /// Removes `lv_*` sockets left in the temp dir by channels whose process
    /// was killed; a clean shutdown removes its own. Only files older than
    /// `minAgeMs` (a channel starting right now is never touched) and only
    /// when connecting is REFUSED, i.e. nobody listens. Another user's live
    /// socket in a shared /tmp answers "access denied" and stays. Unix only:
    /// Windows uses named pipes, which leave no files.
    static int removeStaleSockets(qint64 minAgeMs = kStaleSocketMinAgeMs)
    {
#ifdef Q_OS_WIN
        Q_UNUSED(minAgeMs);
        return 0;
#else
        static const QRegularExpression kName(QStringLiteral("^lv_[0-9a-f]{16}$"));
        const QDateTime cutoff = QDateTime::currentDateTime().addMSecs(-minAgeMs);
        const QFileInfoList entries = QDir(QDir::tempPath()).entryInfoList(
            { QStringLiteral("lv_*") }, QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot);
        int removed = 0;
        for (const QFileInfo& entry : entries) {
            if (entry.isDir() || !kName.match(entry.fileName()).hasMatch()) continue;
            if (entry.lastModified() > cutoff) continue;
            QLocalSocket probe;
            probe.connectToServer(entry.absoluteFilePath());
            if (probe.waitForConnected(200)) {
                probe.abort();
                continue;
            }
            if (probe.error() != QLocalSocket::ConnectionRefusedError) continue;
            if (QFile::remove(entry.absoluteFilePath())) ++removed;
        }
        return removed;
#endif
    }

    /// Publishes the backend under its .rep class name.
    template <template <typename> class ApiDefinition, typename ObjectType>
    bool publishSource(ObjectType* source, const QString& sourceName)
    {
        if (!m_listening) return false;
        m_source = sourceName;
        return m_host.template enableRemoting<ApiDefinition>(source);
    }

    /// Publishes every readable QAbstractItemModel* property of `object`.
    void publishModelProperties(QObject* object)
    {
        if (!m_listening) return;
        const QMetaObject* mo = object->metaObject();
        for (int i = 0; i < mo->propertyCount(); ++i) {
            const QMetaProperty prop = mo->property(i);
            if (!prop.isReadable()) continue;
            auto* model = qvariant_cast<QAbstractItemModel*>(prop.read(object));
            if (!model) continue;
            const QString name = m_viewName + QLatin1Char('/') + QString::fromUtf8(prop.name());
            if (m_host.enableRemoting(model, name, model->roleNames().keys()))
                m_models << name;
            else
                qWarning() << "LogosViewChannel: could not publish model" << name;
        }
    }

    /// {"socket", "secret", "source", "models"}. Every call issues a new
    /// secret, good for ONE connection within a minute.
    QString endpoint()
    {
        if (!m_listening) return QString();
        const QString secret = newSecret();
        m_secrets.insert(secret, QDateTime::currentMSecsSinceEpoch() + kSecretLifetimeMs);
        const QJsonObject o{ { QStringLiteral("socket"), m_socket },
                             { QStringLiteral("secret"), secret },
                             { QStringLiteral("source"), m_source },
                             { QStringLiteral("models"), QJsonArray::fromStringList(m_models) } };
        return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
    }

private:
    static constexpr qint64 kSecretLifetimeMs = 60000;
    static constexpr qint64 kStaleSocketMinAgeMs = 60000;
    static constexpr int kHandshakeTimeoutMs = 5000;
    static constexpr int kSecretLength = 32;

    static QString newSecret()
    {
        QRandomGenerator* rng = QRandomGenerator::system();
        return QString::number(rng->generate64(), 16).rightJustified(16, QLatin1Char('0'))
             + QString::number(rng->generate64(), 16).rightJustified(16, QLatin1Char('0'));
    }

    bool consume(const QString& secret)
    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto it = m_secrets.begin(); it != m_secrets.end();)
            it = it.value() < now ? m_secrets.erase(it) : std::next(it);
        return m_secrets.remove(secret) > 0;   // one connection per secret
    }

    void admit(QLocalSocket* socket)
    {
        QTimer::singleShot(kHandshakeTimeoutMs, socket, [socket]() {
            if (!socket->property("logosAdmitted").toBool()) socket->abort();
        });
        QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QLocalSocket::readyRead, socket, [this, socket]() {
            if (!socket->canReadLine()) {
                if (socket->bytesAvailable() > kSecretLength + 1) socket->abort();
                return;
            }
            QObject::disconnect(socket, &QLocalSocket::readyRead, socket, nullptr);
            const QString secret = QString::fromUtf8(socket->readLine()).trimmed();
            if (!consume(secret)) {
                qWarning() << "LogosViewChannel: refused a connection without a valid secret";
                socket->abort();
                return;
            }
            socket->setProperty("logosAdmitted", true);
            socket->write("ok\n");
            socket->flush();
            m_host.addHostSideConnection(socket);
        });
    }

    QString m_socket;
    QString m_viewName;
    QString m_source;
    QStringList m_models;
    QHash<QString, qint64> m_secrets;   // secret -> expiry (ms since epoch)
    bool m_listening = false;
    QLocalServer m_server;
    QRemoteObjectHost m_host;
};
