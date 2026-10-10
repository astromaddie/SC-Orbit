# Host tests need only a C compiler; production has no libc/runtime dependency.
CC ?= cc
ARM_CC ?= arm-linux-gnueabihf-gcc
ARM_READELF ?= arm-linux-gnueabihf-readelf
ARM_NM ?= arm-linux-gnueabihf-nm
ARM_FLAGS = -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -marm -march=armv7-a -mfloat-abi=soft -nostdlib -static -Wl,-e,_start

.PHONY: test test-sanitize arm verify-arm package clean

test: obj/test-driver
	./obj/test-driver

obj/test-driver: tests/test-driver.c SC-Orbit-driver.c
	mkdir -p obj
	$(CC) -std=c99 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-misleading-indentation $(TEST_FLAGS) tests/test-driver.c -o $@

test-sanitize:
	mkdir -p obj
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all tests/test-driver.c -o obj/test-driver-sanitized
	./obj/test-driver-sanitized

arm: obj/SC-Orbit-driver.armv7l

verify-arm: obj/SC-Orbit-driver.armv7l
	@$(ARM_READELF) -h $< | grep -q 'Machine:.*ARM'
	@$(ARM_READELF) -h $< | grep -q 'soft-float ABI'
	@! $(ARM_READELF) -l $< | grep -q INTERP
	@test -z "$$($(ARM_NM) -u $<)"
	@echo "verified static ARMv7 EABI5 soft-float executable with no undefined symbols"

package: arm verify-arm
	python3 tools/package.py --binary obj/SC-Orbit-driver.armv7l

obj/SC-Orbit-driver.armv7l: SC-Orbit-driver.c
	mkdir -p obj
	$(ARM_CC) $(ARM_FLAGS) $< -o $@

clean:
	rm -f obj/test-driver obj/test-driver-sanitized obj/SC-Orbit-driver.armv7l
