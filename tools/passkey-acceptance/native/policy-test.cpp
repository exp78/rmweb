#include "policy.h"
#include <QCoreApplication>
#include <cstdio>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QByteArray origin("https://passkey.example.test");
    if (!acceptance::validOrigin(origin) || !acceptance::allowed(origin, origin + "/verify")) return 1;
    for (const auto &bad : {"https://passkey.example.test/", "https://user@passkey.example.test", "http://passkey.example.test",
        "https://passkey.example.test:443", "https://passkey.example.test:0", "https://127.0.0.1", "https://localhost", "https://passkey.example.test?x", "https://passkey.example.test#x"})
        if (acceptance::validOrigin(bad)) return 1;
    for (const auto &bad : {"/verify", "about:blank", "https://other.example.test/verify", "https://passkey.example.test/register",
        "https://passkey.example.test/verify?x", "https://passkey.example.test/verify#x", "https://passkey.example.test/a/../verify",
        "https://passkey.example.test/%76erify", "file:///tmp/test", "data:text/html,test"})
        if (acceptance::allowed(origin, bad)) return 1;
    for (int size : {32, 43, 128})
        if (!acceptance::allowed(origin, origin + "/verify#join=" + QByteArray(size, 'A'))) return 1;
    for (const auto &code : {QByteArray(31, 'A'), QByteArray(129, 'A'), QByteArray(32, 'A') + "%2F",
        QByteArray(32, 'A') + "&x=y", QByteArray(32, 'A') + "#x", QByteArray(32, 'A') + "\n"})
        if (acceptance::allowed(origin, origin + "/verify#join=" + code)) return 1;
    std::puts("acceptance-policy PASS"); return 0;
}
