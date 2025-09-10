KERN_DIR = /luckfox-pico-main/sysdrv/source/kernel
CROSS_COMPILE = arm-rockchip830-linux-uclibcgnueabihf-
ARCH = arm

obj-m += joypad.o

all:

	make -C $(KERN_DIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules_prepare

	make -C $(KERN_DIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) modules

clean:
	make -C $(KERN_DIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) clean
	rm -f modules.order

