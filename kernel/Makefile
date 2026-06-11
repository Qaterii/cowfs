obj-m := cowfs.o

cowfs-objs := cowfs_main.o     \
              cowfs_super.o    \
              cowfs_inode.o    \
              cowfs_file.o     \
              cowfs_dir.o      \
              cowfs_shadow.o   \
              cowfs_versions.o \
              cowfs_ctl.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
