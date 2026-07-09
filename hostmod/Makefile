# Host-side NIC module — native build against the running kernel.
#   octnic : registers oct0/oct1 over the card's BAR2 shared-memory window
obj-m += octnic.o
obj-m += octoq.o
KDIR ?= /lib/modules/$(shell uname -r)/build
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
