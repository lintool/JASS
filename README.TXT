How to make it work:

0. make
1. Generate and ATIRE index
2. atire_dictionary > index.dump
3. atire_doclist
4. generate the topic file with ATIRE's evaluation/trec2query TRECfile t > topics.in 
5. atire_to_compiled index.dump doclist.aspt topics.in
6. make -f makefile.ci
7. ./main topics.in
