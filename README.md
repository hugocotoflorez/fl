# fl - file list

## USAGE
1. Compile it with some C compiler: `cc fl.c -o fl`
2. Execute it: `./fl [OPTIONS]`

### OPTIONS
-E,  --external: Open files using xdg-open instead of `$EDITOR`.

## KEYBINDS

- `k`, `j`: Move selector up and down.
- `K`, `J`: Move selected entry up and down. (Useless for now).
- `Enter`: Expand folder or open file. Links are not supported (UB).

## REFERENCES
- https://gist.github.com/fnky/458719343aabd01cfb17a3a4f7296797

