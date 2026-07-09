obj-m += octshm_host.o
obj-m += octdma_test.o
KDIR ?= /lib/modules/$(shell uname -r)/build
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
obj-m += octwin.o
obj-m += octoq_host.o
obj-m += octconsole.o
