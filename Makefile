#
# Makefile for mcp2210
#
# Derived from the good-ole LDD3 Makefile
#

CONFIG_MODULE_SIG=n

ccflags-y += -Wall -Werror -Wunused-macros -Wno-implicit-fallthrough

# To build modules outside of the kernel tree, we run "make"
# in the kernel source tree; the Makefile these then includes this
# Makefile once again.
# This conditional selects whether we are being included from the
# kernel Makefile or not.
ifeq ($(KERNELRELEASE),)
    # Assume the source tree is where the running kernel was built
    # You should set KERNELDIR in the environment if it's elsewhere
    KERNELDIR ?= /lib/modules/$(shell uname -r)/build
    # The current directory is passed to sub-makes as argument
    PWD := $(shell pwd)

all: modules user

user:
	$(MAKE) -C user

# Temporary measure for building out-of-tree
out-of-tree-autoconf.h:
	cp -n out-of-tree-autoconf.h.template out-of-tree-autoconf.h

modules: out-of-tree-autoconf.h
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

clean:
	rm -rf *.o *.gcno *~ core .depend .*.cmd *.ko *.mod.c *.dwo .*.dwo \
		mcp2210.s .tmp_versions  modules.order Module.symvers
	$(MAKE) -C user clean

mcp2210.s: modules
	$(CROSS_COMPILE)objdump -dSr --prefix-addresses --line-numbers \
							mcp2210.ko > mcp2210.s

.PHONY: modules modules_install user clean

else
    # called from kernel build system: just declare what our modules are

    # When building out-of-tree, we need a way to get our config macros
    KBUILD_CPPFLAGS += -include $(PWD)/out-of-tree-autoconf.h

    # Many kernels < v3.0 will produce loads of spam with
    # -Wunused-but-set-variable set
    ifeq ($(VERSION),2)
        ccflags-y += -Wno-unused-but-set-variable
    endif

    CONFIG_MCP2210 ?= m
    mcp2210-objs := mcp2210-core.o mcp2210-ioctl.o mcp2210-ctl.o \
		    mcp2210-spi.o mcp2210-eeprom.o mcp2210-lib.o \
		    mcp2210-gpio.o mcp2210-irq.o

    obj-$(CONFIG_MCP2210)		+= mcp2210.o
endif

