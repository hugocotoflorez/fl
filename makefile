fl: fl.c
	cc fl.c -o fl

install: fl
	cp fl ~/.local/bin
