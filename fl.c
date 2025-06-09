#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "flag/flag.h"
#include "frog/frog.h"

#define UNDO_BACKUP_DIR "/tmp/fl-backup"

static int open_as_external = 0;
static int do_not_delete = 0;

/* Colors for specific entry types. "" is set to default */
static const char *COLORS[] = {
        [DT_BLK] = "", // This is a block device.
        [DT_CHR] = "", // This is a character device.
        [DT_DIR] = "\e[34m", // This is a directory.
        [DT_FIFO] = "", // This is a named pipe (FIFO).
        [DT_LNK] = "\e[36m", // This is a symbolic link.
        [DT_REG] = "", // This is a regular file.
        [DT_SOCK] = "", // This is a UNIX domain socket.
        [DT_UNKNOWN] = "", // The file type could not be determined.
};


struct extend_dirent {
        struct dirent dirent;
        char path[256];
};

typedef DA(struct extend_dirent) dirent_da;
dirent_da dir_arr = { 0 };
dirent_da deleted_dir_arr = { 0 };
int selected_row = 0;

struct termios origin_termios;

char *
__strconcat(const char *s1, ...)
{
        va_list ap;
        char *result = strdup(s1);
        char *current;
        va_start(ap, s1);
        while ((current = va_arg(ap, char *))) {
                result = realloc(result, strlen(current));
                assert(result);
                strcat(result, current);
        }
        return result;
}
#define strconcat(s1, ...) __strconcat(s1, ##__VA_ARGS__, NULL)

void
enable_raw_mode()
{
        struct termios raw_opts;
        tcgetattr(STDIN_FILENO, &origin_termios);
        raw_opts = origin_termios;
        cfmakeraw(&raw_opts);
        raw_opts.c_oflag |= (OPOST | ONLCR); // '\n' -> '\r\n'
        tcsetattr(STDIN_FILENO, TCSANOW, &raw_opts);
}

void
disable_raw_mode()
{
        tcsetattr(STDIN_FILENO, TCSANOW, &origin_termios);
}

/* If this is defined as a function, the alternative buffer does not work.
 * So, it is defined as a macro :/ */
#define disable_custom_mode()                                         \
        {                                                             \
                /* disable alternative buffer */ printf("\e[?1049l"); \
                /* make cursor visible        */ printf("\e[?25h");   \
                disable_raw_mode();                                   \
        }

#define enable_custom_mode()                                          \
        {                                                             \
                enable_raw_mode();                                    \
                /* make cursor invisible      */ printf("\e[?25l");   \
                /* enable alternative buffer  */ printf("\e[?1049h"); \
                /* clear screen               */ printf("\e[H\e[2J"); \
        }

/* Coments in the code above are written before the actual code to be able to
 * use it inside the macro, don't judge me, please. */

void
report(const char *restrict format, ...)
{
        FILE *file = fopen("log.txt", "a");
        va_list ap;
        va_start(ap, format);
        vfprintf(file, format, ap);
        fprintf(file, "\n");
        fclose(file);
}

int
sort_cmp(const void *_a, const void *_b)
{
        struct extend_dirent *a = (struct extend_dirent *) _a;
        struct extend_dirent *b = (struct extend_dirent *) _b;

        char *fullname_a = strconcat(a->path, "/", a->dirent.d_name);
        char *fullname_b = strconcat(b->path, "/", b->dirent.d_name);

        int result = strcmp(fullname_a, fullname_b);

        free(fullname_a);
        free(fullname_b);

        return result;
}

void
sort()
{
        qsort(dir_arr.data, dir_arr.size, sizeof *dir_arr.data, sort_cmp);
}

#define amalloc(size) ({ __auto_type _ptr_ = malloc(size); assert(_ptr_); _ptr_; })

void
edit_file(const char *path, const char *subpath)
{
        char *p = strdup(path);
        if (!p) {
                report("Critical error: can not alloc memory");
                abort();
        }

        if (open_as_external) {
                switch (fork()) {
                case -1:
                        report("Fork failed");
                        return;
                case 0:
                        if (subpath) p = strconcat(subpath, "/", path);
                        execvp("xdg-open", (char *const[]) { "xdg-open", p, NULL });
                        report("Execv failed: %s. At:", strerror(errno));
                        report("execv(\"xdg-open\", (char *const[]) { \"xdg-open\", %s, NULL });", p);
                        free(p);
                        abort();
                }

        } else {
                int child;
                disable_custom_mode();

                switch (child = fork()) {
                case -1:
                        report("Fork failed");
                        return;

                case 0:
                        if (subpath) p = strconcat(subpath, "/", path);
                        char *editor = getenv("EDITOR");
                        if (editor == NULL) {
                                report("Can not find env `EDITOR`");
                                abort();
                        }
                        execvp(editor, (char *const[]) { editor, p, NULL });
                        report("Execv failed: %s. At:", strerror(errno));
                        report("execv(%s, (char *const[]) { %s, %s, NULL });", editor, p, editor);
                        free(p);
                        abort();

                default:
                        waitpid(child, NULL, 0);
                        break;
                }
                enable_custom_mode();
        }
}

/* Add path from subpath path, at list index at. subpath can be null if using
 * current dir, and at can be -1 to append it. */
void
add_subfolder(const char *path, const char *subpath, int at)
{
        struct dirent *entry;
        struct extend_dirent edirent;
        char *p;

        if (subpath)
                p = strconcat(subpath, "/", path);
        else
                p = strdup(path);

        DIR *dir = opendir(p);
        if (!dir) {
                report("Can not open dir: %s", path);
                return;
        }

        if (at < 0 || at > dir_arr.size) at = dir_arr.size;

        while ((entry = readdir(dir))) {
                if (!strcmp(entry->d_name, ".")) continue; // do not add "." to files
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

        if (subpath)
                p = strconcat(subpath, "/", path);
        else
                p = strdup(path);
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

        if (subpath)
                p = strconcat(subpath, "/", path);
        else
                p = strdup(path);
        int i;
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
        if (strcmp(path, ".")) printf("%s/", path);
        printf("%s%s\e[0m\n", COLORS[entry.dirent.d_type], entry.dirent.d_name);
}

void
refresh()
{
        int i;
        /* Erase screen and place cursor at the top left corner */
        printf("\e[2J\e[H");
        for (i = 0; i < dir_arr.size; i++) {
                if (i == selected_row) {
                        printf("\e[7m");
                        print_file(dir_arr.data[i]);
                } else
                        print_file(dir_arr.data[i]);
        }
        fflush(stdout);
}

int
getkey()
{
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1)
                report("Error reading from stdin: %s", strerror(errno));

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

/* TODO: this is ugly as fuck. Noodle code :) */
int
create_filename_path_if_not_exists(const char *filename)
{
        char *f = strdup(filename);
        char *c = f;
        /* TODO: remove ff as it is not clear at all. But it works fine */
        char *ff = f; // start of current dir being analized (just one)
        struct stat buf;
        while ((c = strchr(c, '/'))) {
                *c = 0;
                if (stat(f, &buf) == -1)
                        ; // check that dir does not exist
                else if (*f != 0 && strcmp(".", ff) && mkdir(f, 0700)) {
                        report("Can't mkdir `%s`: %s", f, strerror(errno));
                        goto __error_exit;
                }
                *c = '/';
                c++;
                ff = c;
        }

        /* I dont like this. TODO: refactoring */
        free(f);
        return 0;
__error_exit:
        free(f);
        return -1;
}


int
store_remove(const char *filename)
{
        char buffer[1024 * 1024];
        int fd_in, fd_out;
        char *backup_filename;
        ssize_t n;

        backup_filename = strconcat(UNDO_BACKUP_DIR, "/", filename);
        if (create_filename_path_if_not_exists(backup_filename)) {
                report("Can not create path for file: `%s`: %s", backup_filename, strerror(errno));
                goto __error_exit;
        }

        fd_out = open(backup_filename, O_CREAT | O_WRONLY, 0600);
        /* Todo: use prev permissions */
        if (fd_out < 0) {
                report("Can not create file: `%s`: %s", backup_filename, strerror(errno));
                goto __error_exit;
        }

        fd_in = open(filename, O_RDONLY, 0600);
        /* Todo: use prev permissions */
        if (fd_in < 0) {
                report("Can not open file: `%s`: %s", filename, strerror(errno));
                goto __error_exit;
        }

        while ((n = read(fd_in, buffer, sizeof buffer)))
                if (write(fd_out, buffer, n) != n) {
                        report("Error writing file: `%s`: %s", filename, strerror(errno));
                        goto __error_exit;
                }

        if (n < 0) {
                report("Error reading file: `%s`: %s", backup_filename, strerror(errno));
                goto __error_exit;
        }

        if (remove(filename)) {
                report("Can't remove `%s`: %s", filename, strerror(errno));
                goto __error_exit;
        }

        /* I dont like how the following code looks like. TODO: refactoring */
        free(backup_filename);
        return 0;
__error_exit:
        free(backup_filename);
        return -1;
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
                report("Can not create file: `%s`\n", filename);
                goto __error_exit;
        }

        backup_filename = strconcat(UNDO_BACKUP_DIR, "/", filename);
        fd_in = open(backup_filename, O_RDONLY, 0600);
        /* Todo: use prev permissions */
        if (fd_in < 0) {
                report("Can not open file: `%s`\n", backup_filename);
                goto __error_exit;
        }

        while ((n = read(fd_in, buffer, sizeof buffer)))
                if (write(fd_out, buffer, n) != n) {
                        report("Error writing file: `%s`: %s", filename, strerror(errno));
                        goto __error_exit;
                }

        if (n < 0) {
                report("Error reading file: `%s`: %s", backup_filename, strerror(errno));
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
                        if (!selected_row--)
                                selected_row = 0;
                        refresh();
                        break;
                case 'j':
                        if (++selected_row >= dir_arr.size)
                                selected_row--;
                        refresh();
                        break;

                case 'K':
                        if (selected_row == 0) break;
                        temp = dir_arr.data[selected_row];
                        dir_arr.data[selected_row] = dir_arr.data[selected_row - 1];
                        dir_arr.data[selected_row - 1] = temp;
                        --selected_row;
                        refresh();
                        break;
                case 'J':
                        if (selected_row >= dir_arr.size - 1) break;
                        temp = dir_arr.data[selected_row];
                        dir_arr.data[selected_row] = dir_arr.data[selected_row + 1];
                        dir_arr.data[selected_row + 1] = temp;
                        ++selected_row;
                        refresh();
                        break;

                case 'd':
                        if (do_not_delete) break;
                        temp = dir_arr.data[selected_row];
                        filename = strconcat(temp.path, "/", temp.dirent.d_name);
                        if (store_remove(filename)) {
                                break;
                        }
                        da_append(&deleted_dir_arr, temp);
                        da_remove(&dir_arr, selected_row);
                        free(filename);
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


                case 13:
                case '\b': // backspace
                        if (!is_folder(dir_arr.data[selected_row].dirent)) {
                                edit_file(dir_arr.data[selected_row].dirent.d_name, dir_arr.data[selected_row].path);
                        }

                        else if (is_folder_open(dir_arr.data[selected_row].dirent.d_name,
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

                default:
                        refresh();
                        report("Pressed unused char: %d", action);
                        break;
                }
        }

        disable_custom_mode();
}

int
main(int argc, char *argv[])
{
        int i;

        flag_set(&argc, &argv);

        /* This is totally useless */
        if (flag_get("-E", "--external")) open_as_external = 1;
        if (flag_get("-D", "--no-delete", "--dumb")) do_not_delete = 1;

        for (i = 1; i < argc; i++)
                add_subfolder(argv[i], NULL, -1);

        if (argc == 1) add_subfolder(".", NULL, -1);

        mainloop();

        return 0;
}
