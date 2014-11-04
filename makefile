atire_to_compiled : atire_to_compiled.c
	g++ atire_to_compiled.c -o atire_to_compiled


clean:
	rm atire_to_compiled main *.o CIvocab.c CIpostings.h CIpostings.c CIdoclist.c
	rm -rf CIpostings

