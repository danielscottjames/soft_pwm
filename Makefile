obj-m       := soft_pwm.o

KVERSION= 5.15.93-sunxi

all:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) modules
clean:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) clean
