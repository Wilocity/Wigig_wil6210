obj-$(CONFIG_WIL6210) += wil6210.o

wil6210-y := main.o
wil6210-y += netdev.o
wil6210-y += cfg80211.o
wil6210-y += pcie_bus.o
wil6210-y += debugfs.o
wil6210-y += wmi.o
wil6210-y += interrupt.o
wil6210-y += txrx.o
wil6210-y += rx_reorder.o
wil6210-objs += offchannel.o
wil6210-y += debug.o
wil6210-$(CONFIG_WIL6210_TRACING) += trace.o

ifeq (, $(findstring -W,$(EXTRA_CFLAGS)))
	subdir-ccflags-y += -Werror
endif
# for tracing framework to find trace.h
CFLAGS_trace.o := -I$(src)

subdir-ccflags-y += -D__CHECK_ENDIAN__

obj-y += tools/

### Module version
wil6210-y += version.o

### extra stuff for stand alone module.
CONFIG_WIL6210 := m

# Use Clear-On-Read
subdir-ccflags-y += -DCONFIG_WIL6210_ISR_COR=1

# trace
ifeq (y, $(CONFIG_WIL6210_TRACING))
	subdir-ccflags-y += -DCONFIG_WIL6210_TRACING
endif

KERN_BUILD ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean archive

all:
	make -C $(KERN_BUILD) $(ARCH_FLAGS) M=$(PWD) modules C=2

clean:
	make -C $(KERN_BUILD) $(ARCH_FLAGS) M=$(PWD) clean

archive:
	git archive --format=tar --prefix=wil6210/ HEAD | bzip2 > wil6210.tar.bz2
