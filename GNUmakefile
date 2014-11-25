CI_FLAGS = -O3 -x c++ -DCI_FORCEINLINE

all : atire_to_compiled atire_to_heap main_heap main

main : main.c CIvocab.c CIdoclist.c CI.c CI.h CIvocab.c
	g++ $(CI_FLAGS) main.c CIdoclist.c CI.c CIvocab.c -o main
	cd CIpostings ; make ; cd ..

atire_to_compiled : atire_to_compiled.c
	g++ $(CI_FLAGS) atire_to_compiled.c compress_variable_byte.c -o atire_to_compiled

main_heap : main_heap.c CIvocab_heap.c CIdoclist.c CI.c
	g++ $(CI_FLAGS) main_heap.c CIdoclist.c CI.c CIvocab_heap.c -o main_heap

atire_to_heap : atire_to_heap.c
	g++ $(CI_FLAGS) atire_to_heap.c compress_variable_byte.c compress_simple8b.c maths.c -o atire_to_heap

clean:
	-rm -rf CIpostings
	-rm atire_to_compiled atire_to_heap main_heap main *.o CIvocab.c CIpostings.h CIpostings.c CIdoclist.c CIvocab_heap.c CIpostings.bin

CIdoclist.c CIvocab_heap.c :
	@echo "\nNOTE: now run atire_to_heap index.dump doclist.asp [topics]\n"
	@false

CIvocab.c :
	@echo "\nNOTE: now run atire_to_compiled index.dump doclist.asp [topics] [-c|-s]\n"
	@false
