KDIR := /lib/modules/$(shell uname -r)/build
MODULE_NAME := backdoor
BUILD_DIR := $(CURDIR)/build
SRC_DIR := $(CURDIR)/src

all:
	mkdir -p $(BUILD_DIR)
	echo "obj-m := $(MODULE_NAME).o" > $(BUILD_DIR)/Makefile
	ln -sf $(SRC_DIR)/$(MODULE_NAME).c $(BUILD_DIR)/$(MODULE_NAME).c
	$(MAKE) -C $(KDIR) M=$(BUILD_DIR) modules
	ln -sf $(BUILD_DIR)/$(MODULE_NAME).ko $(CURDIR)/$(MODULE_NAME).ko

clean:
	$(MAKE) -C $(KDIR) M=$(BUILD_DIR) clean || true
	rm -f $(CURDIR)/$(MODULE_NAME).ko
	rm -rf $(BUILD_DIR)