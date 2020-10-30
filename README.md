Use makefile to create the unlinked binary sut.o file:
    $ make sut.o

To test the library code with a program say, test1.c, run:
    $ gcc -pthread test1.c sut.o
    
followed by:
    $ ./a.out