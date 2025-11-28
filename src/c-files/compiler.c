// compiler_multilang_ext.c - Stödjer C, C++, Python, Node, Java, C#, HTML
// Kräver: gcc compiler_multilang_ext.c -o runner_server

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

#define P 8080 
#define BS 65536 
#define TMP_BASE "/tmp/online_code" // Basnamn för källfiler

// --- Hjälpfunktioner ---

static void die(const char *m) {
    perror(m);
    exit(1);
}

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

// Skickar HTTP-svar
static void resp(int c, int st, const char *t, const char *b, const char *ct_override) {
    char h[512];
    int l = b ? (int)strlen(b) : 0;
    const char *content_type = ct_override ? ct_override : "text/plain";

    int n = snprintf(
        h, sizeof(h),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s; charset=utf-8\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        st, t, content_type, l
    );
    w_all(c, h, (size_t)n);
    if (l > 0) w_all(c, b, (size_t)l);
}

// Hanterar körning via fork/exec/pipe
static char *run_subprocess(const char *cmd, char *const argv[], int *ec) {
    int pfd[2];
    if (pipe(pfd) == -1) { *ec = -1; return strdup("Pipe error\n"); }
    pid_t pid = fork();

    if (pid == 0) { // Barnprocess
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        
        execvp(cmd, argv); // Använder execvp för att söka i PATH
        perror("Execvp failed");
        _exit(127); 
    }

    // Föräldraprocess (läser utdata)
    close(pfd[1]);
    char *out = NULL;
    size_t cap = 0, len = 0;
    char buf[512];
    ssize_t r;
    while ((r = read(pfd[0], buf, sizeof(buf))) > 0) {
        if (len + r + 1 > cap) {
            size_t nc = cap == 0 ? 512 : cap * 2;
            char *tmp = realloc(out, nc);
            if (!tmp) { free(out); close(pfd[0]); *ec = -1; return strdup("Mem error during output read\n"); }
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

// --- Ny exekveringsmotor ---

// Returnerar utdata, eller kompileringsfel. Måste free:as.
static char *execute_code(const char *lang, const char *code_path, int *ec, char *tmp_filename) {
    
    // 1. Språk och filändelse
    const char *ext = NULL;
    if (strcmp(lang, "c") == 0) ext = ".c";
    else if (strcmp(lang, "c++") == 0) ext = ".cpp";
    else if (strcmp(lang, "java") == 0) ext = ".java";
    else if (strcmp(lang, "c#") == 0) ext = ".cs";
    else if (strcmp(lang, "python") == 0) ext = ".py";
    else if (strcmp(lang, "javascript") == 0) ext = ".js";
    else if (strcmp(lang, "html") == 0) ext = ".html"; // Särskilt fall
    else { *ec = 1; return strdup("Unsupported language ID\n"); }

    // 2. Skapa fullständig källfilssökväg
    char src_path[256];
    snprintf(src_path, sizeof(src_path), "%s%s", code_path, ext);
    strcpy(tmp_filename, src_path); // Spara fullständiga namnet

    // 3. Hantera Frontend/Markup språk
    if (strcmp(lang, "html") == 0) {
        *ec = 0;
        return strdup(src_path); // Returnera sökvägen istället för utdata
    }

    // --- Steg 4: Kompilering (för C, C++, Java, C#) ---
    char *compiler_out = NULL;
    char *exec_cmd = NULL;
    char *exec_arg = NULL;
    int needs_compilation = 1;
    
    if (strcmp(lang, "c") == 0 || strcmp(lang, "c++") == 0) {
        const char *comp = (strcmp(lang, "c") == 0) ? "gcc" : "g++";
        char *const argv[] = {(char*)comp, (char*)src_path, "-o", (char*)TMP_BIN, NULL};
        compiler_out = run_subprocess(comp, argv, ec);
        exec_cmd = (char*)TMP_BIN;
        
    } else if (strcmp(lang, "java") == 0) {
        char *const argv[] = {"javac", (char*)src_path, NULL};
        compiler_out = run_subprocess("javac", argv, ec);
        exec_cmd = "java";
        exec_arg = "online_code"; // Java-klassnamn (måste matcha TMP_BASE)

    } else if (strcmp(lang, "c#") == 0) {
        // Skapa temporär projektfil (.csproj) och kompilera med dotnet
        char proj_path[256]; snprintf(proj_path, sizeof(proj_path), "%s.csproj", code_path);
        int fd = open(proj_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        w_all(fd, "<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net6.0</TargetFramework></PropertyGroup></Project>", 150);
        close(fd);
        
        char build_dir[256]; snprintf(build_dir, sizeof(build_dir), "PublishDir=%s_build", code_path);
        
        // Kompilera och placera i temporär mapp
        char *const argv[] = {"dotnet", "build", (char*)proj_path, "-o", build_dir, "/nologo", NULL};
        compiler_out = run_subprocess("dotnet", argv, ec);

        exec_cmd = "dotnet";
        exec_arg = "online_code.dll"; // Namnet på utdatafilen
        
    } else {
        needs_compilation = 0; // Tolkade språk
    }

    if (needs_compilation && *ec != 0) {
        // Kompileringsfel
        return compiler_out;
    }
    if (needs_compilation && compiler_out) free(compiler_out); // Frigör OK kompileringsutdata

    // --- Steg 5: Exekvering ---
    if (strcmp(lang, "python") == 0) {
        char *const argv[] = {"python3", (char*)src_path, NULL};
        return run_subprocess("python3", argv, ec);
    } else if (strcmp(lang, "javascript") == 0) {
        char *const argv[] = {"node", (char*)src_path, NULL};
        return run_subprocess("node", argv, ec);
    } else if (needs_compilation) {
        // C, C++, Java, C# körning
        char *const argv[] = {exec_cmd, exec_arg, NULL};
        return run_subprocess(exec_cmd, argv, ec);
    }

    *ec = 1;
    return strdup("Execution logic error.\n"); // Fallback
}


// Huvudhanterare för POST /run
static void handle(int c, char *b, ssize_t n) {
    b[n] = '\0';
    
    // ... (Här finns logiken för att läsa Content-Length och hela body:n till 'code') ...
    // (Använder den robusta läs- och parslogiken från tidigare versioner)
    
    char *cl = strstr(b, "Content-Length:");
    if (!cl) { resp(c, 400, "Bad Request", "Missing CL\n", NULL); return; }
    int clen = 0;
    if (sscanf(cl, "Content-Length:%*[^0-9]%d", &clen) != 1 || clen <= 0 || clen > BS) {
        resp(c, 400, "Bad Request", "Invalid CL\n", NULL); return;
    }
    char *body = strstr(b, "\r\n\r\n");
    if (!body) { resp(c, 400, "Bad Request", "No body separator\n", NULL); return; }
    body += 4;

    char *code = malloc((size_t)clen + 1);
    if (!code) { resp(c, 500, "ISE", "mem fail\n", NULL); return; }
    int already = (int)n - (int)(body - b);
    if (already > clen) already = clen;
    memcpy(code, body, (size_t)already);
    int left = clen - already;
    while (left > 0) {
        ssize_t r = read(c, code + (clen - left), (size_t)left);
        if (r <= 0) { free(code); resp(c, 400, "Bad Request", "Body read fail\n", NULL); return; }
        left -= (int)r;
    }
    code[clen] = '\0';
    
    // 3. Separera språk från kod (MÅSTE vara på första raden)
    char language[32] = {0};
    char *code_start = NULL;
    
    if (sscanf(code, "LANGUAGE: %31s\n", language) == 1) {
        code_start = strchr(code, '\n');
        if (code_start) code_start++; 
        else code_start = code;
    } else {
        free(code);
        resp(c, 400, "Bad Request", "Missing LANGUAGE header in body.\n", NULL);
        return;
    }
    
    // Skriv endast koden till tempfilen
    size_t code_len = strlen(code_start);
    char full_src_path[256];
    snprintf(full_src_path, sizeof(full_src_path), "%s%s", TMP_BASE, ".tmp"); // Temporär fil utan extension

    int fd = open(full_src_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { free(code); resp(c, 500, "ISE", "file open fail\n", NULL); return; }
    w_all(fd, code_start, code_len);
    close(fd);
    free(code); 

    // 4. Exekvera och svara
    int ec = 0;
    char temp_filename_out[256]; // För att få det fullständiga filnamnet
    char *output = execute_code(language, TMP_BASE, &ec, temp_filename_out);

    if (strcmp(language, "html") == 0) {
        // Särskilt fall för HTML: Returnera filens innehåll som HTML
        char *file_path = output; // Output är sökvägen
        
        // För enkelhetens skull, returnera en länk till filen,
        // eller implementera logik för att läsa in och skicka filinnehållet här.
        char final_resp[256];
        snprintf(final_resp, sizeof(final_resp), "HTML content saved to: %s. You must access this file manually.", file_path);
        
        resp(c, 200, "OK", final_resp, "text/plain");
        free(output);
    } else {
        // Standardfall: Textutdata
        char *r_msg = NULL;
        const char *p = ec == 0 ? "OK\n" : "FAIL\n";
        size_t r_len = strlen(p) + strlen(output) + 32;
        r_msg = malloc(r_len);
        if (!r_msg) { free(output); resp(c, 500, "ISE", "resp mem fail\n", NULL); return; }

        snprintf(r_msg, r_len, "%sExit: %d\n\n%s", p, ec, output);
        free(output);

        int st = (ec == 0) ? 200 : 400;
        resp(c, st, st == 200 ? "OK" : "Bad Request", r_msg, NULL);
        free(r_msg);
    }
}

// Hantera socket
int main() {
    // ... (Main-loopen är oförändrad från föregående version) ...
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

    printf("Multi-language runner running on :%d\n", P);

    while (1) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        char buf[BS + 1];

        int c = accept(s, (struct sockaddr *)&ca, &cl);
        if (c < 0) { perror("accept"); continue; }
        
        ssize_t n = read(c, buf, BS);
        if (n <= 0) { close(c); continue; }

        if (strncmp(buf, "POST /run", 9) == 0) { 
            handle(c, buf, n);
        } else if (strncmp(buf, "OPTIONS ", 8) == 0) {
            resp(c, 204, "No Content", "", NULL);
        } else {
            resp(c, 404, "Not Found", "POST /run\n", NULL);
        }
        close(c);
    }
    close(s);
    return 0;
}
