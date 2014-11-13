atire_to_compiled.exe : atire_to_compiled.c
	cl -Zi /Tp atire_to_compiled.c


clean:
	-del /q CIpostings
	-rmdir CIpostings
	-del atire_to_compiled.exe main.exe *.obj CIvocab.c CIpostings.h CIpostings.c CIdoclist.c *.bak *.pdb *.ilk *.suo *.lib


