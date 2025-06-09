# fl - file list

## USAGE
1. Compile it with some C compiler: `cc fl.c -o fl`
2. Execute it: `./fl [OPTIONS]`

### OPTIONS
-E,  --external: Open files using xdg-open instead of `$EDITOR`.
-D, --no-delete, --dumb: Do not delete files if pressing `d`.

## KEYBINDS

- `k`, `j`: Move selector up and down.
- `K`, `J`: Move selected entry up and down. (Useless for now).
- `Enter`: Expand folder or open file. Links are not supported (UB).
- `d`: Delete the selected file.
- `r`: Restore the last file deleted. (Not perserved between sessions).

## REFERENCES
- https://gist.github.com/fnky/458719343aabd01cfb17a3a4f7296797

## HOW DELETION WORK
1. File is copied to /tmp, so it is removed on system power off.
2. If undoing, file is copied from /tmp backup.
3. If you delete a file by error, it is in /tmp.

Note that the backup path is set in the source code, under the name
of UNDO_BACKUP_DIR.
