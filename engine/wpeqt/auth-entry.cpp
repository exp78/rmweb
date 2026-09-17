#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sched.h>
#include <string>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

extern char **environ;

namespace {
// Locate this owned installation from the executable, never caller input.
bool installationPaths(std::string &browser, std::string &helpers) {
    char buffer[PATH_MAX];
    const ssize_t size = readlink("/proc/self/exe", buffer, sizeof(buffer));
    if (size <= 0 || size >= static_cast<ssize_t>(sizeof(buffer))) return false;
    const std::string executable(buffer, static_cast<size_t>(size));
    const size_t separator = executable.rfind('/');
    if (separator == std::string::npos) return false;
    const std::string bin = executable.substr(0, separator);
    if (bin.size() <= 4 || bin.substr(bin.size() - 4) != "/bin") return false;
    browser = bin + "/rmweb-auth-browser";
    helpers = bin.substr(0, bin.size() - 4) + "/runtime/libexec";
    return true;
}
constexpr const char *MountPoint = "/usr/libexec";

class Fd {
public:
    explicit Fd(int value = -1) : value(value) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    int value;
};

int fail(const char *message) {
    // No URLs, keys, or private paths from caller input are logged.
    std::fprintf(stderr, "rmweb-auth-entry: %s\n", message);
    return 1;
}

bool owned(const struct stat &info, bool directory) {
    return info.st_uid == 0 && !(info.st_mode & (S_IWGRP | S_IWOTH | S_ISUID | S_ISGID))
        && (directory ? S_ISDIR(info.st_mode)
                      : S_ISREG(info.st_mode) && (info.st_mode & 0111));
}

// Walk every component without following symlinks. The installation's parent
// directories cannot be exchanged by an unprivileged process after validation.
int openOwned(const char *path, bool directory) {
    if (!path || path[0] != '/') return -1;
    int current = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info{};
    if (current < 0) return -1;
    if (fstat(current, &info) != 0 || !owned(info, true)) {
        ::close(current);
        return -1;
    }
    const std::string input(path);
    size_t start = 1;
    while (start < input.size()) {
        const size_t end = input.find('/', start);
        const bool last = end == std::string::npos;
        const std::string part = input.substr(start, last ? end : end - start);
        if (part.empty() || part == "." || part == "..") { ::close(current); return -1; }
        const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK
            | ((!last || directory) ? O_DIRECTORY : 0);
        const int next = openat(current, part.c_str(), flags);
        ::close(current);
        current = next;
        if (current < 0) return -1;
        if (fstat(current, &info) != 0 || !owned(info, !last || directory)) {
            ::close(current);
            return -1;
        }
        if (last) return current;
        start = end + 1;
    }
    ::close(current);
    return -1;
}

bool validUrl(const char *url) {
    const size_t length = strnlen(url, 8193);
    if (!length || length > 8192) return false;
    size_t scheme = 0;
    if (std::strncmp(url, "https://", 8) == 0) scheme = 8;
    else return false;
    if (length <= scheme || url[scheme] == '/' || url[scheme] == '?' || url[scheme] == '#')
        return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(url[i]);
        if (c <= 0x20 || c == 0x7f) return false;
    }
    return true;
}

bool validKey() {
    const char *key = std::getenv("QTFB_KEY");
    if (!key) return false;
    const size_t length = strnlen(key, 11);
    if (!length || length > 10) return false;
    unsigned long number = 0;
    for (size_t i = 0; i < length; ++i) {
        if (key[i] < '0' || key[i] > '9') return false;
        number = number * 10 + static_cast<unsigned>(key[i] - '0');
        if (number > INT_MAX) return false;
    }
    return true;
}

bool verifyHelpers(const char *base) {
    for (const char *name : {"WPEWebProcess", "WPENetworkProcess", "WPEGPUProcess"}) {
        const std::string path = std::string(base) + "/wpe-webkit-2.0/" + name;
        Fd helper(openOwned(path.c_str(), false));
        if (helper.value < 0 || faccessat(helper.value, "", X_OK, AT_EMPTY_PATH | AT_EACCESS) != 0)
            return false;
    }
    return true;
}
}

int main(int argc, char **argv) {
    const bool check = argc == 2 && std::strcmp(argv[1], "--check") == 0;
    if (argc != 2 && argc != 4) return fail("invalid authentication arguments");
    if (argc == 4) {
        if (std::strcmp(argv[2], "--device-code") != 0)
            return fail("invalid device authorization arguments");
        const size_t size = strnlen(argv[3], 65);
        if (!size || size > 64) return fail("invalid device authorization code");
        for (size_t i = 0; i < size; ++i) {
            const char value = argv[3][i];
            if (!(value >= 'A' && value <= 'Z') && !(value >= '0' && value <= '9') && value != '-')
                return fail("invalid device authorization code");
        }
    }
    if (!check && (!validUrl(argv[1]) || !validKey()))
        return fail("one HTTPS URL and a valid QTFB_KEY are required; --check tests setup only");
    if (geteuid() != 0) return fail("root ownership is required for the private mount namespace");
    if (unsetenv("LD_PRELOAD") != 0) return fail("could not clear inherited preload settings");

    std::string browserPath, helperPath;
    if (!installationPaths(browserPath, helperPath)) return fail("invalid installation layout");
    const char *Browser = browserPath.c_str();
    const char *Helpers = helperPath.c_str();
    Fd browser(openOwned(Browser, false));
    Fd helpers(openOwned(Helpers, true));
    Fd target(openOwned(MountPoint, true));
    if (browser.value < 0 || helpers.value < 0 || target.value < 0 || !verifyHelpers(Helpers))
        return fail("browser or helper paths failed ownership and executable checks");
    if (faccessat(browser.value, "", X_OK, AT_EMPTY_PATH | AT_EACCESS) != 0)
        return fail("browser is not executable");

    // Only this process and its descendants see the overlay. Neither this
    // process's PID nor the stock interface's mounts or lifecycle are changed.
    if (unshare(CLONE_NEWNS) != 0) return fail("could not create a private mount namespace");
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0)
        return fail("could not make namespace mounts private");
    // Resolve paths in the new namespace. File descriptors opened before
    // unshare still refer to the old namespace's mount tree.
    Fd namespaceHelpers(openOwned(Helpers, true));
    Fd namespaceTarget(openOwned(MountPoint, true));
    struct stat initialSource{}, currentSource{}, initialTarget{}, currentTarget{};
    if (namespaceHelpers.value < 0 || namespaceTarget.value < 0
        || fstat(helpers.value, &initialSource) != 0 || fstat(namespaceHelpers.value, &currentSource) != 0
        || fstat(target.value, &initialTarget) != 0 || fstat(namespaceTarget.value, &currentTarget) != 0
        || initialSource.st_dev != currentSource.st_dev || initialSource.st_ino != currentSource.st_ino
        || initialTarget.st_dev != currentTarget.st_dev || initialTarget.st_ino != currentTarget.st_ino)
        return fail("helper paths changed during namespace setup");
    const std::string sourceFd = "/proc/self/fd/" + std::to_string(namespaceHelpers.value);
    const std::string targetFd = "/proc/self/fd/" + std::to_string(namespaceTarget.value);
    if (mount(sourceFd.c_str(), targetFd.c_str(), nullptr, MS_BIND, nullptr) != 0)
        return fail("could not bind the bundled helper directory");
    if (mount(nullptr, MountPoint, nullptr, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV, nullptr) != 0)
        return fail("could not make the helper overlay read-only");
    struct stat before{}, after{};
    struct statvfs filesystem{};
    Fd mounted(openOwned(MountPoint, true));
    if (mounted.value < 0 || fstat(helpers.value, &before) != 0 || fstat(mounted.value, &after) != 0
        || before.st_dev != after.st_dev || before.st_ino != after.st_ino
        || fstatvfs(mounted.value, &filesystem) != 0 || !(filesystem.f_flag & ST_RDONLY)
        || !verifyHelpers(MountPoint))
        return fail("private helper overlay verification failed");
    if (check) {
        std::puts("rmweb-auth-entry: private read-only helper overlay verified");
        return 0;
    }
    char *arguments[] = {const_cast<char *>(Browser), argv[1],
                         argc == 4 ? argv[2] : nullptr, argc == 4 ? argv[3] : nullptr, nullptr};
    fexecve(browser.value, arguments, environ);
    return fail("could not execute the browser");
}
