
// func.c (Kärnfunktioner för servern)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

#define TMP_BASE "/tmp/online_code" // Basnamn för källfiler
#define TMP_BIN "/tmp/compiled_bin"
// OBS: De globala definitionerna P, BS, w_all, resp är deklarerade i huvudfilen
// och antas vara tillgängliga eller inkluderade.


// Skriver hela bufferten (mer robust än enkel write)
static void w_all(int fd, const char *b, size_t n); // Antas finnas i huvudfilen.

// Skickar HTTP-svar
static void resp(int c, int st, const char *t, const char *b, const char *ct_override); // Antas finnas i huvudfilen.

// --- Ny exekveringsmotor (run_subprocess och execute_code) ---

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

// Väljer rätt strategi och exekverar koden
static char *execute_code(const char *lang, const char *code_path, int *ec, char *tmp_filename) {
    
    const char *ext = NULL;
    if (strcmp(lang, "c") == 0) ext = ".c";
    else if (strcmp(lang, "c++") == 0) ext = ".cpp";
    else if (strcmp(lang, "java") == 0) ext = ".java";
    else if (strcmp(lang, "c#") == 0) ext = ".cs";
    else if (strcmp(lang, "python") == 0) ext = ".py";
    else if (strcmp(lang, "javascript") == 0) ext = ".js";
    else if (strcmp(lang, "html") == 0) ext = ".html";
    else { *ec = 1; return strdup("Unsupported language ID\n"); }

    char src_path[256];
    snprintf(src_path, sizeof(src_path), "%s%s", code_path, ext);
    strcpy(tmp_filename, src_path);

    if (strcmp(lang, "html") == 0) {
        *ec = 0;
        return strdup(src_path); 
    }

    char *compiler_out = NULL;
    char *exec_cmd = NULL;
    char *exec_arg = NULL;
    int needs_compilation = 1;
    
    // --- Kompilering ---
    if (strcmp(lang, "c") == 0 || strcmp(lang, "c++") == 0) {
        const char *comp = (strcmp(lang, "c") == 0) ? "gcc" : "g++";
        char *const argv[] = {(char*)comp, (char*)src_path, "-o", (char*)TMP_BIN, NULL};
        compiler_out = run_subprocess(comp, argv, ec);
        exec_cmd = (char*)TMP_BIN;
        
    } else if (strcmp(lang, "java") == 0) {
        char *const argv[] = {"javac", (char*)src_path, NULL};
        compiler_out = run_subprocess("javac", argv, ec);
        exec_cmd = "java";
        exec_arg = "online_code"; // Klassnamn måste matcha TMP_BASE
    } 
    // OBS: C#-logiken är för komplex att inkludera här utan att dra in mer C#-specifik kod/variabler.
    // Lösningen i huvudfilen hanterar C# med extra logik för projektfiler.
    else if (strcmp(lang, "c#") == 0) {
        // Detta block skulle vara platsen för C# hantering (dotnet build)
        // ... (Kortad C# logik) ...
        *ec = 1; return strdup("C# is complex, only fully implemented in main file.\n");
    }
    
    else {
        needs_compilation = 0; // Tolkade språk
    }

    if (needs_compilation && *ec != 0) {
        return compiler_out;
    }
    if (needs_compilation && compiler_out) free(compiler_out);

    // --- Exekvering ---
    if (strcmp(lang, "python") == 0) {
        char *const argv[] = {"python3", (char*)src_path, NULL};
        return run_subprocess("python3", argv, ec);
    } else if (strcmp(lang, "javascript") == 0) {
        char *const argv[] = {"node", (char*)src_path, NULL};
        return run_subprocess("node", argv, ec);
    } else if (needs_compilation) {
        // C, C++, Java körning
        // Anpassa argv för Java (behöver -cp /tmp för att hitta klassfil)
        if (strcmp(lang, "java") == 0) {
            char *const argv_java[] = {"java", "-cp", "/tmp", "online_code", NULL};
            return run_subprocess("java", argv_java, ec);
        }
        
        char *const argv[] = {exec_cmd, NULL}; // C/C++ binär
        return run_subprocess(exec_cmd, argv, ec);
    }

    *ec = 1;
    return strdup("Execution logic error.\n"); 
}
