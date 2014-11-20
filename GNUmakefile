all : atire_to_compiled atire_to_heap main_heap main

main : main.c CIvocab.c CIdoclist.c CI.c CI.h CIvocab.c
	g++ -O3 -o main -x c++ main.c CIdoclist.c CI.c CIvocab.c
	cd CIpostings ; make ; cd ..

atire_to_compiled : atire_to_compiled.c
	g++ -O3 -x c++ atire_to_compiled.c -o atire_to_compiled compress_variable_byte.c

main_heap : main_heap.c CIvocab_heap.c CIdoclist.c CI.c
	g++ -O3 -o main_heap -x c++ main_heap.c CIdoclist.c CI.c CIvocab_heap.c

atire_to_heap : atire_to_heap.c
	g++ -O3 -x c++ atire_to_heap.c -o atire_to_heap compress_variable_byte.c

clean:
	-rm -rf CIpostings
	-rm atire_to_compiled atire_to_heap main_heap main *.o CIvocab.c CIpostings.h CIpostings.c CIdoclist.c

CIdoclist.c CIvocab_heap.c :
	@echo "\nNOTE: now run atire_to_heap index.dump doclist.asp [topics]\n"
	@false

CIvocab.c :
	@echo "\nNOTE: now run atire_to_compiled index.dump doclist.asp [topics] [-c|-s]\n"
	@false