# SPDX-License-Identifier: GPL-2.0
# make KDIR=/path/to/linux-headers/build
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules $(EXTRA)

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
