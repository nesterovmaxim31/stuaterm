stuaterm: stuaterm.o
	cc stuaterm.o -funsigned-char -o stuaterm 

stuaterm.o: stuaterm.c
	cc stuaterm.c -c -funsigned-char -Wall -O0 -ggdb

clean:
	rm *.o stuaterm
