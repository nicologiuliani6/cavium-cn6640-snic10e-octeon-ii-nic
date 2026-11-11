CC ?= cc
CFLAGS += -Wall -Werror

test:
	$(CC) $(CFLAGS) -o test_ring test_ring.c
	./test_ring

clean:
	rm -f test_ring

.PHONY: test clean
