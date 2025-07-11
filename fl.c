#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <regex.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "flag/flag.h"
#include "frog/frog.h"

#define LOG_FILE ".local/state/fl/fl.log" /* Start at HOME */
#define UNDO_BACKUP_DIR "/tmp/fl-backup"

/* Colors for specific entry types. "" is set to default */
static const char *COLORS[] = {
        [DT_BLK] = "",       /* This is a block device. */
        [DT_CHR] = "",       /* This is a character device. */
        [DT_DIR] = "\e[34m", /* This is a directory. */
        [DT_FIFO] = "",      /* This is a named pipe (FIFO). */
        [DT_LNK] = "\e[36m", /* This is a symbolic link. */
        [DT_REG] = "",       /* This is a regular file. */
        [DT_SOCK] = "",      /* This is a UNIX domain socket. */
        [DT_UNKNOWN] = "",   /* The file type could not be determined. */
};

struct extend_dirent {
        struct dirent dirent;
        char path[256];
};

typedef DA(struct extend_dirent) dirent_da;

/* Entries array */
dirent_da dir_arr = { 0 };
dirent_da deleted_dir_arr = { 0 };

struct winsize wsize;
struct termios origin_termios;
int selected_row = 0;
int woffset = 0;
char pattern[1024] = { 0 };
int stdout_fileno;

/* Program options */
#ifdef EXTERNAL
int open_as_external = 1;
#else
int open_as_external = 0;
#endif

int do_not_delete = 0;


char *
__strconcat(const char *s1, ...)
{
        va_list ap;
        char *result;
        char *current;
        int size = 0;
        /* Count total size, to avoid realloc */
        va_start(ap, s1);
        while ((current = va_arg(ap, char *)))
                size += strlen(current);
        /* alloc total size + null termination */
        va_end(ap);
        result = malloc(size + 1);
        assert(result);
        /* Concat function arguments */
        va_start(ap, s1);
        strcpy(result, s1);
        while ((current = va_arg(ap, char *)))
                strcat(result, current);
        va_end(ap);
        return result;
}
#define strconcat(s1, ...) __strconcat(s1, ##__VA_ARGS__, NULL)

char *
__staticstrconcat(char *buf, int size, ...)
{
        va_list ap;
        char *current;
        va_start(ap, size);
        /* Concat function arguments */
        buf[0] = 0;
        while ((current = va_arg(ap, char *)))
                strncat(buf, current, size);
        va_end(ap);
        return buf;
}
#define staticstrconcat(buf, size, ...) \
        __staticstrconcat(buf, size, ##__VA_ARGS__, NULL)

void
enable_raw_mode()
{
        static struct termios raw_opts;
        static int raw_mode_init = 0;
        if (raw_mode_init == 0) {
                tcgetattr(STDIN_FILENO, &origin_termios);
                raw_opts = origin_termios;
                cfmakeraw(&raw_opts);
                raw_opts.c_oflag |= (OPOST | ONLCR); // '\n' -> '\r\n'
                raw_mode_init = 1;
        }
        tcsetattr(STDIN_FILENO, TCSANOW, &raw_opts);
}

void
disable_raw_mode()
{
        tcsetattr(STDIN_FILENO, TCSANOW, &origin_termios);
}

enum {
        CUSTOM_MODE_SET,
        CUSTOM_MODE_UNSET,
} custom_mode_status = CUSTOM_MODE_UNSET;

// #define DISABLE "\e[?47l"
// #define ENABLE "\e[?47h"
#define DISABLE "\e[?1049l"
#define ENABLE "\e[?1049h"
// #define DISABLE ""
// #define ENABLE ""

/* If this is defined as a function, the alternative buffer does not work.
 * So, it is defined as a macro :/ */
#define disable_custom_mode()                           \
        if (custom_mode_status == CUSTOM_MODE_SET) {    \
                dprintf(stdout_fileno, DISABLE);        \
                dprintf(stdout_fileno, "\e[?25h");      \
                disable_raw_mode();                     \
                custom_mode_status = CUSTOM_MODE_UNSET; \
        }

/* Like disable_custom_mode but does not disable alternative buffer */
#define soft_disable_custom_mode()                      \
        if (custom_mode_status == CUSTOM_MODE_SET) {    \
                dprintf(stdout_fileno, "\e[?25h");      \
                disable_raw_mode();                     \
                custom_mode_status = CUSTOM_MODE_UNSET; \
        }

#define enable_custom_mode()                           \
        if (custom_mode_status == CUSTOM_MODE_UNSET) { \
                enable_raw_mode();                     \
                dprintf(stdout_fileno, "\e[?25l");     \
                dprintf(stdout_fileno, ENABLE);        \
                dprintf(stdout_fileno, "\e[2J\e[H");   \
                custom_mode_status = CUSTOM_MODE_SET;  \
        }

/* Coments in the code above are written before the actual code to be able to
 * use it inside the macro, don't judge me, please. */

int create_filename_path_if_not_exists(const char *path);

void
report(const char *restrict format, ...)
{
        char buffer[1024 * 1024];
        int fd_in, fd_out;
        char backup_filename[1024];
        ssize_t n;
        static char *home = NULL;

        if (home == NULL) {
                home = getenv("HOME");
                if (home)
                        home = strdup(home);
                else
                        printf("Can not get env `HOME`");
        }

        staticstrconcat(backup_filename, 1024, home ?: ".", "/", LOG_FILE);
        if (create_filename_path_if_not_exists(backup_filename)) {
                printf("Can not create path for file: `%s`", backup_filename);
                return;
        }

        FILE *file = fopen(backup_filename, "a");
        va_list ap;
        va_start(ap, format);
        vfprintf(file, format, ap);
        fprintf(file, "\n");
        fclose(file);
        va_end(ap);
}

/* f must be string literal */
#define error(f, ...) report(f ": %s", ##__VA_ARGS__, strerror(errno));

int
sort_cmp(const void *_a, const void *_b)
{
        struct extend_dirent *a = (struct extend_dirent *) _a;
        struct extend_dirent *b = (struct extend_dirent *) _b;
        char fullpath_a[1024], fullpath_b[1024];
        return strcmp(
        staticstrconcat(fullpath_a, 1024, a->path, "/", a->dirent.d_name),
        staticstrconcat(fullpath_b, 1024, b->path, "/", b->dirent.d_name));
}

void
sort()
{
        qsort(dir_arr.data, dir_arr.size, sizeof *dir_arr.data, sort_cmp);
}

int
endwith(const char *str, const char *sufix)
{
        return strcmp(str + strlen(str) - strlen(sufix), sufix) == 0;
}

void
edit_file(const char *path, const char *subpath)
{
        static char *editor = NULL;
        int child;
        char buf[1024];
        char *p = subpath ? staticstrconcat(buf, 1024, subpath, "/", path) :
                            strncpy(buf, path, 1024);

        if (!p) {
                error("Critical error: can not alloc memory");
                abort();
        }

        /* Todo: filter by extension, as some files should be opened allways
         * externally. */
        if (open_as_external || endwith(p, ".pdf")) {
                switch (fork()) {
                case -1:
                        error("Fork failed");
                        return;
                case 0:
                        setsid();
                        execvp("xdg-open",
                               (char *const[]) { "xdg-open", p, NULL });
                        error("Execv failed");
                        report("  at: execv(\"xdg-open\", (char *const[]) "
                               "{ \"xdg-open\", %s, NULL });",
                               p);
                        abort();
                default:
                        return;
                }
        }

        disable_custom_mode();

        switch (child = fork()) {
        case -1:
                error("Fork failed");
                break;
        case 0:
                if (!editor) editor = getenv("EDITOR");
                if (!editor) {
                        error("Can not find env `EDITOR`");
                        /* If it can not find editor, open externally */
                        open_as_external = 1;
                        edit_file(path, subpath);
                        break;
                }
                execvp(editor, (char *const[]) { editor, p, NULL });
                error("Execv failed");
                report("  at: execv(%s, (char *const[]) { %s, %s, NULL });",
                       editor, p, editor);
                abort();

        default:
                waitpid(child, NULL, 0);
                break;
        }

        enable_custom_mode();
}

/* Add path from subpath path, at list index at. subpath can be null if using
 * current dir, and at can be -1 to append it. */
void
add_subfolder(const char *path, const char *subpath, int at)
{
        struct dirent *entry;
        struct extend_dirent edirent;
        char *p;
        DIR *dir;

        p = subpath ? strconcat(subpath, "/", path) :
                      strdup(path);

        if (!(dir = opendir(p))) {
                error("Can not open dir: %s", path);
                return;
        }
        if (at < 0 || at > dir_arr.size) at = dir_arr.size;

        while ((entry = readdir(dir))) {
                if (!strcmp(entry->d_name, ".")) continue; // do not add "^./"
                edirent.dirent = *entry;
                strcpy(edirent.path, p);
                da_insert(&dir_arr, edirent, at);
        }

        free(p);
        sort();
}

void
remove_subfolder(const char *path, const char *subpath)
{
        int i;
        char *p;

        p = subpath ? strconcat(subpath, "/", path) :
                      strdup(path);

        for (i = 0; i < dir_arr.size; i++) {
                if (!memcmp(p, dir_arr.data[i].path, strlen(p))) {
                        da_remove(&dir_arr, i);
                        --i;
                }
        }
        free(p);
}

int
is_folder_open(const char *path, const char *subpath)
{
        char *p;
        int i;

        p = subpath ? strconcat(subpath, "/", path) :
                      strdup(path);
        for (i = 0; i < dir_arr.size; i++) {
                if (!memcmp(p, dir_arr.data[i].path, strlen(p))) {
                        return 1;
                }
        }
        free(p);
        return 0;
}

void
print_file(struct extend_dirent entry)
{
        char *path = entry.path;
        if (!memcmp(path, "./", 2)) path += 2; // remove the ugly ./ prefix
        if (strcmp(path, ".")) dprintf(stdout_fileno, "%s/", path);
        dprintf(stdout_fileno, "%s%s\e[0m\n",
                COLORS[entry.dirent.d_type], entry.dirent.d_name);
}

void
calc_wsize(int _)
{
        /* To be called on SIGWINCH */
        ioctl(0, TIOCGWINSZ, &wsize);
}

void
refresh()
{
        int i;
        /* I don't know how this work, just assume calcs are right */
        int ws = (wsize.ws_row - 1 < dir_arr.size) ? wsize.ws_row - 1 :
                                                     dir_arr.size;

        if (selected_row < woffset) woffset = selected_row;
        if (selected_row >= woffset + ws) woffset = selected_row - ws + 1;

        dprintf(stdout_fileno, "\e[2J\e[H");
        for (i = woffset; i < ws + woffset; i++) {
                if (i == selected_row) dprintf(stdout_fileno, "\e[7m");
                print_file(dir_arr.data[i]);
        }
        fsync(stdout_fileno);
}

int
getkey()
{
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) {
                error("Error reading from stdin");
                abort();
        }
        switch (c) {
                /* Handle special stuff here */
        default:
                return c;
        }
}

int
is_folder(struct dirent entry)
{
        switch (entry.d_type) {
        case DT_DIR:
                return 1;
        case DT_LNK:
                /* Todo: accept symlinks */
        default:
                return 0;
        }
}

void
place_cursor_midwindow()
{
        selected_row = dir_arr.size / 2;
        woffset = (wsize.ws_row >= dir_arr.size) ?
                  0 :
                  selected_row - wsize.ws_row / 2;
}


void
change_dir()
{
        char path[1024];

        staticstrconcat(path, 1024,
                        dir_arr.data[selected_row].path, "/",
                        dir_arr.data[selected_row].dirent.d_name);

        if (!is_folder(dir_arr.data[selected_row].dirent)) {
                report("Can not change dir to %s: it is not a folder", path);
                return;
        }
        if (chdir(path)) {
                error("Can not change dir to %s", path);
                return;
        }

        dir_arr.size = 0;
        add_subfolder(".", NULL, -1);
        place_cursor_midwindow();
}

/* TODO: this is ugly as fuck. Noodle code :) */
int
create_filename_path_if_not_exists(const char *path)
{
        char *full_path = strdup(path);
        char *current_path = full_path;
        char *curr_dir = full_path;
        struct stat buf;

        while ((current_path = strchr(current_path, '/'))) {
                *current_path = 0;
                if (stat(full_path, &buf) != -1)
                        ; // check if dir exists
                else if (*full_path == 0)
                        ; // check if full path is not empty
                else if (!strcmp(".", curr_dir))
                        ; // check if curr_dir is not '.'
                else if (mkdir(full_path, 0700)) {
                        error("Can't mkdir `%s`", full_path);
                        free(full_path);
                        return -1;
                }
                /* Restore '/' and point to the first char after the '/' */
                *(current_path++) = '/';
                curr_dir = current_path;
        }

        free(full_path);
        return 0;
}

int
store_remove(const char *filename)
{
        char buffer[1024 * 1024];
        int fd_in, fd_out;
        char backup_filename[1024];
        ssize_t n;

        staticstrconcat(backup_filename, 1024, UNDO_BACKUP_DIR, "/", filename);
        if (create_filename_path_if_not_exists(backup_filename)) {
                error("Can not create path for file: `%s`", backup_filename);
                return -1;
        }

        fd_out = open(backup_filename, O_CREAT | O_WRONLY, 0600);
        /* Todo: use prev permissions */
        if (fd_out < 0) {
                error("Can not create file: `%s`", backup_filename);
                return -1;
        }

        fd_in = open(filename, O_RDONLY, 0600);
        /* Todo: use prev permissions */
        if (fd_in < 0) {
                error("Can not open file: `%s`", filename);
                return -1;
        }

        while ((n = read(fd_in, buffer, sizeof buffer)))
                if (write(fd_out, buffer, n) != n) {
                        error("Error writing file: `%s`", filename);
                        return -1;
                }

        if (n < 0) {
                error("Error reading file: `%s`", backup_filename);
                return -1;
        }

        if (remove(filename)) {
                error("Can't remove `%s`", filename);
                return -1;
        }

        return 0;
}

int
restore(const char *filename)
{
        char buffer[1024 * 1024];
        int fd_in, fd_out;
        char *backup_filename;
        ssize_t n;

        fd_out = open(filename, O_CREAT | O_WRONLY, 0600);
        /* Todo: use prev permissions */
        if (fd_out < 0) {
                error("Can not create file `%s`\n", filename);
                goto __error_exit;
        }

        backup_filename = strconcat(UNDO_BACKUP_DIR, "/", filename);
        fd_in = open(backup_filename, O_RDONLY, 0600);
        /* Todo: use prev permissions */
        if (fd_in < 0) {
                error("Can not open file: `%s`\n", backup_filename);
                goto __error_exit;
        }

        while ((n = read(fd_in, buffer, sizeof buffer)))
                if (write(fd_out, buffer, n) != n) {
                        error("Error writing file: `%s`", filename);
                        goto __error_exit;
                }

        if (n < 0) {
                error("Error reading file: `%s`", backup_filename);
                goto __error_exit;
        }

        /* I dont like how the following code looks like. TODO: refactoring */
        free(backup_filename);
        return 0;
__error_exit:
        free(backup_filename);
        return -1;
}

void
select_matching_file(const char *restrict pattern)
{
        int cflags = REG_EXTENDED | REG_ICASE | REG_NEWLINE;
        int eflags = 0;
        regex_t regex;
        regmatch_t pmatch[1];
        size_t nmatch = 0;
        char *input;
        int i;
        int errcode;
        char buf[1024];
        int offset;
        int size;

        if (pattern[0] == 0) return;

        if ((errcode = regcomp(&regex, pattern, cflags))) {
                size = regerror(errcode, &regex, buf, sizeof buf);
                report("regcomp error: %*s", size, buf);
        }
        for (offset = 1; offset < dir_arr.size; offset++) {
                i = (offset + selected_row) % dir_arr.size;
                staticstrconcat(buf, sizeof(buf) - 1,
                                dir_arr.data[i].path, "/",
                                dir_arr.data[i].dirent.d_name);
                if (regexec(&regex, buf, 1, pmatch, eflags) != REG_NOMATCH) {
                        selected_row = i;
                        break;
                }
        }
        regfree(&regex);
}

#define TRIM_R(string)                                 \
        do {                                           \
                char *c = string + strlen(string) - 1; \
                if (c < string) break;                 \
                while (c >= string && isspace(*c))     \
                        --c;                           \
                c[1] = 0;                              \
        } while (0)


void
search()
{
        soft_disable_custom_mode();
        fgets(pattern, sizeof pattern - 1, stdin);
        TRIM_R(pattern);
        enable_custom_mode();
        select_matching_file(pattern);
}

void
mainloop()
{
        int action;
        int quit = 0;
        char *filename;
        struct extend_dirent temp;

        enable_custom_mode();
        refresh();

        while (!quit) {
                action = getkey();

                switch (action) {
                case 'q':
                case 0x3: /* C-c */
                        quit = 1;
                        break;

                case 'k':
                        if (!selected_row--) selected_row = 0;
                        refresh();
                        break;
                case 'j':
                        if (++selected_row >= dir_arr.size) selected_row--;
                        refresh();
                        break;

                case 'K':
                        if (selected_row == 0) break;
                        temp = dir_arr.data[selected_row];
                        dir_arr.data[selected_row] =
                        dir_arr.data[selected_row - 1];
                        dir_arr.data[selected_row - 1] = temp;
                        --selected_row;
                        refresh();
                        break;
                case 'J':
                        if (selected_row >= dir_arr.size - 1) break;
                        temp = dir_arr.data[selected_row];
                        dir_arr.data[selected_row] =
                        dir_arr.data[selected_row + 1];
                        dir_arr.data[selected_row + 1] = temp;
                        ++selected_row;
                        refresh();
                        break;

                case 'd':
                        if (do_not_delete) break;
                        temp = dir_arr.data[selected_row];
                        filename = strconcat(temp.path, "/", temp.dirent.d_name);
                        if (store_remove(filename)) break;
                        free(filename);
                        da_append(&deleted_dir_arr, temp);
                        da_remove(&dir_arr, selected_row);
                        if (selected_row == dir_arr.size) --selected_row;
                        refresh();
                        break;

                case 'u':
                        if (deleted_dir_arr.size == 0) break;
                        temp = deleted_dir_arr.data[--deleted_dir_arr.size];
                        filename = strconcat(temp.path, "/", temp.dirent.d_name);
                        restore(filename);
                        free(filename);
                        da_append(&dir_arr, temp);
                        sort();
                        refresh();
                        break;

                case ' ':
                        change_dir();
                        refresh();
                        break;

                case '/':
                        dprintf(stdout_fileno, "search >> ");
                        fsync(stdout_fileno);
                        search();
                        refresh();
                        break;
                case 'n':
                        select_matching_file(pattern);
                        refresh();
                        break;

                case 13:
                case '\b': // backspace
                        if (!is_folder(dir_arr.data[selected_row].dirent)) {
                                edit_file(dir_arr.data[selected_row].dirent.d_name, dir_arr.data[selected_row].path);
                        } else if (is_folder_open(dir_arr.data[selected_row].dirent.d_name,
                                                  dir_arr.data[selected_row].path))
                                remove_subfolder(dir_arr.data[selected_row].dirent.d_name,
                                                 dir_arr.data[selected_row].path);
                        else
                                add_subfolder(dir_arr.data[selected_row].dirent.d_name,
                                              dir_arr.data[selected_row].path, selected_row + 1);
                        refresh();
                        break;

                case 's':
                        sort();
                        refresh();
                        break;

                case 'g':
                        selected_row = 0;
                        refresh();
                        break;
                case 'G':
                        selected_row = dir_arr.size - 1;
                        refresh();
                        break;

                default:
                        refresh();
                        break;
                }
        }

        disable_custom_mode();
}

int
main(int argc, char *argv[])
{
        int i;
        char *path;
        char cwd[1024];

        flag_set(&argc, &argv);
        if (flag_get("-E", "--external")) open_as_external = 1;
        if (flag_get("-I", "--internal")) open_as_external = 0;
        if (flag_get("-D", "--no-delete", "--dumb")) do_not_delete = 1;
        if (flag_get_value(&path, "-d", "--directory")) {
                if (chdir(path)) {
                        error("Can not change dir to %s", path);
                        return -1;
                }
        }

        if (!isatty(STDIN_FILENO)) {
                report("Can't use fl, stdin doesn't refer to a terminal");
                return -1;
        }

        stdout_fileno = open("/dev/tty", O_WRONLY);

        if (!stdout_fileno) {
                report("Can't use fl, stdout doesn't refer to a terminal:%s",
                       strerror(errno));
                return -1;
        }

        if (signal(SIGWINCH, calc_wsize) == SIG_ERR) {
                report("Can't set window resize handler");
                return -1;
        }

        for (i = 1; i < argc; i++) {
                add_subfolder(argv[i], NULL, -1);
        }
        add_subfolder(".", NULL, -1);

        calc_wsize(0);
        place_cursor_midwindow();
        mainloop();
        printf("%s\n", getcwd(cwd, 1024));

        return 0;
}
