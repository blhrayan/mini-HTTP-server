#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BACKLOG 16
#define REQ_MAX 8192
#define DEFAULT_PORT 8080

static char root_real[PATH_MAX];
static size_t root_len;

static const struct { const char *ext; const char *mime; } mimes[] = {
    {".html", "text/html; charset=utf-8"},
    {".htm",  "text/html; charset=utf-8"},
    {".css",  "text/css; charset=utf-8"},
    {".js",   "application/javascript"},
    {".json", "application/json"},
    {".txt",  "text/plain; charset=utf-8"},
    {".png",  "image/png"},
    {".jpg",  "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif",  "image/gif"},
    {".svg",  "image/svg+xml"},
    {".ico",  "image/x-icon"},
    {".pdf",  "application/pdf"},
};

static const char *mime_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (dot) {
        for (size_t i = 0; i < sizeof(mimes) / sizeof(mimes[0]); i++)
            if (strcasecmp(dot, mimes[i].ext) == 0)
                return mimes[i].mime;
    }
    return "application/octet-stream";
}

static void send_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = send(fd, buf, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        buf += n;
        len -= (size_t)n;
    }
}

static void send_error(int fd, int code, const char *reason, int head_only) {
    char body[256], header[256];
    int blen = snprintf(body, sizeof(body),
        "<html><body><h1>%d %s</h1></body></html>\n", code, reason);
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n", code, reason, blen);
    send_all(fd, header, (size_t)hlen);
    if (!head_only) send_all(fd, body, (size_t)blen);
}

static int url_decode(char *s) {
    char *out = s;
    while (*s) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            int v = (int)strtol(hex, NULL, 16);
            if (v == 0) return -1;
            *out++ = (char)v;
            s += 3;
        } else {
            *out++ = *s++;
        }
    }
    *out = '\0';
    return 0;
}

static void log_request(const char *ip, const char *method, const char *path, int status) {
    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&now));
    printf("[%s] %-15s %-4s %-30.60s -> %d\n", ts, ip, method, path, status);
    fflush(stdout);
}

static void handle_client(int fd, const char *ip) {
    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char req[REQ_MAX + 1];
    size_t n = 0;
    while (n < REQ_MAX) {
        ssize_t r = recv(fd, req + n, REQ_MAX - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
        req[n] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (n == 0) return;
    req[n] = '\0';

    char method[16], target[2048], version[16];
    if (sscanf(req, "%15s %2047s %15s", method, target, version) != 3) {
        send_error(fd, 400, "Bad Request", 0);
        log_request(ip, "?", "?", 400);
        return;
    }

    int head_only = strcmp(method, "HEAD") == 0;
    if (strcmp(method, "GET") != 0 && !head_only) {
        send_error(fd, 405, "Method Not Allowed", 0);
        log_request(ip, method, target, 405);
        return;
    }

    char *q = strpbrk(target, "?#");
    if (q) *q = '\0';
    char shown[2048];
    snprintf(shown, sizeof(shown), "%s", target);

    if (target[0] != '/' || url_decode(target) != 0) {
        send_error(fd, 400, "Bad Request", head_only);
        log_request(ip, method, shown, 400);
        return;
    }
    if (strstr(target, "..")) {
        send_error(fd, 403, "Forbidden", head_only);
        log_request(ip, method, shown, 403);
        return;
    }

    char full[PATH_MAX];
    if (snprintf(full, sizeof(full), "%s%s", root_real, target) >= (int)sizeof(full)) {
        send_error(fd, 414, "URI Too Long", head_only);
        log_request(ip, method, shown, 414);
        return;
    }

    struct stat st;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        size_t l = strlen(full);
        const char *sep = (full[l - 1] == '/') ? "" : "/";
        if (snprintf(full + l, sizeof(full) - l, "%sindex.html", sep) >= (int)(sizeof(full) - l)) {
            send_error(fd, 414, "URI Too Long", head_only);
            log_request(ip, method, shown, 414);
            return;
        }
    }

    char resolved[PATH_MAX];
    if (!realpath(full, resolved)) {
        send_error(fd, 404, "Not Found", head_only);
        log_request(ip, method, shown, 404);
        return;
    }

    if (strncmp(resolved, root_real, root_len) != 0 ||
        (resolved[root_len] != '/' && resolved[root_len] != '\0')) {
        send_error(fd, 403, "Forbidden", head_only);
        log_request(ip, method, shown, 403);
        return;
    }

    if (stat(resolved, &st) != 0 || !S_ISREG(st.st_mode)) {
        send_error(fd, 404, "Not Found", head_only);
        log_request(ip, method, shown, 404);
        return;
    }

    int file = open(resolved, O_RDONLY);
    if (file < 0) {
        send_error(fd, 403, "Forbidden", head_only);
        log_request(ip, method, shown, 403);
        return;
    }

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lld\r\n"
        "Connection: close\r\n\r\n",
        mime_for(resolved), (long long)st.st_size);
    send_all(fd, header, (size_t)hlen);

    if (!head_only) {
        char buf[8192];
        ssize_t r;
        while ((r = read(file, buf, sizeof(buf))) > 0)
            send_all(fd, buf, (size_t)r);
    }
    close(file);
    log_request(ip, method, shown, 200);
}

static void reap_children(int sig) {
    (void)sig;
    int saved = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    errno = saved;
}

int main(int argc, char *argv[]) {
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;
    const char *root = (argc > 2) ? argv[2] : "./www";

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        return 1;
    }
    if (!realpath(root, root_real)) {
        perror("document root");
        return 1;
    }
    root_len = strlen(root_real);

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reap_children;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(listener, BACKLOG) < 0) {
        perror("listen");
        return 1;
    }

    printf("Serving %s on http://localhost:%d (Ctrl+C to stop)\n", root_real, port);
    fflush(stdout);

    while (1) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int fd = accept(listener, (struct sockaddr *)&client, &clen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));

        pid_t pid = fork();
        if (pid == 0) {
            close(listener);
            handle_client(fd, ip);
            close(fd);
            _exit(0);
        } else if (pid < 0) {
            perror("fork");
        }
        close(fd);
    }
}
