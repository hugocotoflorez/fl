install: fl
	cp fl ~/.local/bin

fl: fl.c
	cc fl.c -o fl

