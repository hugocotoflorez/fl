fl: fl.c
	cc fl.c -o fl -DEXTERNAL

install: fl
	cp fl ~/.local/bin
