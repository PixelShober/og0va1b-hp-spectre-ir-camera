KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

obj-m := og0va1b.o ipu-bridge.o

# Detect if kernel was built with clang by checking CC_VERSION_TEXT
CLANG_KERNEL := $(shell grep -l "clang" $(KDIR)/include/generated/compile.h 2>/dev/null | wc -l)
ifeq ($(CLANG_KERNEL), 1)
    BUILD_FLAGS := LLVM=1
else
    BUILD_FLAGS :=
endif

all:
	$(MAKE) -C $(KDIR) M=$(PWD) $(BUILD_FLAGS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) $(BUILD_FLAGS) clean
