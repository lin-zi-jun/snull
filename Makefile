# Comment/uncomment the following line to disable/enable debugging
#DEBUG = y


# Add your debugging flag (or not) to CFLAGS
export ARCH=arm
export CROSS_COMPILE=/home/linzijun/im6q/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-
ifeq ($(DEBUG),y)
  DEBFLAGS = -O -g -DSBULL_DEBUG # "-O" is needed to expand inlines
else
  DEBFLAGS = -O2
endif

EXTRA_CFLAGS += $(DEBFLAGS)
EXTRA_CFLAGS += -I..

ifneq ($(KERNELRELEASE),)
# call from kernel build system

obj-m    := snull.o

else

KERNELDIR ?= /home/linzijun/im6q/linux-imx-3.14.28
PWD       := $(shell pwd)

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
	cp snull.ko /home/linzijun/tftpboot
#	gcc client.c -o client
#	arm-linux-gnueabihf-gcc server.c -o server
#	cp client server /home/linzijun/tftpboot

endif
