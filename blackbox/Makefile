CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -pthread
LIB = blackbox.c sha256.c
all: bbtest bbverify bbbench
bbtest: bbtest.c toysim.c $(LIB) blackbox.h sha256.h toysim.h bb_platform.h
	$(CC) $(CFLAGS) -o $@ bbtest.c toysim.c $(LIB)
bbverify: bbverify.c $(LIB) blackbox.h
	$(CC) $(CFLAGS) -o $@ bbverify.c $(LIB)
bbbench: bbbench.c $(LIB) blackbox.h
	$(CC) $(CFLAGS) -o $@ bbbench.c $(LIB)
test: bbtest
	./bbtest
asan: 
	$(CC) -std=c11 -g -O1 -Wall -Wextra -pthread -fsanitize=address,undefined -o bbtest_asan bbtest.c toysim.c $(LIB) && ./bbtest_asan
tsan:
	$(CC) -std=c11 -g -O1 -pthread -fsanitize=thread -o bbtest_tsan bbtest.c toysim.c $(LIB) && ./bbtest_tsan
clean:
	rm -f bbtest bbverify bbbench bbtest_asan bbtest_tsan *.o *.bbx
.PHONY: all test asan tsan clean
