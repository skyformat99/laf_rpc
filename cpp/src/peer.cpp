#include <QtCore/qmetaobject.h>
#include "../qtnetworkng/qtnetworkng.h"
#include "../include/base.h"
#include "../include/peer.h"
#include "../include/rpc_p.h"
#include "../include/tran_crypto.h"
#include "../include/serialization.h"

BEGIN_LAFRPC_NAMESPACE

inline QByteArray packRequest(const QSharedPointer<Serialization> &serialization, const Request &request)
{
    QVariantList l;
    l.append(QVariant::fromValue<int>(1));
    l.append(request.id);
    l.append(request.methodName);
    l.append(QVariant::fromValue<QVariantList>(request.args));
    l.append(request.kwargs);
    l.append(request.header);
    l.append(request.channel);
    l.append(request.rawSocket);
    return serialization->pack(QVariant::fromValue(l));
}


inline QByteArray packResponse(const QSharedPointer<Serialization> &serialization, const Response &response)
{
    QVariantList l;
    l.append(QVariant::fromValue<int>(2));
    l.append(response.id);
    l.append(response.result);
    l.append(response.exception);
    l.append(response.channel);
    l.append(response.rawSocket);
    return serialization->pack(QVariant::fromValue(l));
}


#define GOT_REQUEST 1
#define GOT_RESPONSE 2
#define GOT_NOTHING 3

int unpackRequestOrResponse(const QSharedPointer<Serialization> &serialization, const QByteArray &data,
                                   Request *request, Response *response)
{
    QVariant v;
    try {
        v = serialization->unpack(data);
    } catch (RpcSerializationException &) {
        return GOT_NOTHING;
    }

    if(v.type() != QVariant::List) {
        return GOT_NOTHING;
    }
    const QVariantList &l = v.toList();
    bool ok;
    if(l.size() == 8) {
        if(l[0].toInt(&ok) != 1) {
            return GOT_NOTHING;
        }
        request->id = l[1].toByteArray();
        request->methodName = l[2].toString();
        request->args = l[3].toList();
        request->kwargs = l[4].toMap();
        request->header = l[5].toMap();
        request->channel = l[6].toLongLong(&ok);
        request->rawSocket = l[7].toByteArray();
        if(ok) {
            return GOT_REQUEST;
        } else {
            return GOT_NOTHING;
        }
    } else if (l.size() == 6) {
        if(l[0].toInt(&ok) != 2) {
            return GOT_NOTHING;
        }
        response->id = l[1].toByteArray();
        response->result = l[2];
        response->exception = l[3];
        response->channel = l[4].toLongLong(&ok);
        response->rawSocket = l[5].toByteArray();
        if(ok) {
            return GOT_RESPONSE;
        } else {
            return GOT_NOTHING;
        }
    }
    return GOT_NOTHING;
}


class PeerPrivate
{
public:
    typedef qtng::ValueEvent<QSharedPointer<Response>> Waiter;

    PeerPrivate(const QString &name, const QSharedPointer<qtng::DataChannel> &channel,
                   const QPointer<Rpc> &rpc, const QByteArray &key, Peer *parent);
    ~PeerPrivate();
    void shutdown();
    QVariant call(const QString &methodName, const QVariantList &args, const QVariantMap &kwargs);
    void handlePacket();
    void handleRequest(QSharedPointer<Request> request);
    void handleResponse(QSharedPointer<Response> response);
    QVariant lookupAndCall(const QString &methodName, const QVariantList &args,
                           const QVariantMap &kwargs, const QVariantMap &header);

    QMap<QByteArray, QSharedPointer<Waiter>> waiters;
    QString name;
    QString address;
    QSharedPointer<qtng::DataChannel> channel;
    QPointer<Rpc> rpc;
    QByteArray key;
    bool broken;
    qtng::CoroutineGroup *operations;
    quint64 nextRequestId;

    Q_DECLARE_PUBLIC(Peer)
    Peer * const q_ptr;
};


PeerPrivate::PeerPrivate(const QString &name, const QSharedPointer<qtng::DataChannel> &channel,
                               const QPointer<Rpc> &rpc, const QByteArray &key, Peer *parent)
    :name(name)
    , channel(channel)
    , rpc(rpc)
    , key(key)
    , broken(false)
    , operations(new qtng::CoroutineGroup())
    , nextRequestId(1)
    , q_ptr(parent)
{
    operations->spawn([this]{handlePacket();});
}


PeerPrivate::~PeerPrivate()
{
    shutdown();
    delete operations;
}


void PeerPrivate::shutdown()
{
    Q_Q(Peer);
    if(broken) {
        return;
    }
    broken = true;
    QSharedPointer<Response> emptyResponse(new Response());
    for(QMap<QByteArray, QSharedPointer<Waiter>>::const_iterator itor = waiters.constBegin(); itor != waiters.constEnd(); ++itor) {
        itor.value()->send(emptyResponse);
    }
    waiters.clear();
    operations->killall();
    channel->close();
    QPointer<Peer> self(q);
    qtng::callInEventLoopAsync([self] {
        if (self.isNull()) {
            return;
        }
        emit self->disconnected(self.data());
    });
//    emit q->disconnected(q);
    q->clearServices();
    if (!rpc.isNull()) {
        rpc->d_ptr->removePeer(name);
    }
}


QVariant PeerPrivate::call(const QString &methodName, const QVariantList &args, const QVariantMap &kwargs)
{
    Q_Q(Peer);
    bool success;

    if(broken || rpc.isNull()) {
        throw RpcDisconnectedException(QString::fromUtf8("rpc is gone."));
    }

    QSharedPointer<UseStream> streamFromClient;
    for (const QVariant &v: args) {
        QSharedPointer<UseStream> p = UseStream::convert(v);
        if (!p.isNull()) {
            if (!streamFromClient.isNull()) {
                qWarning() << "there is two use stream arguments in" << methodName;
                throw RpcInternalException();
            } else {
                streamFromClient = p;
            }
        }
    }
    for (const QVariant &v: kwargs.values()) {
        QSharedPointer<UseStream> p = UseStream::convert(v);
        if (!p.isNull()) {
            if (!streamFromClient.isNull()) {
                qWarning() << "there is two use stream arguments in" << methodName;
                throw RpcInternalException();
            } else {
                streamFromClient = p;
            }
        }
    }

    Request request;
    request.id = createUuid();
    request.methodName = methodName;
    request.args = args;
    request.kwargs = kwargs;
    if(!rpc->d_ptr->headerCallback.isNull()) {
        request.header = rpc->d_ptr->headerCallback->make(q, methodName);
        if(broken || rpc.isNull()) {
            throw RpcDisconnectedException(QString::fromUtf8("rpc is gone."));
        }
    }

    QSharedPointer<qtng::VirtualChannel> subChannelFromClient;
    if (!streamFromClient.isNull()) {
        subChannelFromClient = channel->makeChannel();
        request.channel = subChannelFromClient->channelNumber();
        if(broken || rpc.isNull()) {
            throw RpcDisconnectedException(QString::fromUtf8("rpc is gone."));
        }
        if (streamFromClient->preferRawSocket) {
            QByteArray connectionId;
            QSharedPointer<qtng::SocketLike> connection = rpc->makeRawSocket(name, &connectionId);
            if (!connection.isNull() && !connectionId.isEmpty()) {
                request.rawSocket = connectionId;
                streamFromClient->rawSocket = connection;
            }
            if(broken || rpc.isNull()) {
                throw RpcDisconnectedException(QString::fromUtf8("rpc is gone."));
            }
        }
    }

    QByteArray requestBytes = packRequest(rpc.data()->serialization(), request);
    if(requestBytes.isEmpty()) {
        throw RpcSerializationException(QString::fromUtf8("can not serialize request while calling remote method: %1").arg(methodName));
    }
    if(!key.isEmpty()) {
        requestBytes = rpc->crypto()->encrypt(requestBytes, key);
        if(requestBytes.isEmpty()) {
            throw RpcInternalException(QString::fromUtf8("can not encrypt data while calling remote method: %1").arg(methodName));
        }
        if(broken || rpc.isNull()) {
            throw RpcDisconnectedException(QString::fromUtf8("rpc is gone."));
        }
    }

    QSharedPointer<Waiter> waiter(new Waiter());
    waiters.insert(request.id, waiter);

    success = channel->sendPacket(requestBytes);
    if(!success) {
        shutdown();
        throw RpcDisconnectedException("can not send packet.");
    }

    if(broken || rpc.isNull()) {
        throw RpcDisconnectedException(QString::fromUtf8("rpc is gone."));
    }

    QSharedPointer<Response> response;
    try {
        qtng::Timeout _(rpc->timeout());
        response = waiter->wait();
        waiters.remove(request.id);
    } catch (qtng::TimeoutException &) {
        waiters.remove(request.id);
        const QString &message = QString::fromUtf8("timeout calling remote method: `%1`").arg(methodName);
        throw RpcRemoteException(message);
    } catch (qtng::CoroutineExitException &) {
        waiters.remove(request.id);
        throw;
    } catch(RpcException &) {
        throw;
    } catch (...) {
        waiters.remove(request.id);
        const QString &message = QString::fromUtf8("unknown error occurs while waiting response of remote method: `%1`").arg(methodName);
        throw RpcInternalException(message);
    }

    if(response.isNull() || !response->isOk()) {
        const QString &message = QString::fromUtf8("got empty response while waiting response of remote method: `%1`").arg(methodName);
        throw RpcDisconnectedException(message);
    }

    if(broken || rpc.isNull()) {
        throw RpcDisconnectedException(QString::fromUtf8("rpc is gone."));
    }

    if(!response->exception.isNull()) {
        RpcRemoteException::raise(response->exception);
        // the upper function do not return if success.
        throw RpcInternalException("unknown exception.");
    }
    if (!streamFromClient.isNull()) {
        streamFromClient->setReady(UseStream::ClientSide | UseStream::ParamInRequest, subChannelFromClient);
    }

    QSharedPointer<UseStream> streamFromServer = UseStream::convert(response->result);
    if (!streamFromServer.isNull()) {
        streamFromServer->setRpc(rpc);
        if (response->channel == 0) {
            qWarning() << "the response of" << methodName << "is a use-stream, but has no channel number.";
            throw RpcInternalException();
        }
        QSharedPointer<qtng::VirtualChannel> subChannelFromServer = channel->getChannel(response->channel);
        if (subChannelFromServer.isNull()) {
            qWarning() << methodName << "returns a channel, but is gone.";
            throw RpcRemoteException();
        }
        if (!response->rawSocket.isEmpty()) {
            if (!streamFromServer->preferRawSocket) {
                qWarning() << "the response of" << methodName << "do not prefer raw socket, but got one.";
            }
            QSharedPointer<qtng::SocketLike> connection = rpc->getRawSocket(name, response->rawSocket);
            if (connection.isNull()) {
                qWarning() << "the response of" << methodName << "returns a raw socket, but is gone:" << response->rawSocket;
            }
            streamFromServer->rawSocket = connection;
        }
        streamFromServer->setReady(UseStream::ClientSide | UseStream::ValueOfResponse, subChannelFromServer);
    }
    return response->result;
}

void PeerPrivate::handlePacket()
{
    if(broken || rpc.isNull()) {
        return;
    }

    while(true) {
        QByteArray packet;
        try {
            packet = channel->recvPacket();
        } catch (qtng::CoroutineException &) {
            return shutdown();
        } catch (...) {
            qWarning() << "got unknown exception while receving packet.";
            return shutdown();
        }

        if(packet.isEmpty()) {
            return shutdown();
        }

        if(broken || rpc.isNull()) {
            return shutdown();
        }
        if(!key.isEmpty()) {
            packet = rpc.data()->crypto()->decrypt(packet, key);
            if(packet.isEmpty()) {
                qDebug() << "invalid packet.";
                return shutdown();
            }
        }
        
        QSharedPointer<Request> request(new Request());
        QSharedPointer<Response> response(new Response());
        int what = unpackRequestOrResponse(rpc->serialization(), packet, request.data(), response.data());
        if(what == GOT_REQUEST && request->isOk()) {
            operations->spawn([this, request] {
                handleRequest(request);
            });
        } else if(what == GOT_RESPONSE && response->isOk()) {
            QSharedPointer<qtng::ValueEvent<QSharedPointer<Response>>> waiter = waiters.value(response->id);
            if(waiter.isNull()) {
                qDebug() << "received a response from server, but waiter is gone: " << response->id;
            } else {
                waiter->send(response);
            }
        } else {
            qDebug() << "can not handle received packet." << packet;
        }
    }
}

void PeerPrivate::handleRequest(QSharedPointer<Request> request)
{
    Q_Q(Peer);
    bool success;
    if(broken || rpc.isNull()) {
        return;
    }
    if(!rpc->d_ptr->headerCallback.isNull()) {
        bool success = rpc->d_ptr->headerCallback->auth(q, request->methodName, request->header);
        if(!success) {
            qDebug() << "invalid packet from" << name;
            return;
        }
    }
    if(broken || rpc.isNull()) {
        return;
    }

    QSharedPointer<UseStream> streamFromClient;
    for (const QVariant &v: request->args) {
        streamFromClient = UseStream::convert(v);
        if (!streamFromClient.isNull()) {
            break;
        }
    }
    if (streamFromClient.isNull()) {
        for (const QVariant &v: request->kwargs.values()) {
            streamFromClient = UseStream::convert(v);
            if (!streamFromClient.isNull()) {
                break;
            }
        }
    }

    Response response;
    response.id = request->id;

    QSharedPointer<qtng::VirtualChannel> subChannelFromClient;
    if (!streamFromClient.isNull()) {
        if (request->channel == 0) {
            qWarning() << "the request of" << request->methodName << "pass a use-stream parameter, but sent no channel.";
            QSharedPointer<RpcRemoteException> e(new RpcRemoteException("bad channel"));
            response.exception.setValue(e);
        } else {
            subChannelFromClient = channel->getChannel(request->channel);
            if (subChannelFromClient.isNull()) {
                qWarning() << "the request of" << request->methodName << "sent a channel, but it is gone.";
                QSharedPointer<RpcRemoteException> e(new RpcRemoteException("bad channel"));
                response.exception.setValue(e);
            } else {
                if (!request->rawSocket.isEmpty()) {
                    if (!streamFromClient->preferRawSocket) {
                        qWarning() << request->methodName << "send an UseSteam(prefer_raw_socket), but there is no raw socket.";
                    }
                    QSharedPointer<qtng::SocketLike> connection =  rpc->getRawSocket(name, request->rawSocket);
                    if (connection.isNull()) {
                        qWarning() << request->methodName << "sent an use-stream raw socket, but it is gone.";
                    } else {
                        streamFromClient->rawSocket = connection;
                    }
                }
            }
        }
    }

    if (response.exception.isNull()) {
        try {
            response.result = lookupAndCall(request->methodName, request->args, request->kwargs, request->header);
        } catch (qtng::CoroutineException) {
            throw;
        } catch (RpcRemoteException &e) {
            response.exception = e.clone();
            qDebug() << response.exception;
        } catch (...) {
            QSharedPointer<RpcRemoteException> e(new RpcRemoteException("unknown exception caught."));
            response.exception.setValue(e);
        }
        if(broken || rpc.isNull()) {
            return;
        }
    }

    QSharedPointer<qtng::VirtualChannel> subChannelFromServer;
    QSharedPointer<UseStream> streamFromServer;
    if (response.exception.isNull()) {
        streamFromServer = UseStream::convert(response.result);
        if (!streamFromServer.isNull()) {
            streamFromServer->setRpc(rpc);
            subChannelFromServer = channel->makeChannel();
            if (broken || rpc.isNull()) {
                return;
            }
            if (subChannelFromServer.isNull()) {
                qWarning() << "can not make channel for the respone of" << request->methodName;
                response.result.clear();
                QSharedPointer<RpcRemoteException> e(new RpcRemoteException("bad channel"));
                response.exception.setValue(e);
            } else {
                response.channel = subChannelFromServer->channelNumber();
                if (streamFromServer->preferRawSocket) {
                    QByteArray connectionId;
                    QSharedPointer<qtng::SocketLike> connection = rpc->makeRawSocket(name, &connectionId);
                    if (connection.isNull() || connectionId.isEmpty()) {
                        qDebug() << "can not make raw sockt to" << name << "for" << request->methodName;
                    } else {
                        streamFromServer->rawSocket = connection;
                        response.rawSocket = connectionId;
                    }
                    if (broken || rpc.isNull()) {
                        return;
                    }
                }
            }
        }
    }

    const QByteArray &responseBytes = packResponse(rpc.data()->serialization(), response);
    if (responseBytes.isEmpty()) {
       qDebug() << "can not serialize response.";
       return;
    }

    success = channel->sendPacket(responseBytes);
    if(!success || broken || rpc.isNull()) {
        return;
    }

    if (!response.exception.isNull()) {
        return;
    }

    if (!streamFromClient.isNull()) {
        streamFromClient->setReady(UseStream::ServerSide | UseStream::ParamInRequest, subChannelFromClient);
    }
    if (!streamFromServer.isNull()) {
        streamFromServer->setReady(UseStream::ServerSide | UseStream::ValueOfResponse, subChannelFromServer);
    }
}

QByteArray removeNamespace(const QByteArray &typeName)
{
    if (typeName.isEmpty()) {
        return typeName;
    }
    int lt = typeName.indexOf('<');
    QByteArray leftPart, rightPart;
    if (lt >= 0) {
        leftPart = typeName.left(lt);
        int gt = typeName.lastIndexOf('>');
        if (gt < 0) {
            return typeName;
        }
        rightPart = typeName.mid(lt + 1, gt - lt - 1);
    } else {
        leftPart = typeName;
        rightPart = "";
    }
    int colon = leftPart.lastIndexOf("::");
    if (colon >= 0) {
        leftPart = leftPart.mid(colon + 2);
    }
    if (rightPart.isEmpty()) {
        return leftPart;
    } else {
        return leftPart + "<" + removeNamespace(rightPart) + ">";
    }
}

int metaTypeOf(const char *typeName)
{
    int type = QMetaType::type(typeName);
    if (type) {
        return type;
    }
    QByteArray toFound(typeName);
    removeNamespace(toFound);

    for(int i = QMetaType::User;; ++i) {
        const char *s = QMetaType::typeName(i);
        if (!s) {
            return 0;
        }
        QByteArray tempName(s);
        if (tempName.isEmpty()) {
            return 0;
        }
        tempName = removeNamespace(tempName);
        if (toFound == tempName) {
            return i;
        }
    }
}

QVariant objectCall(QObject *obj, const QString &methodName, QVariantList args, QVariantMap kwargs)
{
    const QMetaObject *metaObj = obj->metaObject();
    if (!metaObj) {
        throw RpcRemoteException("obj is not callable.");
    }
    if (args.size() > 9) {
        throw RpcRemoteException("too many arguments.");
    }
    QMetaMethod found;
    for (int i = metaObj->methodOffset(); i < metaObj->methodCount(); ++i) {
        const QMetaMethod &method = metaObj->method(i);
        if (method.name() == methodName) {
            found = method;
            break;
        }
    }
    if (!found.isValid()) {
        throw RpcRemoteException("method not found.");
    }

    const QList<QByteArray> &parameterTypeNames = found.parameterTypes();
    const QList<QByteArray> &parameterNames = found.parameterNames();
    QList<int> parameterTypes;
    for (int i = 0; i < found.parameterCount(); ++i) {
        parameterTypes.append(found.parameterType(i));
    }
    if (parameterTypeNames.size() != parameterNames.size()) {
        throw RpcRemoteException("parameter names and types do not match.");
    }
    if (parameterTypeNames.size() != parameterTypes.size()) {
        throw RpcRemoteException("parameter type ids and names do not match.");
    }
    if (args.size() != parameterTypes.size()) {
        throw RpcRemoteException("the size of past args do not match the parameter count of method.");
    }
    QList<QGenericArgument> parameters;
    for (int i = 0; i < parameterTypes.size(); ++i) {
        int typeId = parameterTypes.at(i);
        const QByteArray &typeName = parameterTypeNames.at(i);
        const QByteArray &parameterName = parameterNames.at(i);
        QVariant &arg = args[i];
        if (!arg.convert(typeId)) {
            QString message = "the parameter %1 with type %2 can not accept the past argument.";
            message = message.arg(QString::fromUtf8(parameterName)).arg(QString::fromUtf8(typeName));
            throw RpcRemoteException(message);
        }
        parameters.append(QGenericArgument(typeName.constData(), arg.constData()));
    }
    for (int i = parameters.size(); i < 10; ++i) {
        parameters.append(QGenericArgument());
    }

    int type = metaTypeOf(found.typeName());
    if (!type) {
        throw RpcRemoteException(QStringLiteral("unknown return type: %1").arg(found.typeName()));
    }
    QVariant rvalue(type, QMetaType::create(type));
    QGenericReturnArgument rarg(found.typeName(), rvalue.data());

    found.invoke(obj, Qt::DirectConnection, rarg, parameters[0], parameters[1], parameters[2], parameters[3],
            parameters[4], parameters[5], parameters[6], parameters[7], parameters[8], parameters[9]);
    return rvalue;

}

QVariant PeerPrivate::lookupAndCall(const QString &methodName, const QVariantList &args, const QVariantMap &kwargs, const QVariantMap &header)
{
    Q_Q(Peer);
    const QStringList &l = methodName.split(QChar('.'));
    if(l.size() < 1) {
        throw RpcRemoteException();
    }
    const QString &serviceName = l[0];
    const QStringList &l2 = l.mid(1);
    if(!q->getServices().contains(serviceName)) {
        throw RpcRemoteException();
    }
    const RpcService &rpcService = q->getServices().value(serviceName);

    QPointer<Rpc> rpc = this->rpc;
    rpc.data()->d_func()->setCurrentPeerAndHeader(q, header);
    Cleaner cleaner([rpc]{
        if(rpc.isNull())
            return;
        rpc.data()->d_func()->deleteCurrentPeerAndHeader();
    });Q_UNUSED(cleaner);

    if(rpcService.type == ServiceType::FUNCTION) {
        return rpcService.function(args, kwargs);
    } else {
        if(l2.isEmpty()) {
            throw RpcRemoteException();
        }
        const QSharedPointer<Callable> &callable = qSharedPointerDynamicCast<Callable>(rpcService.instance);
        if(callable.isNull()) {
            try {
                if (!this->rpc->d_ptr->loggingCallback.isNull()) {
                    this->rpc->d_ptr->loggingCallback->calling(q, methodName, args, kwargs);
                }
                const QVariant &result = objectCall(rpcService.instance.data(), l2[0], args, kwargs);
                if (!this->rpc->d_ptr->loggingCallback.isNull()) {
                    this->rpc->d_ptr->loggingCallback->success(q, methodName, args, kwargs, result);
                }
                return result;
            } catch (...) {
                if (!this->rpc->d_ptr->loggingCallback.isNull()) {
                    this->rpc->d_ptr->loggingCallback->failed(q, methodName, args, kwargs);
                }
                throw;
            }
        } else {
            try {
                if (!this->rpc->d_ptr->loggingCallback.isNull()) {
                    this->rpc->d_ptr->loggingCallback->calling(q, methodName, args, kwargs);
                }
                const QVariant &result = callable->call(l2[0], args, kwargs);
                if (!this->rpc->d_ptr->loggingCallback.isNull()) {
                    this->rpc->d_ptr->loggingCallback->success(q, methodName, args, kwargs, result);
                }
                return result;
            } catch (...) {
                if (!this->rpc->d_ptr->loggingCallback.isNull()) {
                    this->rpc->d_ptr->loggingCallback->failed(q, methodName, args, kwargs);
                }
                throw;
            }
        }
    }
}


Peer::Peer(const QString &name, const QSharedPointer<qtng::DataChannel> &channel,
                 const QPointer<Rpc> &rpc, const QByteArray &key)
    :d_ptr(new PeerPrivate(name, channel, rpc, key, this))
{
}


Peer::~Peer()
{
    delete d_ptr;
}


void Peer::shutdown()
{
    Q_D(Peer);
    d->shutdown();
}


bool Peer::isOk() const
{
    Q_D(const Peer);
    return !d->broken && !d->rpc.isNull();
}


bool Peer::isActive() const
{
    Q_D(const Peer);
    return !d->waiters.isEmpty();
}


QString Peer::name() const
{
    Q_D(const Peer);
    return d->name;
}


void Peer::setName(const QString &name)
{
    Q_D(Peer);
    d->name = name;
}


QString Peer::address() const
{
    Q_D(const Peer);
    return d->address;
}


void Peer::setAddress(const QString &address)
{
    Q_D(Peer);
    d->address = address;
}


QVariant Peer::call(const QString &method, const QVariantList &args, const QVariantMap &kwargs)
{
    Q_D(Peer);
    return d->call(method, args, kwargs);
}


QVariant Peer::call(const QString &method, const QVariant &arg1)
{
    Q_D(Peer);
    QVariantList args;
    args << arg1;
    return d->call(method, args, QVariantMap());
}


QVariant Peer::call(const QString &method, const QVariant &arg1, const QVariant &arg2)
{
    Q_D(Peer);
    QVariantList args;
    args << arg1 << arg2;
    return d->call(method, args, QVariantMap());
}


QVariant Peer::call(const QString &method, const QVariant &arg1, const QVariant &arg2, const QVariant &arg3)
{
    Q_D(Peer);
    QVariantList args;
    args << arg1 << arg2 << arg3;
    return d->call(method, args, QVariantMap());
}


QVariant Peer::call(const QString &method, const QVariant &arg1, const QVariant &arg2, const QVariant &arg3, const QVariant &arg4)
{
    Q_D(Peer);
    QVariantList args;
    args << arg1 << arg2 << arg3 << arg4;
    return d->call(method, args, QVariantMap());
}


QVariant Peer::call(const QString &method, const QVariant &arg1, const QVariant &arg2, const QVariant &arg3, const QVariant &arg4, const QVariant &arg5)
{
    Q_D(Peer);
    QVariantList args;
    args << arg1 << arg2 << arg3 << arg4 << arg5;
    return d->call(method, args, QVariantMap());
}


QVariant Peer::call(const QString &method, const QVariant &arg1, const QVariant &arg2, const QVariant &arg3, const QVariant &arg4, const QVariant &arg5, const QVariant &arg6)
{
    Q_D(Peer);
    QVariantList args;
    args << arg1 << arg2 << arg3 << arg4 << arg5 << arg6;
    return d->call(method, args, QVariantMap());
}


QVariant Peer::call(const QString &method, const QVariant &arg1, const QVariant &arg2, const QVariant &arg3, const QVariant &arg4, const QVariant &arg5, const QVariant &arg6, const QVariant &arg7)
{
    Q_D(Peer);
    QVariantList args;
    args << arg1 << arg2 << arg3 << arg4 << arg5 << arg6 << arg7;
    return d->call(method, args, QVariantMap());
}


QVariant Peer::call(const QString &method, const QVariant &arg1, const QVariant &arg2, const QVariant &arg3, const QVariant &arg4, const QVariant &arg5, const QVariant &arg6, const QVariant &arg7, const QVariant &arg8)
{
    Q_D(Peer);
    QVariantList args;
    args << arg1 << arg2 << arg3 << arg4 << arg5 << arg6 << arg7 << arg8;
    return d->call(method, args, QVariantMap());
}


QVariant Peer::call(const QString &method, const QVariant &arg1, const QVariant &arg2, const QVariant &arg3, const QVariant &arg4, const QVariant &arg5, const QVariant &arg6, const QVariant &arg7, const QVariant &arg8, const QVariant &arg9)
{
    Q_D(Peer);
    QVariantList args;
    args << arg1 << arg2 << arg3 << arg4 << arg5 << arg6 << arg7 << arg8 << arg9;
    return d->call(method, args, QVariantMap());
}


QSharedPointer<qtng::DataChannel> Peer::makeChannel()
{
    Q_D(Peer);
    if (!isOk()) {
        return QSharedPointer<qtng::DataChannel>();
    }
    return d->channel->makeChannel();
}

QSharedPointer<qtng::DataChannel> Peer::getChannel(quint32 channelNumber)
{
    Q_D(Peer);
    if (!isOk()) {
        return QSharedPointer<qtng::DataChannel>();
    }
    return d->channel->getChannel(channelNumber);

}

END_LAFRPC_NAMESPACE
