#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_LINE   4096
#define MAX_TOKENS 256
#define MAX_ARGS   128
#define MAX_CMDS   32

extern char **environ;

typedef enum {
    T_WORD,
    T_PIPE,
    T_IN,
    T_OUT,
    T_APPEND
} TokenType;

typedef struct {
    TokenType type;
    char *text;
} Token;

typedef struct {
    char *argv[MAX_ARGS];
    int argc;
    char *infile;
    char *outfile;
    int append;
} Command;

static int last_status = 0;
static volatile sig_atomic_t got_sigint = 0;

static int hosted = 0;
static pid_t fg_pids[MAX_CMDS];
static volatile sig_atomic_t fg_count = 0;

static void on_sigint(int sig)
{
    (void)sig;
    got_sigint = 1;
    if (hosted)
        for (int i = 0; i < fg_count; i++)
            kill(fg_pids[i], SIGINT);
    write(STDOUT_FILENO, "\n", 1);
}

static void setup_shell_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    signal(SIGQUIT, SIG_IGN);
}

static void reset_child_signals(void)
{
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
}

static void buf_add(char *buf, size_t *len, char c)
{
    if (*len < MAX_LINE - 1)
        buf[(*len)++] = c;
}

static void buf_add_str(char *buf, size_t *len, const char *s)
{
    while (*s)
        buf_add(buf, len, *s++);
}

static const char *expand_var(const char *p, char *buf, size_t *len)
{
    p++;

    if (*p == '?') {
        char num[16];
        snprintf(num, sizeof num, "%d", last_status);
        buf_add_str(buf, len, num);
        return p + 1;
    }

    if (!isalpha((unsigned char)*p) && *p != '_') {
        buf_add(buf, len, '$');
        return p;
    }

    char name[256];
    size_t nlen = 0;
    while ((isalnum((unsigned char)*p) || *p == '_') && nlen < sizeof name - 1)
        name[nlen++] = *p++;
    name[nlen] = '\0';

    const char *value = getenv(name);
    if (value)
        buf_add_str(buf, len, value);
    return p;
}

static void free_tokens(Token *toks, int n)
{
    for (int i = 0; i < n; i++)
        free(toks[i].text);
}

static int tokenize(const char **pp, Token *toks, int max, int *sep)
{
    int n = 0;
    const char *p = *pp;

    *sep = 0;
    while (*p) {
        while (isspace((unsigned char)*p))
            p++;
        if (*p == '\0')
            break;

        if (*p == ';' || *p == '&') {
            *sep = *p++;
            break;
        }

        if (n >= max) {
            fprintf(stderr, "minishell: too many tokens\n");
            free_tokens(toks, n);
            return -1;
        }

        if (*p == '|') { toks[n++] = (Token){T_PIPE, NULL}; p++; continue; }
        if (*p == '<') { toks[n++] = (Token){T_IN,   NULL}; p++; continue; }
        if (*p == '>') {
            if (p[1] == '>') { toks[n++] = (Token){T_APPEND, NULL}; p += 2; }
            else             { toks[n++] = (Token){T_OUT,    NULL}; p++;    }
            continue;
        }

        char buf[MAX_LINE];
        size_t len = 0;
        int quoted = 0;

        while (*p && !isspace((unsigned char)*p) && !strchr("|&<>;", *p)) {
            if (*p == '\'') {
                quoted = 1;
                p++;
                while (*p && *p != '\'')
                    buf_add(buf, &len, *p++);
                if (*p != '\'') {
                    fprintf(stderr, "minishell: unclosed single quote\n");
                    free_tokens(toks, n);
                    return -1;
                }
                p++;
            } else if (*p == '"') {
                quoted = 1;
                p++;
                while (*p && *p != '"') {
                    if (*p == '$') {
                        p = expand_var(p, buf, &len);
                    } else if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) {
                        buf_add(buf, &len, p[1]);
                        p += 2;
                    } else {
                        buf_add(buf, &len, *p++);
                    }
                }
                if (*p != '"') {
                    fprintf(stderr, "minishell: unclosed double quote\n");
                    free_tokens(toks, n);
                    return -1;
                }
                p++;
            } else if (*p == '$') {
                p = expand_var(p, buf, &len);
            } else if (*p == '\\' && p[1]) {
                buf_add(buf, &len, p[1]);
                p += 2;
            } else {
                buf_add(buf, &len, *p++);
            }
        }
        buf[len] = '\0';

        if (len == 0 && !quoted)
            continue;

        toks[n].type = T_WORD;
        toks[n].text = strdup(buf);
        if (toks[n].text == NULL) {
            perror("minishell: strdup");
            free_tokens(toks, n);
            return -1;
        }
        n++;
    }
    *pp = p;
    return n;
}

static int parse(Token *toks, int ntok, Command *cmds)
{
    int nc = 0;
    memset(&cmds[0], 0, sizeof(Command));

    for (int i = 0; i < ntok; i++) {
        Command *c = &cmds[nc];

        switch (toks[i].type) {
        case T_WORD:
            if (c->argc >= MAX_ARGS - 1) {
                fprintf(stderr, "minishell: too many arguments\n");
                return -1;
            }
            c->argv[c->argc++] = toks[i].text;
            break;

        case T_IN:
        case T_OUT:
        case T_APPEND:
            if (i + 1 >= ntok || toks[i + 1].type != T_WORD) {
                fprintf(stderr, "minishell: syntax error: missing file name after redirection\n");
                return -1;
            }
            if (toks[i].type == T_IN) {
                c->infile = toks[i + 1].text;
            } else {
                c->outfile = toks[i + 1].text;
                c->append = (toks[i].type == T_APPEND);
            }
            i++;
            break;

        case T_PIPE:
            if (c->argc == 0) {
                fprintf(stderr, "minishell: syntax error near '|'\n");
                return -1;
            }
            if (nc + 1 >= MAX_CMDS) {
                fprintf(stderr, "minishell: pipeline too long\n");
                return -1;
            }
            nc++;
            memset(&cmds[nc], 0, sizeof(Command));
            break;
        }
    }

    if (cmds[nc].argc == 0) {
        fprintf(stderr, "minishell: syntax error: missing command\n");
        return -1;
    }
    return nc + 1;
}

static int is_builtin(const char *name)
{
    return strcmp(name, "cd") == 0     || strcmp(name, "pwd") == 0   ||
           strcmp(name, "exit") == 0   || strcmp(name, "export") == 0 ||
           strcmp(name, "unset") == 0  || strcmp(name, "help") == 0;
}

static int builtin_cd(Command *c)
{
    const char *dir = c->argc > 1 ? c->argv[1] : getenv("HOME");
    int go_back = (dir != NULL && strcmp(dir, "-") == 0);

    if (go_back)
        dir = getenv("OLDPWD");
    if (dir == NULL) {
        fprintf(stderr, "minishell: cd: %s not set\n", go_back ? "OLDPWD" : "HOME");
        return 1;
    }

    char old[MAX_LINE];
    int have_old = getcwd(old, sizeof old) != NULL;

    if (chdir(dir) != 0) {
        fprintf(stderr, "minishell: cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    if (have_old)
        setenv("OLDPWD", old, 1);

    char cwd[MAX_LINE];
    if (getcwd(cwd, sizeof cwd)) {
        setenv("PWD", cwd, 1);
        if (go_back)
            printf("%s\n", cwd);
    }
    return 0;
}

static int builtin_pwd(void)
{
    char cwd[MAX_LINE];
    if (getcwd(cwd, sizeof cwd) == NULL) {
        perror("minishell: pwd");
        return 1;
    }
    printf("%s\n", cwd);
    return 0;
}

static int builtin_export(Command *c)
{
    if (c->argc == 1) {
        for (char **e = environ; *e; e++)
            printf("%s\n", *e);
        return 0;
    }
    int status = 0;
    for (int i = 1; i < c->argc; i++) {
        char *eq = strchr(c->argv[i], '=');
        if (eq == NULL || eq == c->argv[i]) {
            fprintf(stderr, "minishell: export: usage: export NAME=value\n");
            status = 1;
            continue;
        }
        *eq = '\0';
        if (setenv(c->argv[i], eq + 1, 1) != 0) {
            perror("minishell: export");
            status = 1;
        }
        *eq = '=';
    }
    return status;
}

static int builtin_unset(Command *c)
{
    for (int i = 1; i < c->argc; i++)
        unsetenv(c->argv[i]);
    return 0;
}

static int builtin_help(void)
{
    printf("Minishell built-ins:\n"
           "  cd [dir]         change directory (default: $HOME)\n"
           "  pwd              print current directory\n"
           "  export NAME=val  set an environment variable\n"
           "  unset NAME       remove an environment variable\n"
           "  exit [code]      leave the shell\n"
           "  help             show this message\n"
           "Supports: | ; & < > >> 'quotes' \"quotes\" $VAR $?\n");
    return 0;
}

static int run_builtin(Command *c)
{
    const char *name = c->argv[0];

    if (strcmp(name, "cd") == 0)     return builtin_cd(c);
    if (strcmp(name, "pwd") == 0)    return builtin_pwd();
    if (strcmp(name, "export") == 0) return builtin_export(c);
    if (strcmp(name, "unset") == 0)  return builtin_unset(c);
    if (strcmp(name, "help") == 0)   return builtin_help();
    if (strcmp(name, "exit") == 0) {
        int code = c->argc > 1 ? atoi(c->argv[1]) : last_status;
        exit(code);
    }
    return 1;
}

static int apply_redirections(Command *c)
{
    if (c->infile) {
        int fd = open(c->infile, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "minishell: %s: %s\n", c->infile, strerror(errno));
            return -1;
        }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (c->outfile) {
        int flags = O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC);
        int fd = open(c->outfile, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "minishell: %s: %s\n", c->outfile, strerror(errno));
            return -1;
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    return 0;
}

static int decode_status(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}

static void run_builtin_in_parent(Command *c)
{
    int saved_in = dup(STDIN_FILENO);
    int saved_out = dup(STDOUT_FILENO);

    if (apply_redirections(c) == 0)
        last_status = run_builtin(c);
    else
        last_status = 1;

    fflush(stdout);
    dup2(saved_in, STDIN_FILENO);
    dup2(saved_out, STDOUT_FILENO);
    close(saved_in);
    close(saved_out);
}

static void execute(Command *cmds, int n, int background)
{
    if (n == 1 && !background && is_builtin(cmds[0].argv[0])) {
        run_builtin_in_parent(&cmds[0]);
        return;
    }

    pid_t *pids = fg_pids;
    int spawned = 0;
    int prev_read = -1;

    fflush(stdout);

    for (int i = 0; i < n; i++) {
        int fds[2] = {-1, -1};

        if (i < n - 1 && pipe(fds) < 0) {
            perror("minishell: pipe");
            break;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("minishell: fork");
            if (fds[0] != -1) { close(fds[0]); close(fds[1]); }
            break;
        }

        if (pid == 0) {
            reset_child_signals();

            if (prev_read != -1) {
                dup2(prev_read, STDIN_FILENO);
                close(prev_read);
            }
            if (fds[1] != -1) {
                dup2(fds[1], STDOUT_FILENO);
                close(fds[1]);
                close(fds[0]);
            }
            if (apply_redirections(&cmds[i]) < 0)
                exit(1);

            if (is_builtin(cmds[i].argv[0]))
                exit(run_builtin(&cmds[i]));

            execvp(cmds[i].argv[0], cmds[i].argv);
            fprintf(stderr, "minishell: %s: %s\n", cmds[i].argv[0],
                    errno == ENOENT ? "command not found" : strerror(errno));
            exit(errno == ENOENT ? 127 : 126);
        }

        pids[spawned++] = pid;
        if (!background)
            fg_count = spawned;
        if (prev_read != -1)
            close(prev_read);
        if (fds[1] != -1)
            close(fds[1]);
        prev_read = fds[0];
    }
    if (prev_read != -1)
        close(prev_read);

    if (background) {
        if (spawned > 0)
            printf("[%d]\n", (int)pids[spawned - 1]);
        last_status = 0;
        return;
    }

    for (int i = 0; i < spawned; i++) {
        int status;
        while (waitpid(pids[i], &status, 0) < 0) {
            if (errno != EINTR) {
                status = 1 << 8;
                break;
            }
        }
        if (i == spawned - 1)
            last_status = decode_status(status);
    }
    fg_count = 0;
    if (spawned < n)
        last_status = 1;
}

static void reap_background_jobs(void)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        printf("[%d] done (status %d)\n", (int)pid, decode_status(status));
}

static void print_prompt(void)
{
    char cwd[MAX_LINE];
    if (getcwd(cwd, sizeof cwd))
        printf("\033[1;32mminishell\033[0m:\033[1;34m%s\033[0m$ ", cwd);
    else
        printf("minishell$ ");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    char line[MAX_LINE];
    Token toks[MAX_TOKENS];
    Command cmds[MAX_CMDS];
    int interactive = isatty(STDIN_FILENO);

    if (argc > 1 && strcmp(argv[1], "-i") == 0 && !interactive) {
        interactive = 1;
        hosted = 1;
    }

    setup_shell_signals();

    for (;;) {
        reap_background_jobs();
        got_sigint = 0;
        if (interactive)
            print_prompt();

        if (fgets(line, sizeof line, stdin) == NULL) {
            if (got_sigint) {
                clearerr(stdin);
                continue;
            }
            if (interactive)
                printf("exit\n");
            break;
        }

        if (strchr(line, '\n') == NULL && !feof(stdin)) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF)
                ;
            fprintf(stderr, "minishell: line too long\n");
            last_status = 1;
            continue;
        }
        line[strcspn(line, "\n")] = '\0';

        const char *p = line;
        while (*p) {
            int sep;
            int ntok = tokenize(&p, toks, MAX_TOKENS, &sep);
            if (ntok < 0) {
                last_status = 2;
                break;
            }
            if (ntok == 0) {
                if (sep) {
                    fprintf(stderr, "minishell: syntax error near '%c'\n", sep);
                    last_status = 2;
                }
                break;
            }

            int ncmds = parse(toks, ntok, cmds);

            if (ncmds > 0)
                execute(cmds, ncmds, sep == '&');
            else
                last_status = 2;

            free_tokens(toks, ntok);
            if (ncmds < 0 || got_sigint)
                break;
        }
    }

    return last_status;
}
