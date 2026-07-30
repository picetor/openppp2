#include <arpa/inet.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace {
bool Resolve(const char* hostname, int family, const char* expected_address = nullptr) {
    addrinfo hints{};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* addresses = nullptr;
    if (getaddrinfo(hostname, "443", &hints, &addresses) != 0) {
        return false;
    }

    bool found = expected_address == nullptr;
    for (addrinfo* address = addresses; address != nullptr && !found; address = address->ai_next) {
        char text[INET6_ADDRSTRLEN]{};
        const void* source = address->ai_family == AF_INET
            ? static_cast<const void*>(&reinterpret_cast<sockaddr_in*>(address->ai_addr)->sin_addr)
            : static_cast<const void*>(&reinterpret_cast<sockaddr_in6*>(address->ai_addr)->sin6_addr);
        found = inet_ntop(address->ai_family, source, text, sizeof(text)) != nullptr &&
            std::strcmp(text, expected_address) == 0;
    }
    freeaddrinfo(addresses);
    return found;
}

bool ConnectHttps(const char* hostname) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* addresses = nullptr;
    if (getaddrinfo(hostname, "443", &hints, &addresses) != 0) {
        return false;
    }

    int socket_handle = -1;
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        socket_handle = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_handle >= 0 && connect(socket_handle, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        if (socket_handle >= 0) {
            close(socket_handle);
            socket_handle = -1;
        }
    }
    freeaddrinfo(addresses);
    if (socket_handle < 0) {
        return false;
    }

    SSL_CTX* context = SSL_CTX_new(TLS_client_method());
    if (context == nullptr || SSL_CTX_set_default_verify_paths(context) != 1) {
        SSL_CTX_free(context);
        close(socket_handle);
        return false;
    }

    SSL* ssl = SSL_new(context);
    const bool configured = ssl != nullptr &&
        SSL_set_tlsext_host_name(ssl, hostname) == 1 &&
        SSL_set1_host(ssl, hostname) == 1 &&
        SSL_set_fd(ssl, socket_handle) == 1;
    const bool connected = configured && SSL_connect(ssl) == 1 &&
        SSL_get_verify_result(ssl) == X509_V_OK;

    SSL_free(ssl);
    SSL_CTX_free(context);
    close(socket_handle);
    return connected;
}
}  // namespace

int main() {
    if (!Resolve("openppp2-musl-hosts.test", AF_INET, "127.0.0.1")) {
        std::fputs("/etc/hosts resolution failed\n", stderr);
        return 1;
    }
    if (!Resolve("example.com", AF_INET)) {
        std::fputs("DNS A resolution failed\n", stderr);
        return 1;
    }
    if (!Resolve("example.com", AF_INET6)) {
        std::fputs("DNS AAAA resolution failed\n", stderr);
        return 1;
    }
    if (!ConnectHttps("example.com")) {
        ERR_print_errors_fp(stderr);
        std::fputs("HTTPS SNI or certificate verification failed\n", stderr);
        return 1;
    }
    return 0;
}