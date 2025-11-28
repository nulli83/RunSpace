#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

#define P 8080 // Port
#define BS 65536 // Buffertstorlek
#define TMP_SRC "/tmp/online_code.c"
#define TMP_BIN "/tmp/compiled_bin"

// Enkel utgång (ofta inline i riktiga fall, men behålls för struktur)
static void die(const char *m) {
    perror(m);
    exit(1);
}

// Skriver hela bufferten (mer robust än enkel write)
static void w_all(int fd, const char *b, size_t n) {
    size_t t = 0;
    while (t < n) {
        ssize_t w = write(fd, b + t, n - t);
        if (w < 0) {
            if (errno == EINTR) continue;
            perror("write_all");
            break;
        }
        t += (size_t)w;
    }
}

// Skicka svar (kortare namn, minimala headers)
static void resp(int c, int st, const char *t, const char *b) {
    char h[512];
    int l = b ? (int)strlen(b) : 0;

    int n = snprintf(
        h, sizeof(h),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        st, t, l
    );
    w_all(c, h, (size_t)n);
    if (l > 0) w_all(c, b, (size_t)l);
}

// Kör gcc (säker fork/exec, ingen shell)
static char *compile(const char *src, int *ec) {
    int pfd[2];
    if (pipe(pfd) == -1) { *ec = -1; return strdup("pipe error\n"); }
    pid_t pid = fork();

    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        
        // Mycket erfarna C-programmerare använder exec-familjen direkt.
        execlp("gcc", "gcc", src, "-o", TMP_BIN, (char *)NULL);
        _exit(127); 
    }

    close(pfd[1]);
    
    char *out = NULL;
    size_t cap = 0, len = 0;
    char buf[512];
    ssize_t r;
    while ((r = read(pfd[0], buf, sizeof(buf))) > 0) {
        if (len + r + 1 > cap) {
            size_t nc = cap == 0 ? 512 : cap * 2;
            char *tmp = realloc(out, nc);
            if (!tmp) { free(out); close(pfd[0]); *ec = -1; return strdup("mem error\n"); }
            out = tmp; cap = nc;
        }
        memcpy(out + len, buf, (size_t)r);
        len += (size_t)r;
    }
    close(pfd[0]);

    int status;
    if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status)) { *ec = -1; }
    else { *ec = WEXITSTATUS(status); }

    if (!out) out = strdup("");
    else out[len] = '\0';
    return out;
}


// Huvudhanterare för POST /compile
static void handle(int c, char *b, ssize_t n) {
    b[n] = '\0';

    char *cl = strstr(b, "Content-Length:");
    if (!cl) { resp(c, 400, "Bad Request", "Missing CL\n"); return; }

    int clen = 0;
    if (sscanf(cl, "Content-Length:%*[^0-9]%d", &clen) != 1 || clen <= 0 || clen > BS) {
        resp(c, 400, "Bad Request", "Invalid CL\n"); return;
    }

    char *body = strstr(b, "\r\n\r\n");
    if (!body) { resp(c, 400, "Bad Request", "No body separator\n"); return; }
    body += 4;

    // Läs hela body
    int hlen = (int)(body - b);
    int already = (int)n - hlen;
    char *code = malloc((size_t)clen + 1);
    if (!code) { resp(c, 500, "ISE", "mem fail\n"); return; }

    if (already > clen) already = clen;
    memcpy(code, body, (size_t)already);

    int left = clen - already;
    while (left > 0) {
        ssize_t r = read(c, code + (clen - left), (size_t)left);
        if (r <= 0) { free(code); resp(c, 400, "Bad Request", "Body read fail\n"); return; }
        left -= (int)r;
    }
    code[clen] = '\0';

    // Skriv tempfil (lågnivå I/O)
    int fd = open(TMP_SRC, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { free(code); resp(c, 500, "ISE", "file open fail\n"); return; }
    w_all(fd, code, (size_t)clen);
    close(fd);
    free(code);

    // Kompilera och svara
    int ec = 0;
    char *gcc_out = compile(TMP_SRC, &ec);

    char *r_msg = NULL;
    const char *p = ec == 0 ? "OK\n" : "FAIL\n";
    size_t r_len = strlen(p) + strlen(gcc_out) + 32;
    r_msg = malloc(r_len);
    if (!r_msg) { free(gcc_out); resp(c, 500, "ISE", "resp mem fail\n"); return; }

    snprintf(r_msg, r_len, "%sExit: %d\n\n%s", p, ec, gcc_out);
    free(gcc_out);

    int st = (ec == 0) ? 200 : 400;
    resp(c, st, st == 200 ? "OK" : "Bad Request", r_msg);
    free(r_msg);
}

// Hantera socket
int main() {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) die("socket");

    int o = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &o, sizeof(o));

    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(P);

    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) die("bind");
    if (listen(s, 8) < 0) die("listen");

    printf("Running on :%d\n", P);

    while (1) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        char buf[BS + 1];

        int c = accept(s, (struct sockaddr *)&ca, &cl);
        if (c < 0) { perror("accept"); continue; }
        
        ssize_t n = read(c, buf, BS);
        if (n <= 0) { close(c); continue; }

        if (strncmp(buf, "POST /compile", 13) == 0) {
            handle(c, buf, n);
        } else if (strncmp(buf, "OPTIONS ", 8) == 0) {
            resp(c, 204, "No Content", "");
        } else {
            resp(c, 404, "Not Found", "POST /compile\n");
        }
        close(c);
    }
    close(s);
    return 0;
}
