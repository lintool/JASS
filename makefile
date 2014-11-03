atire_to_compiled : atire_to_compiled.c
	g++ -O3 atire_to_compiled.c -o atire_to_compiled


clean:
	rm atire_to_compiled main *.o CIvovab.h CIpostings.h CIpostings.c
