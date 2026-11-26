#pragma once
#include <QHostAddress>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <stdexcept>
#include <utility>

namespace cc::service_fixtures {
enum class response { good, hold, redirect, oversized_header, oversized_body, unavailable };

struct fixture_server : QObject {
    QTcpServer listener;
    QByteArray update, maintenance;
    response primary = response::good, fallback = response::good, notice = response::good;
    QStringList seen;
    int connected{}, releases{}, handled{};
    bool invalid_request{};

    fixture_server(QByteArray update_bytes, QByteArray maintenance_bytes)
        : update(std::move(update_bytes)), maintenance(std::move(maintenance_bytes)) {
        if (!listener.listen(QHostAddress(QHostAddress::LocalHost), 0))
            throw std::runtime_error("Cannot open ephemeral IPv4 loopback listener.");
        connect(&listener, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = listener.nextPendingConnection()) {
                ++connected;
                socket->setReadBufferSize(8192);
                connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                    --connected;
                    ++releases;
                    socket->deleteLater();
                });
                connect(
                    socket, &QTcpSocket::readyRead, this,
                    [this, socket, request = QByteArray{}, responded = false]() mutable {
                        if (responded) {
                            socket->readAll();
                            return;
                        }
                        request += socket->readAll();
                        if (request.size() > 8192) {
                            invalid_request = true;
                            socket->abort();
                            return;
                        }
                        if (!request.contains("\r\n\r\n"))
                            return;
                        responded = true;
                        ++handled;
                        const auto line = request.left(request.indexOf("\r\n")).split(' ');
                        if (line.size() != 3 || line[0] != "GET" || line[2] != "HTTP/1.1" ||
                            request.toLower().contains("\r\ncookie:") ||
                            request.toLower().contains("\r\nauthorization:")) {
                            invalid_request = true;
                            socket->abort();
                            return;
                        }
                        const auto path = QString::fromLatin1(line[1]);
                        seen.push_back(path);
                        const auto* body = &update;
                        auto plan = primary;
                        if (path == "/maintenance") {
                            body = &maintenance;
                            plan = notice;
                        } else if (path == "/update/fallback")
                            plan = fallback;
                        else if (path != "/update/primary") {
                            invalid_request = true;
                            socket->abort();
                            return;
                        }
                        switch (plan) {
                        case response::hold:
                            socket->write(
                                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 1200\r\nConnection: close\r\n\r\n{");
                            break; // Leave the second half and EOF undelivered.
                        case response::redirect:
                            socket->write(
                                "HTTP/1.1 302 Found\r\nLocation: /must-not-follow\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                            socket->disconnectFromHost();
                            break;
                        case response::oversized_header:
                            socket->write(
                                "HTTP/1.1 200 OK\r\nContent-Length: 65537\r\nConnection: close\r\n\r\n");
                            break;
                        case response::oversized_body:
                            socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
                            socket->write(QByteArray(70'000, ' '));
                            socket->disconnectFromHost();
                            break;
                        case response::unavailable:
                            socket->write(
                                "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                            socket->disconnectFromHost();
                            break;
                        case response::good:
                            socket->write(
                                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                QByteArray::number(body->size()) + "\r\nConnection: close\r\n\r\n");
                            socket->write(*body);
                            socket->disconnectFromHost();
                            break;
                        }
                    });
            }
        });
    }
};

}
