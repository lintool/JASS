all : atire_to_compiled atire_to_heap main_heap main

main : main.c CIvocab.c
	make -f makefile.ci

atire_to_compiled : atire_to_compiled.c
	g++ -x c++ atire_to_compiled.c -o atire_to_compiled compress_variable_byte.c

main_heap : main_heap.c CIvocab.c
	g++ -O3 -g -o main_heap -x c++ main_heap.c CIdoclist.c CI.c CIvocab.c

atire_to_heap : atire_to_heap.c
	g++ -x c++ atire_to_heap.c -o atire_to_heap compress_variable_byte.c

clean:
	-rm -rf CIpostings
	-rm atire_to_compiled atire_to_heap main_heap main *.o CIvocab.c CIpostings.h CIpostings.c CIdoclist.c

