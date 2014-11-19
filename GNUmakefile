all : atire_to_compiled main

main : main.c
	make -f makefile.ci

atire_to_compiled : atire_to_compiled.c
	g++ -x c++ atire_to_compiled.c -o atire_to_compiled compress_variable_byte.c

clean:
	-rm -rf CIpostings
	-rm atire_to_compiled main *.o CIvocab.c CIpostings.h CIpostings.c CIdoclist.c

