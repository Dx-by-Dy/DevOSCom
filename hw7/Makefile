KDIR := /lib/modules/$(shell uname -r)/build
BUILD_DIR := $(CURDIR)/build
SRC_DIR := $(CURDIR)/src

all:
	mkdir -p $(BUILD_DIR)
	echo "obj-m := pipebuf.o" > $(BUILD_DIR)/Makefile
	ln -sf $(SRC_DIR)/pipebuf.c $(BUILD_DIR)/pipebuf.c
	$(MAKE) -C $(KDIR) M=$(BUILD_DIR) modules
	ln -sf $(BUILD_DIR)/pipebuf.ko $(CURDIR)/pipebuf.ko

clean:
	$(MAKE) -C $(KDIR) M=$(BUILD_DIR) clean || true
	rm -f $(CURDIR)/pipebuf.ko
	rm -rf $(BUILD_DIR)