/*
 * Shell `simplesh` (basado en el shell de xv6)
 *
 * Ampliación de Sistemas Operativos
 * Departamento de Ingeniería y Tecnología de Computadores
 * Facultad de Informática de la Universidad de Murcia
 *
 * Alumnos: APELLIDOS, NOMBRE (GX.X)
 *          APELLIDOS, NOMBRE (GX.X)
 *
 * Convocatoria: FEBRERO/JUNIO/JULIO
 */

/*
 * Ficheros de cabecera
 */

#define _POSIX_C_SOURCE 200809L /* IEEE 1003.1-2008 (véase /usr/include/features.h) */
//#define NDEBUG                /* Traduce asertos y DMACROS a 'no ops' */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>
#include <limits.h>
#include <libgen.h>

// Biblioteca readline
#include <readline/readline.h>
#include <readline/history.h>

/******************************************************************************
 * Constantes, macros y variables globales
 ******************************************************************************/

static const char *VERSION = "0.19";

// Niveles de depuración
#define DBG_CMD (1 << 0)
#define DBG_TRACE (1 << 1)
// . . .
static int g_dbg_level = 0;

#ifndef NDEBUG
#define DPRINTF(dbg_level, fmt, ...)                              \
    do                                                            \
    {                                                             \
        if (dbg_level & g_dbg_level)                              \
            fprintf(stderr, "%s:%d:%s(): " fmt,                   \
                    __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
    } while (0)

#define DBLOCK(dbg_level, block)     \
    do                               \
    {                                \
        if (dbg_level & g_dbg_level) \
            block;                   \
    } while (0);
#else
#define DPRINTF(dbg_level, fmt, ...)
#define DBLOCK(dbg_level, block)
#endif

#define TRY(x)                                              \
    do                                                      \
    {                                                       \
        int __rc = (x);                                     \
        if (__rc < 0)                                       \
        {                                                   \
            fprintf(stderr, "%s:%d:%s: TRY(%s) failed\n",   \
                    __FILE__, __LINE__, __func__, #x);      \
            fprintf(stderr, "ERROR: rc=%d errno=%d (%s)\n", \
                    __rc, errno, strerror(errno));          \
            exit(EXIT_FAILURE);                             \
        }                                                   \
    } while (0)

// Número máximo de argumentos de un comando
#define MAX_ARGS 16

#define BSIZE_MAX 1048576

// Delimitadores
static const char WHITESPACE[] = " \t\r\n\v";
// Caracteres especiales
static const char SYMBOLS[] = "<|>&;()";

/******************************************************************************
 * Funciones auxiliares
 ******************************************************************************/

// Imprime el mensaje
void info(const char *fmt, ...)
{
    va_list arg;

    fprintf(stdout, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stdout, fmt, arg);
    va_end(arg);
}

// Imprime el mensaje de error
void error(const char *fmt, ...)
{
    va_list arg;

    fprintf(stderr, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stderr, fmt, arg);
    va_end(arg);
}

// Imprime el mensaje de error y aborta la ejecución
void panic(const char *fmt, ...)
{
    va_list arg;

    fprintf(stderr, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stderr, fmt, arg);
    va_end(arg);

    exit(EXIT_FAILURE);
}

// `fork()` que muestra un mensaje de error si no se puede crear el hijo
int fork_or_panic(const char *s)
{
    int pid;

    pid = fork();
    if (pid == -1)
        panic("%s failed: errno %d (%s)", s, errno, strerror(errno));
    return pid;
}

/******************************************************************************
 * Estructuras de datos `cmd`
 ******************************************************************************/

// Las estructuras `cmd` se utilizan para almacenar información que servirá a
// simplesh para ejecutar líneas de órdenes con redirecciones, tuberías, listas
// de comandos y tareas en segundo plano. El formato es el siguiente:

//     |----------+--------------+--------------|
//     | (1 byte) | ...          | ...          |
//     |----------+--------------+--------------|
//     | type     | otros campos | otros campos |
//     |----------+--------------+--------------|

// Nótese cómo las estructuras `cmd` comparten el primer campo `type` para
// identificar su tipo. A partir de él se obtiene un tipo derivado a través de
// *casting* forzado de tipo. Se consigue así polimorfismo básico en C.

// Valores del campo `type` de las estructuras de datos `cmd`
enum cmd_type
{
    EXEC = 1,
    REDR = 2,
    PIPE = 3,
    LIST = 4,
    BACK = 5,
    SUBS = 6,
    INV = 7
};

struct cmd
{
    enum cmd_type type;
};

// Comando con sus parámetros
struct execcmd
{
    enum cmd_type type;
    char *argv[MAX_ARGS];
    char *eargv[MAX_ARGS];
    int argc;
};

// Comando con redirección
struct redrcmd
{
    enum cmd_type type;
    struct cmd *cmd;
    char *file;
    char *efile;
    int flags;
    mode_t mode;
    int fd;
};

// Comandos con tubería
struct pipecmd
{
    enum cmd_type type;
    struct cmd *left;
    struct cmd *right;
};

// Lista de órdenes
struct listcmd
{
    enum cmd_type type;
    struct cmd *left;
    struct cmd *right;
};

// Tarea en segundo plano (background) con `&`
struct backcmd
{
    enum cmd_type type;
    struct cmd *cmd;
};

// Subshell
struct subscmd
{
    enum cmd_type type;
    struct cmd *cmd;
};

/******************************************************************************
 * Funciones para construir las estructuras de datos `cmd`
 ******************************************************************************/

// Construye una estructura `cmd` de tipo `EXEC`
struct cmd *execcmd(void)
{
    struct execcmd *cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("execcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = EXEC;

    return (struct cmd *)cmd;
}

// Construye una estructura `cmd` de tipo `REDR`
struct cmd *redrcmd(struct cmd *subcmd,
                    char *file, char *efile,
                    int flags, mode_t mode, int fd)
{
    struct redrcmd *cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("redrcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = REDR;
    cmd->cmd = subcmd;
    cmd->file = file;
    cmd->efile = efile;
    cmd->flags = flags;
    cmd->mode = mode;
    cmd->fd = fd;

    return (struct cmd *)cmd;
}

// Construye una estructura `cmd` de tipo `PIPE`
struct cmd *pipecmd(struct cmd *left, struct cmd *right)
{
    struct pipecmd *cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("pipecmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = PIPE;
    cmd->left = left;
    cmd->right = right;

    return (struct cmd *)cmd;
}

// Construye una estructura `cmd` de tipo `LIST`
struct cmd *listcmd(struct cmd *left, struct cmd *right)
{
    struct listcmd *cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("listcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = LIST;
    cmd->left = left;
    cmd->right = right;

    return (struct cmd *)cmd;
}

// Construye una estructura `cmd` de tipo `BACK`
struct cmd *backcmd(struct cmd *subcmd)
{
    struct backcmd *cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("backcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = BACK;
    cmd->cmd = subcmd;

    return (struct cmd *)cmd;
}

// Construye una estructura `cmd` de tipo `SUB`
struct cmd *subscmd(struct cmd *subcmd)
{
    struct subscmd *cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("subscmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = SUBS;
    cmd->cmd = subcmd;

    return (struct cmd *)cmd;
}

/******************************************************************************
 * Funciones para realizar el análisis sintáctico de la línea de órdenes
 ******************************************************************************/

// `get_token` recibe un puntero al principio de una cadena (`start_of_str`),
// otro puntero al final de esa cadena (`end_of_str`) y, opcionalmente, dos
// punteros para guardar el principio y el final del token, respectivamente.
//
// `get_token` devuelve un *token* de la cadena de entrada.

int get_token(char **start_of_str, char const *end_of_str,
              char **start_of_token, char **end_of_token)
{
    char *s;
    int ret;

    // Salta los espacios en blanco
    s = *start_of_str;
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;

    // `start_of_token` apunta al principio del argumento (si no es NULL)
    if (start_of_token)
        *start_of_token = s;

    ret = *s;
    switch (*s)
    {
    case 0:
        break;
    case '|':
    case '(':
    case ')':
    case ';':
    case '&':
    case '<':
        s++;
        break;
    case '>':
        s++;
        if (*s == '>')
        {
            ret = '+';
            s++;
        }
        break;

    default:

        // El caso por defecto (cuando no hay caracteres especiales) es el
        // de un argumento de un comando. `get_token` devuelve el valor
        // `'a'`, `start_of_token` apunta al argumento (si no es `NULL`),
        // `end_of_token` apunta al final del argumento (si no es `NULL`) y
        // `start_of_str` avanza hasta que salta todos los espacios
        // *después* del argumento. Por ejemplo:
        //
        //     |-----------+---+---+---+---+---+---+---+---+---+-----------|
        //     | (espacio) | a | r | g | u | m | e | n | t | o | (espacio)
        //     |
        //     |-----------+---+---+---+---+---+---+---+---+---+-----------|
        //                   ^                                   ^
        //            start_o|f_token                       end_o|f_token

        ret = 'a';
        while (s < end_of_str &&
               !strchr(WHITESPACE, *s) &&
               !strchr(SYMBOLS, *s))
            s++;
        break;
    }

    // `end_of_token` apunta al final del argumento (si no es `NULL`)
    if (end_of_token)
        *end_of_token = s;

    // Salta los espacios en blanco
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;

    // Actualiza `start_of_str`
    *start_of_str = s;

    return ret;
}

// `peek` recibe un puntero al principio de una cadena (`start_of_str`), otro
// puntero al final de esa cadena (`end_of_str`) y un conjunto de caracteres
// (`delimiter`).
//
// El primer puntero pasado como parámero (`start_of_str`) avanza hasta el
// primer carácter que no está en el conjunto de caracteres `WHITESPACE`.
//
// `peek` devuelve un valor distinto de `NULL` si encuentra alguno de los
// caracteres en `delimiter` justo después de los caracteres en `WHITESPACE`.

int peek(char **start_of_str, char const *end_of_str, char *delimiter)
{
    char *s;

    s = *start_of_str;
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;
    *start_of_str = s;

    return *s && strchr(delimiter, *s);
}

// Definiciones adelantadas de funciones
struct cmd *parse_line(char **, char *);
struct cmd *parse_pipe(char **, char *);
struct cmd *parse_exec(char **, char *);
struct cmd *parse_subs(char **, char *);
struct cmd *parse_redr(struct cmd *, char **, char *);
struct cmd *null_terminate(struct cmd *);

// `parse_cmd` realiza el *análisis sintáctico* de la línea de órdenes
// introducida por el usuario.
//
// `parse_cmd` utiliza `parse_line` para obtener una estructura `cmd`.

struct cmd *parse_cmd(char *start_of_str)
{
    char *end_of_str;
    struct cmd *cmd;

    DPRINTF(DBG_TRACE, "STR\n");

    end_of_str = start_of_str + strlen(start_of_str);

    cmd = parse_line(&start_of_str, end_of_str);

    // Comprueba que se ha alcanzado el final de la línea de órdenes
    peek(&start_of_str, end_of_str, "");
    if (start_of_str != end_of_str)
        error("%s: error sintáctico: %s\n", __func__);

    DPRINTF(DBG_TRACE, "END\n");

    return cmd;
}

// `parse_line` realiza el análisis sintáctico de la línea de órdenes
// introducida por el usuario.
//
// `parse_line` comprueba en primer lugar si la línea contiene alguna tubería.
// Para ello `parse_line` llama a `parse_pipe` que a su vez verifica si hay
// bloques de órdenes y/o redirecciones.  A continuación, `parse_line`
// comprueba si la ejecución de la línea se realiza en segundo plano (con `&`)
// o si la línea de órdenes contiene una lista de órdenes (con `;`).

struct cmd *parse_line(char **start_of_str, char *end_of_str)
{
    struct cmd *cmd;
    int delimiter;

    cmd = parse_pipe(start_of_str, end_of_str);

    while (peek(start_of_str, end_of_str, "&"))
    {
        // Consume el delimitador de tarea en segundo plano
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '&');

        // Construye el `cmd` para la tarea en segundo plano
        cmd = backcmd(cmd);
    }

    if (peek(start_of_str, end_of_str, ";"))
    {
        if (cmd->type == EXEC && ((struct execcmd *)cmd)->argv[0] == 0)
            error("%s: error sintáctico: no se encontró comando\n", __func__);

        // Consume el delimitador de lista de órdenes
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == ';');

        // Construye el `cmd` para la lista
        cmd = listcmd(cmd, parse_line(start_of_str, end_of_str));
    }

    return cmd;
}

// `parse_pipe` realiza el análisis sintáctico de una tubería de manera
// recursiva si encuentra el delimitador de tuberías '|'.
//
// `parse_pipe` llama a `parse_exec` y `parse_pipe` de manera recursiva para
// realizar el análisis sintáctico de todos los componentes de la tubería.

struct cmd *parse_pipe(char **start_of_str, char *end_of_str)
{
    struct cmd *cmd;
    int delimiter;

    cmd = parse_exec(start_of_str, end_of_str);

    if (peek(start_of_str, end_of_str, "|"))
    {
        if (cmd->type == EXEC && ((struct execcmd *)cmd)->argv[0] == 0)
            error("%s: error sintáctico: no se encontró comando\n", __func__);

        // Consume el delimitador de tubería
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '|');

        // Construye el `cmd` para la tubería
        cmd = pipecmd(cmd, parse_pipe(start_of_str, end_of_str));
    }

    return cmd;
}

// `parse_exec` realiza el análisis sintáctico de un comando a no ser que la
// expresión comience por un paréntesis, en cuyo caso se llama a `parse_subs`.
//
// `parse_exec` reconoce las redirecciones antes y después del comando.

struct cmd *parse_exec(char **start_of_str, char *end_of_str)
{
    char *start_of_token;
    char *end_of_token;
    int token, argc;
    struct execcmd *cmd;
    struct cmd *ret;

    // ¿Inicio de un bloque?
    if (peek(start_of_str, end_of_str, "("))
        return parse_subs(start_of_str, end_of_str);

    // Si no, lo primero que hay en una línea de órdenes es un comando

    // Construye el `cmd` para el comando
    ret = execcmd();
    cmd = (struct execcmd *)ret;

    // ¿Redirecciones antes del comando?
    ret = parse_redr(ret, start_of_str, end_of_str);

    // Bucle para separar los argumentos de las posibles redirecciones
    argc = 0;
    while (!peek(start_of_str, end_of_str, "|)&;"))
    {
        if ((token = get_token(start_of_str, end_of_str,
                               &start_of_token, &end_of_token)) == 0)
            break;

        // El siguiente token debe ser un argumento porque el bucle
        // para en los delimitadores
        if (token != 'a')
            error("%s: error sintáctico: se esperaba un argumento\n", __func__);

        // Almacena el siguiente argumento reconocido. El primero es
        // el comando
        cmd->argv[argc] = start_of_token;
        cmd->eargv[argc] = end_of_token;
        cmd->argc = ++argc;
        if (argc >= MAX_ARGS)
            panic("%s: demasiados argumentos\n", __func__);

        // ¿Redirecciones después del comando?
        ret = parse_redr(ret, start_of_str, end_of_str);
    }

    // El comando no tiene más parámetros
    cmd->argv[argc] = 0;
    cmd->eargv[argc] = 0;

    return ret;
}

// `parse_subs` realiza el análisis sintáctico de un bloque de órdenes
// delimitadas por paréntesis o `subshell` llamando a `parse_line`.
//
// `parse_subs` reconoce las redirecciones después del bloque de órdenes.

struct cmd *parse_subs(char **start_of_str, char *end_of_str)
{
    int delimiter;
    struct cmd *cmd;
    struct cmd *scmd;

    // Consume el paréntesis de apertura
    if (!peek(start_of_str, end_of_str, "("))
        error("%s: error sintáctico: se esperaba '('", __func__);
    delimiter = get_token(start_of_str, end_of_str, 0, 0);
    assert(delimiter == '(');

    // Realiza el análisis sintáctico hasta el paréntesis de cierre
    scmd = parse_line(start_of_str, end_of_str);

    // Construye el `cmd` para el bloque de órdenes
    cmd = subscmd(scmd);

    // Consume el paréntesis de cierre
    if (!peek(start_of_str, end_of_str, ")"))
        error("%s: error sintáctico: se esperaba ')'", __func__);
    delimiter = get_token(start_of_str, end_of_str, 0, 0);
    assert(delimiter == ')');

    // ¿Redirecciones después del bloque de órdenes?
    cmd = parse_redr(cmd, start_of_str, end_of_str);

    return cmd;
}

// `parse_redr` realiza el análisis sintáctico de órdenes con
// redirecciones si encuentra alguno de los delimitadores de
// redirección ('<' o '>').

struct cmd *parse_redr(struct cmd *cmd, char **start_of_str, char *end_of_str)
{
    int delimiter;
    char *start_of_token;
    char *end_of_token;

    // Si lo siguiente que hay a continuación es delimitador de
    // redirección...
    while (peek(start_of_str, end_of_str, "<>"))
    {
        // Consume el delimitador de redirección
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '<' || delimiter == '>' || delimiter == '+');

        // El siguiente token tiene que ser el nombre del fichero de la
        // redirección entre `start_of_token` y `end_of_token`.
        if ('a' != get_token(start_of_str, end_of_str, &start_of_token, &end_of_token))
            error("%s: error sintáctico: se esperaba un fichero", __func__);

        // Construye el `cmd` para la redirección
        switch (delimiter)
        {
        case '<':
            cmd = redrcmd(cmd, start_of_token, end_of_token, O_RDONLY, 0, STDIN_FILENO);
            break;
        case '>':
            cmd = redrcmd(cmd, start_of_token, end_of_token, O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU, STDOUT_FILENO);
            break;
        case '+': // >>
            cmd = redrcmd(cmd, start_of_token, end_of_token, O_WRONLY | O_CREAT | O_APPEND, S_IRWXU, STDOUT_FILENO);
            break;
        }
    }

    return cmd;
}

// Termina en NULL todas las cadenas de las estructuras `cmd`
struct cmd *null_terminate(struct cmd *cmd)
{
    struct execcmd *ecmd;
    struct redrcmd *rcmd;
    struct pipecmd *pcmd;
    struct listcmd *lcmd;
    struct backcmd *bcmd;
    struct subscmd *scmd;
    int i;

    if (cmd == 0)
        return 0;

    switch (cmd->type)
    {
    case EXEC:
        ecmd = (struct execcmd *)cmd;
        for (i = 0; ecmd->argv[i]; i++)
            *ecmd->eargv[i] = 0;
        break;

    case REDR:
        rcmd = (struct redrcmd *)cmd;
        null_terminate(rcmd->cmd);
        *rcmd->efile = 0;
        break;

    case PIPE:
        pcmd = (struct pipecmd *)cmd;
        null_terminate(pcmd->left);
        null_terminate(pcmd->right);
        break;

    case LIST:
        lcmd = (struct listcmd *)cmd;
        null_terminate(lcmd->left);
        null_terminate(lcmd->right);
        break;

    case BACK:
        bcmd = (struct backcmd *)cmd;
        null_terminate(bcmd->cmd);
        break;

    case SUBS:
        scmd = (struct subscmd *)cmd;
        null_terminate(scmd->cmd);
        break;

    case INV:
    default:
        panic("%s: estructura `cmd` desconocida\n", __func__);
    }

    return cmd;
}

/******************************************************************************
 * Funciones para la ejecución de la línea de órdenes
 ******************************************************************************/

void exec_cmd(struct execcmd *ecmd)
{
    assert(ecmd->type == EXEC);

    if (ecmd->argv[0] == NULL)
        exit(EXIT_SUCCESS);

    execvp(ecmd->argv[0], ecmd->argv);

    panic("no se encontró el comando '%s'\n", ecmd->argv[0]);
}

/* Esta funcion devuelve 1 si es un cwd, 2 si es cd y 3 si es exit
    y 0 si no es interno*/
int is_interno(char *cmd)
{
    char *cwd = "cwd";
    char *cd = "cd";
    char *exit = "exit";
    char *psplit = "psplit";
    if (cmd == 0)
        return 0;
    if (strcmp(cmd, cwd) == 0)
    {
        return 1;
    }
    else if (strcmp(cmd, cd) == 0)
    {
        return 2;
    }
    else if (strcmp(cmd, exit) == 0)
    {
        return 3;
    }
    else if (strcmp(cmd, psplit) == 0)
    {
        return 4;
    }

    return 0; //No interno.
}
//RUN CWD

void run_cwd()
{
    char diract[PATH_MAX];
    getcwd(diract, PATH_MAX);
    printf("cwd: %s\n", diract);
}

void run_exit()
{
    exit(EXIT_SUCCESS);
}

//Comprobar el numero de argumentos y sacar ese error.
void run_cd(struct execcmd *cmd)
{
    char diract[PATH_MAX];
    getcwd(diract, PATH_MAX);
    if (cmd->argc == 1)
    {
        setenv("OLDPWD", diract, 1);
        char *home = getenv("HOME");
        chdir(home);
    }
    else if (cmd->argc > 2)
    {
        fprintf(stderr, "run_cd: Demasiados argumentos\n");
    }
    else if (strcmp(cmd->argv[1], "-") == 0)
    {
        if (getenv("OLDPWD") == NULL)
            fprintf(stderr, "run_cd: Variable OLDPWD no definida\n");
        chdir(getenv("OLDPWD"));
    }
    else
    {

        if (chdir(cmd->argv[1]) == 0)
        {
            //modificar variable oldpwd

            setenv("OLDPWD", diract, 1);
        }
        else
        {
            //ERROR
            fprintf(stderr, "run_cd: No existe el directorio '%s'\n", cmd->argv[1]);
        }
    }
}

/**
 * Dado un buffer de caracteres devuelve la posicion de donde termina la primera linea ,antes de encontrar un \n, 
*/

void run_psplit(struct execcmd *cmd)
{
    int opt, nlines, nbytes, bsize, error, flag, procs;
    nlines = 0;
    nbytes = 1024;
    bsize = 1024;
    error = 0;
    flag = 0;
    procs = 0;
    optind = 1;
    while (flag == 0 && (opt = getopt(cmd->argc, cmd->argv, "hb:l:s:p:")) != -1)
    {
        switch (opt)
        {
        case 'h':
            fprintf(stdout, "Uso: psplit [-l NLINES] [-b NBYTES] [-s BSIZE] [-p PROCS] [FILE1] [FILE2]...\n      Opciones:\n      -l NLINES Número máximo de líneas por fichero.\n      -b NBYTES Número máximo de bytes por fichero.\n      -s BSIZE Tamaño en bytes de los bloques leídos de [ FILEn ] o stdin.\n      -p PROCS Número máximo de procesos simultáneos.\n   -h        Ayuda\n\n");
            return;
        case 'b':
            if (error != 0)
            {
                flag = 1;
            }
            else
            {
                error = 1;
                nbytes = atoi(optarg);
            }
            break;
        case 'l':
            if (error != 0)
            {
                flag = 1;
            }
            else
            {
                error = 1;
                nlines = atoi(optarg);
            }
            break;
        case 's':
            bsize = atoi(optarg);
            if (bsize < 1 || bsize > BSIZE_MAX)
                flag = 2;
            break;
        case 'p':
            procs = atoi(optarg);
            if (procs < 1)
                flag = 3;
            break;
        default:
            fprintf(stderr, "Usage: %s [-f] [-n NUM]\n", cmd->argv[0]);
            exit(EXIT_FAILURE);
        }
    }
    char *buff[cmd->argc - optind];
    int numFicheros = 0;
    for (int i = optind; i < cmd->argc; i++)
    {
        buff[numFicheros] = cmd->argv[i];
        numFicheros++;
        //printf("%s\n",cmd->argv[i]);
    }

    switch (flag)
    {
    case 1:
        //OPCIONES L Y B
        fprintf(stderr, "psplit: Opciones incompatibles\n");
        return;
    case 2:
        //TAMAÑO BSIZE
        fprintf(stderr, "psplit: Opción -s no válida\n");
        return;
    case 3:
        //TAMAÑO DE PROCS
        fprintf(stderr, "psplit: Opción -p no válida\n");
        return;
    }

    int fd;
    char blect[bsize]; //bytes leidos

    memset(blect, 0, bsize);
    if (numFicheros == 0)
    { //CASO EN EL QUE TENGAMOS QUE LEER DE LA ENTRADA ESTÁNDAR

        if (nlines != 0)
        {                        //-L
            int bytesleidos = 0; //bytes comprobados, \n o no
            int lineas = 0;      //lineas ya en fichero
            int r = 0;           //bytes leidos del fichero inicial
            int w = 0;           //bytes escritos ya en fichero, desplamiento en blect
            int f_aux = 0;
            int fdaux = 0;
            char i_Str[strlen("stdin")];
            int i = 0;
            int cerrar_fichero = 1;
            //o lectura de binarios, o bloquear tee?
            while ((r = read(0, blect, bsize)) != 0)
            {
                w = 0;
                bytesleidos = 0;
                while (bytesleidos != r) //mientras los bytes leidos no sean el tamaño de la lectura
                {
                    if (f_aux == 0)
                    {
                        sprintf(i_Str, "stdin%d", i);
                        fdaux = open(i_Str, O_CREAT | O_RDWR | O_TRUNC, S_IRWXU);
                        i++;
                    }
                    while (lineas != nlines && bytesleidos != r)
                    {
                        if (blect[bytesleidos] == '\n')
                            lineas++;
                        bytesleidos++;
                    }
                    if (lineas == nlines)
                    {
                        w += write(fdaux, blect + w, bytesleidos - w);
                        f_aux = 0;
                        lineas = 0;
                        close(fdaux);
                        //   fsync(fdaux);

                        cerrar_fichero = 0;
                    }
                    else if (bytesleidos == r)
                    {
                        write(fdaux, blect + w, r - w);
                        f_aux = 1;
                        cerrar_fichero = 1;
                    }
                }
                memset(blect, 0, bsize);
            }
            if (cerrar_fichero == 1)
            {
                close(fdaux);
                //fsync(fdaux);
            }
        }
        else
        { //-B

            int i = 0; //indice para los archivos creados
            int r = 0; //bytes leidos
            int fdaux;
            int f_aux = 0;
            int wb = 0;
            int wf = 0;
            char i_Str[strlen("stdin")];
            int cerrar_fichero = 1;
            //bytes leidos en ese read.
            while ((r = read(0, blect, bsize)) != 0)
            {

                wb = 0;

                while (r != 0)
                {
                    if (f_aux == 0)
                    {
                        sprintf(i_Str, "stdin%d", i);
                        fdaux = open(i_Str, O_CREAT | O_RDWR | O_TRUNC, S_IRWXU);
                        i++;
                    }
                    /*if (r >= nbytes)
                    {
                        wb += write(fdaux, blect + wb, nbytes);
                        r -= nbytes;
                        f_aux = 0;
                        close(fdaux);
                        fsync(fdaux);
                        cerrar_fichero = 0;
                    }
                    else
                    {*/
                    if (nbytes - wf <= r)
                    {
                        wb += write(fdaux, blect + wb, nbytes - wf);
                        f_aux = 0;
                        r -= nbytes - wf;
                        close(fdaux);
                        fsync(fdaux);
                        wf = 0;
                        cerrar_fichero = 0;
                    }
                    else
                    {
                        wf += write(fdaux, blect + wb, r);
                        f_aux = fdaux;
                        r = 0;
                        cerrar_fichero = 1;
                    }
                    //}
                }
                //  memset(blect, 0, bsize);
            }
            //Cerrar cuando se ha quedado a medias un fichero
            if (cerrar_fichero == 1)
            {
                close(fdaux);
                fsync(fdaux);
            }
        }
    }
    else
    {

        if (nlines != 0) //OPCIÓN -L EN EL CASO QUE HAYAN FICHEROS
        {
            for (int j = 0; j < numFicheros; j++)
            {
                if ((fd = open(buff[j], O_RDONLY)) != -1)
                {
                    int bytesleidos = 0;
                    int lineas = 0;
                    int r;
                    int w;
                    int f_aux = 0;
                    int fdaux;
                    char i_Str[20];
                    int i = 0;
                    int cerrar_fichero = 1;
                    while ((r = read(fd, blect, bsize)) != 0)
                    {
                        printf("%s-", blect);
                        w = 0;
                        bytesleidos = 0;
                        while (bytesleidos != r)
                        {
                            if (f_aux == 0)
                            {
                                sprintf(i_Str, "%s%d", buff[j], i);
                                fdaux = open(i_Str, O_CREAT | O_RDWR | O_TRUNC, S_IRWXU);
                                i++;
                                f_aux = 1;
                                cerrar_fichero = 0;
                            }
                            while (lineas != nlines && bytesleidos != r)
                            {
                                if (blect[bytesleidos] == '\n')
                                    lineas++;
                                bytesleidos++;
                            }
                            if (lineas == nlines)
                            {
                                w += write(fdaux, blect + w, bytesleidos - w);
                                f_aux = 0;
                                lineas = 0;
                                close(fdaux);
                                fsync(fdaux);
                                cerrar_fichero = 0;
                            }
                            else if (bytesleidos == r)
                            {
                                write(fdaux, blect + w, r - w);
                                cerrar_fichero = 1;
                            }
                        }
                        memset(blect, 0, bsize);
                    }
                    close(fdaux);
                    fsync(fdaux);
                }
            }
        }
        else //OPCIÓN -B EN EL CASO QUE HAYAN FICHEROS
        {
            for (int j = 0; j < numFicheros; j++)
            {
                if ((fd = open(buff[j], O_RDONLY)) != -1)
                {
                    int i = 0; //indice para los archivos creados
                    int r = 0; //bytes leidos
                    int fdaux;
                    int f_aux = 0;
                    int wb = 0;
                    int wf = 0;
                    char i_Str[strlen(buff[j])];
                    //bytes leidos en ese read.
                    while ((r = read(fd, blect, bsize)) != 0) // lectura del fichero abierto anteriormente
                    {

                        wb = 0;
                        while (r != 0)
                        {
                            if (f_aux == 0)
                            {
                                sprintf(i_Str, "%s%d", buff[j], i);
                                wf = 0;
                                fdaux = open(i_Str, O_CREAT | O_RDWR | O_TRUNC, S_IRWXU);
                                i++;
                            }
                            /* if (r >= nbytes)
                            {
                                wb += write(fdaux, blect + wb, nbytes);
                                r -= nbytes;
                                f_aux = 0;
                                close(fdaux);
                                fsync(fdaux);
                            }
                            else
                            {*/
                            if (nbytes - wf < r)
                            {
                                wb += write(fdaux, blect + wb, nbytes - wf);
                                f_aux = 0;
                                r -= nbytes - wf;
                                close(fdaux);
                                fsync(fdaux);
                                wf = 0;
                            }
                            else
                            {
                                wf += write(fdaux, blect + wb, r);
                                f_aux = fdaux;
                                r = 0;
                            }
                            //}
                        }
                        memset(blect, 0, bsize);
                    }
                    close(fdaux);
                    fsync(fdaux);
                }
                else
                {
                    //Error no existe el archivo
                }
            }
        }
    }
}
//funcion que ejecuta el comando una vez comprueba si este es externo o interno
int exec_comp(struct execcmd *ecmd)

{
    //cmd puede no ser siempre un exec

    int numCmd = is_interno(ecmd->argv[0]);

    switch (numCmd)
    {
    case 0: //No interno
        if (fork_or_panic("fork EXEC") == 0)
            exec_cmd(ecmd);
        TRY(wait(NULL));
        break;
    case 1: //cwd
        run_cwd();
        break;
    case 2: //cd
        run_cd(ecmd);
        break;
    case 3: //exit

        free(ecmd);
        run_exit();
        break;
    case 4:
        run_psplit(ecmd);
        break;
    }
    return numCmd;
}

void run_cmd(struct cmd *cmd)
{
    struct execcmd *ecmd;
    struct redrcmd *rcmd;
    struct listcmd *lcmd;
    struct pipecmd *pcmd;
    struct backcmd *bcmd;
    struct subscmd *scmd;
    int p[2];
    int fd; //file descriptor

    DPRINTF(DBG_TRACE, "STR\n");

    if (cmd == 0)
        return;

    switch (cmd->type)
    {
    case EXEC:
        ecmd = (struct execcmd *)cmd;

        exec_comp(ecmd);

        break;

    case REDR:
        rcmd = (struct redrcmd *)cmd;

        if (is_interno(((struct execcmd *)rcmd->cmd)->argv[0]))
        {

            if ((fd = open(rcmd->file, rcmd->flags, rcmd->mode)) < 0)
            {
                perror("open");
                exit(EXIT_FAILURE);
            }
            p[1] = dup(1); //Necesitamos hacer una copia para no perderlo
            TRY(dup2(fd, STDOUT_FILENO));
            run_cmd(rcmd->cmd);
            TRY(dup2(p[1], STDOUT_FILENO));
        }
        else
        {

            if (fork_or_panic("fork REDR") == 0)
            {
                TRY(close(rcmd->fd));
                //amhadimos el tercer campo para poder actualizar, pus open tiene 3 parametros ahora
                if ((fd = open(rcmd->file, rcmd->flags, rcmd->mode)) < 0)
                {
                    perror("open");
                    exit(EXIT_FAILURE);
                }

                if (rcmd->cmd->type == EXEC)
                    exec_cmd((struct execcmd *)rcmd->cmd);
                else
                    run_cmd(rcmd->cmd);
                exit(EXIT_SUCCESS);
            }
            TRY(wait(NULL));
        }
        break;

    case LIST:
        lcmd = (struct listcmd *)cmd;
        run_cmd(lcmd->left);
        run_cmd(lcmd->right);
        break;

    case PIPE:
        pcmd = (struct pipecmd *)cmd;
        if (pipe(p) < 0)
        {
            perror("pipe");
            exit(EXIT_FAILURE);
        }

        // Ejecución del hijo de la izquierda
        if (fork_or_panic("fork PIPE left") == 0)
        {
            TRY(close(STDOUT_FILENO));
            TRY(dup(p[1]));
            TRY(close(p[0]));
            TRY(close(p[1]));
            //comprobar
            if (pcmd->left->type == EXEC && !is_interno(((struct execcmd *)pcmd->left)->argv[0]))
                exec_cmd((struct execcmd *)pcmd->left);
            else
                run_cmd(pcmd->left);
            exit(EXIT_SUCCESS);
        }

        // Ejecución del hijo de la derecha
        if (fork_or_panic("fork PIPE right") == 0)
        {
            TRY(close(STDIN_FILENO)); //cerrando entrada estandar
            TRY(dup(p[0]));
            TRY(close(p[0]));
            TRY(close(p[1]));
            if (pcmd->right->type == EXEC && !is_interno(((struct execcmd *)pcmd->right)->argv[0]))
                exec_cmd((struct execcmd *)pcmd->right);
            else
                run_cmd(pcmd->right);
            exit(EXIT_SUCCESS);
        }
        TRY(close(p[0]));
        TRY(close(p[1]));

        // Esperar a ambos hijos
        TRY(wait(NULL));
        TRY(wait(NULL));
        break;

    case BACK:
        bcmd = (struct backcmd *)cmd;

        if (fork_or_panic("fork BACK") == 0)
        {
            if (bcmd->cmd->type == EXEC && !is_interno(((struct execcmd *)bcmd->cmd)->argv[0]))
                exec_cmd((struct execcmd *)bcmd->cmd);
            else
                run_cmd(bcmd->cmd);
            exit(EXIT_SUCCESS);
        }
        break;

    case SUBS:
        scmd = (struct subscmd *)cmd;
        if (fork_or_panic("fork SUBS") == 0)
        {
            run_cmd(scmd->cmd);
            exit(EXIT_SUCCESS);
        }
        TRY(wait(NULL));
        break;

    case INV:
    default:
        panic("%s: estructura `cmd` desconocida\n", __func__);
    }

    DPRINTF(DBG_TRACE, "END\n");
}

void print_cmd(struct cmd *cmd)
{
    struct execcmd *ecmd;
    struct redrcmd *rcmd;
    struct listcmd *lcmd;
    struct pipecmd *pcmd;
    struct backcmd *bcmd;
    struct subscmd *scmd;

    if (cmd == 0)
        return;

    switch (cmd->type)
    {
    case EXEC:
        ecmd = (struct execcmd *)cmd;
        if (ecmd->argv[0] != 0)
            printf("fork( exec( %s ) )", ecmd->argv[0]);
        break;

    case REDR:
        rcmd = (struct redrcmd *)cmd;
        printf("fork( ");
        if (rcmd->cmd->type == EXEC)
            printf("exec ( %s )", ((struct execcmd *)rcmd->cmd)->argv[0]);
        else
            print_cmd(rcmd->cmd);
        printf(" )");
        break;

    case LIST:
        lcmd = (struct listcmd *)cmd;
        print_cmd(lcmd->left);
        printf(" ; ");
        print_cmd(lcmd->right);
        break;

    case PIPE:
        pcmd = (struct pipecmd *)cmd;
        printf("fork( ");
        if (pcmd->left->type == EXEC)
            printf("exec ( %s )", ((struct execcmd *)pcmd->left)->argv[0]);
        else
            print_cmd(pcmd->left);
        printf(" ) => fork( ");
        if (pcmd->right->type == EXEC)
            printf("exec ( %s )", ((struct execcmd *)pcmd->right)->argv[0]);
        else
            print_cmd(pcmd->right);
        printf(" )");
        break;

    case BACK:
        bcmd = (struct backcmd *)cmd;
        printf("fork( ");
        if (bcmd->cmd->type == EXEC)
            printf("exec ( %s )", ((struct execcmd *)bcmd->cmd)->argv[0]);
        else
            print_cmd(bcmd->cmd);
        printf(" )");
        break;

    case SUBS:
        scmd = (struct subscmd *)cmd;
        printf("fork( ");
        print_cmd(scmd->cmd);
        printf(" )");
        break;

    case INV:
    default:
        panic("%s: estructura `cmd` desconocida\n", __func__);
    }
}

void free_cmd(struct cmd *cmd)
{
    struct execcmd *ecmd;
    struct redrcmd *rcmd;
    struct listcmd *lcmd;
    struct pipecmd *pcmd;
    struct backcmd *bcmd;
    struct subscmd *scmd;

    if (cmd == 0)
        return;

    switch (cmd->type)
    {
    case EXEC:
        break;

    case REDR:
        rcmd = (struct redrcmd *)cmd;
        free_cmd(rcmd->cmd);

        // free(rcmd->cmd);
        break;

    case LIST:
        lcmd = (struct listcmd *)cmd;

        free_cmd(lcmd->left);
        free_cmd(lcmd->right);
        //TODO
        // free(lcmd->right);
        //free(lcmd->left);
        break;

    case PIPE:
        pcmd = (struct pipecmd *)cmd;

        free_cmd(pcmd->left);
        free_cmd(pcmd->right);
        //TODO
        // free(pcmd->right);
        //free(pcmd->left);
        break;

    case BACK:
        bcmd = (struct backcmd *)cmd;

        free_cmd(bcmd->cmd);
        //TODO
        //free(bcmd->cmd);
        break;

    case SUBS:
        scmd = (struct subscmd *)cmd;

        free_cmd(scmd->cmd);

        //free(scmd->cmd);
        break;

    case INV:
    default:
        panic("%s: estructura `cmd` desconocida\n", __func__);
    }
    /* Están  apuntando a lo mismo, se liberaría dos veces en caso de dejar 
    ambos 'free()' pues apuntan a la misma direccion de memoria ()*/
    free(cmd);
}

/******************************************************************************
 * Lectura de la línea de órdenes con la biblioteca libreadline
 ******************************************************************************/

// `get_cmd` muestra un *prompt* y lee lo que el usuario escribe usando la
// biblioteca readline. Ésta permite mantener el historial, utilizar las flechas
// para acceder a las órdenes previas del historial, búsquedas de órdenes, etc.

char *get_cmd()
{
    char *buf;
    uid_t uid = getuid();
    struct passwd *passwd = getpwuid(uid);
    if (!passwd)
    {
        perror("getpwuid");
        exit(EXIT_FAILURE);
    }
    char *user = passwd->pw_name;

    char path[PATH_MAX];
    if (!getcwd(path, PATH_MAX))
    {
        perror("getcwuid");
    }
    char *dir = basename(path);
    char prompt[strlen(user) + strlen(dir) + 4];
    sprintf(prompt, "%s@%s> ", user, dir);

    // Lee la orden tecleada por el usuario
    buf = readline(prompt);

    // Si el usuario ha escrito una orden, almacenarla en la historia.
    if (buf)
        add_history(buf);

    return buf;
}

/******************************************************************************
 * Bucle principal de `simplesh`
 ******************************************************************************/

void help(char **argv)
{
    info("Usage: %s [-d N] [-h]\n\
         shell simplesh v%s\n\
         Options: \n\
         -d set debug level to N\n\
         -h help\n\n",
         argv[0], VERSION);
}

void parse_args(int argc, char **argv)
{
    int option;

    // Bucle de procesamiento de parámetros
    while ((option = getopt(argc, argv, "d:h")) != -1)
    {
        switch (option)
        {
        case 'd':
            g_dbg_level = atoi(optarg);
            break;
        case 'h':
        default:
            help(argv);
            exit(EXIT_SUCCESS);
            break;
        }
    }
}

int main(int argc, char **argv)
{
    char *buf;
    struct cmd *cmd;
    unsetenv("OLDPWD"); //Borramos la anterior
    parse_args(argc, argv);

    DPRINTF(DBG_TRACE, "STR\n");

    // Bucle de lectura y ejecución de órdenes
    while ((buf = get_cmd()) != NULL)
    {
        // Realiza el análisis sintáctico de la línea de órdenes
        cmd = parse_cmd(buf);

        // Termina en `NULL` todas las cadenas de las estructuras `cmd`
        null_terminate(cmd);

        DBLOCK(DBG_CMD, {
            info("%s:%d:%s: print_cmd: ",
                 __FILE__, __LINE__, __func__);
            print_cmd(cmd); printf("\n"); fflush(NULL); });

        // Ejecuta la línea de órdenes
        run_cmd(cmd);

        // Libera la memoria de las estructuras `cmd`
        free_cmd(cmd);

        // Libera la memoria de la línea de órdenes
        free(buf);
    }

    DPRINTF(DBG_TRACE, "END\n");

    return 0;
}