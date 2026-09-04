CC      = gcc
CFLAGS  = -Wall -O2
LDLIBS  = -lcurl
 
all: ellf llf
 
ellf: ELLF.c
	$(CC) $(CFLAGS) ELLF.c -o ellf $(LDLIBS)
 
llf: LLF.c
	$(CC) $(CFLAGS) LLF.c -o llf $(LDLIBS)
 
clean:
	rm -f ellf llf
 
.PHONY: all clean