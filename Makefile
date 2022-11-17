all: power

CFLAGS := -D_GNU_SOURCE -Wall -pipe -Wstrict-prototypes -Wmissing-prototypes \
	-Wno-long-long -Wno-unused-parameter -Werror

power: power.c
	$(CC) -O0 -g $(CFLAGS) -o $@ $<

clean:
	$(RM) power
