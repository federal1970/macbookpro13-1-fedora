# Out-of-tree build of the in-tree cs8409 HDA codec driver with Apple
# MacBookPro13,1 support. hda/ mirrors sound/hda of the kernel: the driver
# sources and the private headers they include, both from Linux 7.2.
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/hda modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/hda clean
