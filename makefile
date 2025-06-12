fl: fl.c
	cc fl.c -o fl

install: fl
	cp fl ~/.local/bin


debug:
	cc *.c -o fl-gdb -ggdb
	cc *.c -o fl-fsan -fsanitize=address,null
