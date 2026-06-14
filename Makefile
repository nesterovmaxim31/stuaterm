stuaterm: stuaterm.o
	cc stuaterm.o -o stuaterm

stuaterm.o: stuaterm.c
	cc stuaterm.c -c

clean:
	rm *.o stuaterm
