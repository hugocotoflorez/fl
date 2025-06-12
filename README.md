# fl - file list

## USAGE
1. Compile it with some C compiler: `cc fl.c -o fl`
2. Execute it: `./fl [OPTIONS]`

### makefile installation
There is a makefile for compiling and installing it. It is needed to have
`~/.local/bin` in path.
```sh
make install
```

### OPTIONS
- `-E`,  `--external`: Open files using xdg-open instead of `$EDITOR`.
- `-D`, `--no-delete`, `--dumb`: Do not delete files if pressing `d`.
- `-d`, `--directory`: Change working directory for program execution.

## KEYBINDS
- `k`, `j`: Move selector up and down.
- `K`, `J`: Move selected entry up and down. (Useless for now).
- `Enter`: Expand folder or open file. Links are not supported yet (UB).
- `d`: Delete selected file.
- `r`: Restore last file deleted.
- `space`: Change working directory to selected entry.
- `/`: Search for a pattern and select first occurence.
- `n`: Select next occurence.

## REFERENCES
- https://gist.github.com/fnky/458719343aabd01cfb17a3a4f7296797

## HOW DELETION WORK
1. File is copied to /tmp, so it is removed on system power off.
2. If undoing, file is copied from /tmp backup to current directory.
3. If you delete a file by error, it is in /tmp.

Undo list is not perserved between program executions. Note that backup path is
set in source code, under the name of UNDO_BACKUP_DIR.

## Things that (may) work
1. Select a single entry.
2. Move selector.
3. Sort by name.
4. Color entries by type.
5. Expand folders.
6. Open files (internal and externally).
7. Move selected row up and down (useless feature).
8. Delete files.
9. Restore deleted files.
10. Change working directory.
11. Place selector in the middle (less movement is required).
12. Improve mid cursor positioning.
13. Moving window for large directories.

