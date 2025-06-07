#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "flag/flag.h"
#include "frog/frog.h"


struct extend_dirent {
        struct dirent dirent;
        char path[256];
};

typedef DA(struct extend_dirent) dirent_da;
dirent_da dir_arr;
int selected_row = 0;


void
report(const char *restrict format, ...)
{
        va_list ap;
        va_start(ap, format);
        printf("\e[H");
        vprintf(format, ap);
}

#define amalloc(size) ({ __auto_type _ptr_ = malloc(size); assert(_ptr_); _ptr_; })

void
add_subfolder(const char *path, const char *subpath)
{
        struct dirent *entry;
        struct extend_dirent edirent;

        char *p = (char *) path;
        if (subpath) {
                p = amalloc(strlen(path) + strlen(subpath) + 2);
                *p = 0;
                strcat(p, subpath);
                strcat(p, "/");
                strcat(p, path);
        }
        DIR *dir = opendir(p);
        if (!dir) {
                report("Can not open dir: %s", path);
                return;
        }

        while ((entry = readdir(dir))) {
                edirent.dirent = *entry;
                strcpy(edirent.path, p);
                da_append(&dir_arr, edirent);
        }
}

void
remove_subfolder(const char *path, const char *subpath)
{
        int i;
        char *p = (char *) path;
        if (subpath) {
                p = amalloc(strlen(path) + strlen(subpath) + 2);
                *p = 0;
                strcat(p, subpath);
                strcat(p, "/");
                strcat(p, path);
        }
        for (i = 0; i < dir_arr.size; i++) {
                if (!memcmp(p, dir_arr.data[i].path, strlen(p))) {
                        da_remove(&dir_arr, i);
                        --i;
                }
        }
}

int
is_folder_open(const char *path, const char *subpath)
{
        char *p = (char *) path;
        if (subpath) {
                p = amalloc(strlen(path) + strlen(subpath) + 2);
                *p = 0;
                strcat(p, subpath);
                strcat(p, "/");
                strcat(p, path);
        }
        int i;
        for (i = 0; i < dir_arr.size; i++) {
                if (!memcmp(p, dir_arr.data[i].path, strlen(p))) {
                        return 1;
                }
        }
        return 0;
}

void
print_file(struct extend_dirent entry)
{
        if (strcmp(entry.path, "."))
                printf("[%s/]", entry.path);
        printf("%s\n", entry.dirent.d_name);
}

void
print_files()
{
        int i;
        /* Erase screen and place cursor at top left corner */
        printf("\e[2J\e[H");
        for (i = 0; i < dir_arr.size; i++) {
                if (i == selected_row) {
                        printf("\e[%d;%dm", 30, 44);
                        print_file(dir_arr.data[i]);
                        printf("\e[0m");
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

struct termios origin_termios;
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
mainloop()
{
        int action;
        int quit = 0;
        struct extend_dirent temp;
        enable_raw_mode();
        printf("\e[?25l"); // make cursor invisible
        printf("\e[?1049h"); // enable alternative buffer
        printf("\e[H\e[2J"); // clear screen

        print_files();
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
                        print_files();
                        break;
                case 'j':
                        if (++selected_row >= dir_arr.size)
                                selected_row--;
                        print_files();
                        break;

                case 'K':
                        if (selected_row == 0) break;
                        temp = dir_arr.data[selected_row];
                        dir_arr.data[selected_row] = dir_arr.data[selected_row - 1];
                        dir_arr.data[selected_row - 1] = temp;
                        --selected_row;
                        print_files();
                        break;
                case 'J':
                        if (selected_row >= dir_arr.size - 1) break;
                        temp = dir_arr.data[selected_row];
                        dir_arr.data[selected_row] = dir_arr.data[selected_row + 1];
                        dir_arr.data[selected_row + 1] = temp;
                        ++selected_row;
                        print_files();
                        break;

                case 13:
                case '\b': // backspace
                        if (!is_folder(dir_arr.data[selected_row].dirent)) break;

                        if (is_folder_open(dir_arr.data[selected_row].dirent.d_name, dir_arr.data[selected_row].path))
                                remove_subfolder(dir_arr.data[selected_row].dirent.d_name, dir_arr.data[selected_row].path);

                        else
                                add_subfolder(dir_arr.data[selected_row].dirent.d_name,
                                              dir_arr.data[selected_row].path);

                        print_files();
                        break;
                default:
                        print_files();
                        report("Pressed char: %d", action);
                        break;
                }
        }

        printf("\e[?1049l");
        printf("\e[?25h");
        disable_raw_mode();
}

int
main(int argc, char *argv[])
{
        int iteractive = 1;
        int i;

        flag_set(&argc, &argv);
        if (flag_get("-I", "--no-iteractive")) iteractive = 0;

        for (i = 1; i < argc; i++)
                add_subfolder(argv[i], NULL);

        if (argc == 1) add_subfolder(".", NULL);

        if (iteractive)
                mainloop();
        else
                print_files();

        return 0;
}
