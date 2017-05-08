obj-m += eurocom-mx5r2-keyleds.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
#CFLAGS_eurocom-mx5r2-keyleds.o := -DDEBUG

all:
	make -C $(KDIR) M=$(PWD) modules

install:
	make -C $(KDIR) M=$(PWD) modules_install

clean:
	make -C $(KDIR) M=$(PWD) clean
