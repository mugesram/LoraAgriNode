# STM32L432 LoRa sensor logger firmware
#   make          build build/logger.bin
#   make flash    put the board in DFU mode and write the firmware (tools/flash.py)
#   make test     run the storage tests on the PC
# Power-supply test builds (App/Inc/powertest.h), each in its own folder:
#   make TEST=lora|flash|sensor|cycle [flash]    build_test_<name>/test_<name>.bin
TARGET  = logger
B       = build
TESTNUM_lora   = 1
TESTNUM_flash  = 2
TESTNUM_sensor = 3
TESTNUM_cycle  = 4
ifdef TEST
ifeq ($(TESTNUM_$(TEST)),)
$(error TEST must be lora, flash, sensor or cycle)
endif
TARGET  = test_$(TEST)
B       = build_test_$(TEST)
TESTFLAGS = -DPOWER_TEST=$(TESTNUM_$(TEST)) -DPT_NAME=\"$(TEST)\"
endif
CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

HAL  = Drivers/STM32L4xx_HAL_Driver
TUSB = Middlewares/Third_Party/tinyusb/src
SX   = Middlewares/Third_Party/sx126x_driver/src

SRCS = $(wildcard Core/Src/*.c) $(wildcard App/Src/*.c) $(wildcard Drivers/BSP/*.c) \
       $(addprefix $(HAL)/Src/stm32l4xx_hal, .c _cortex.c _rcc.c _rcc_ex.c _gpio.c _pwr.c _pwr_ex.c \
         _spi.c _spi_ex.c _adc.c _adc_ex.c _rtc.c _rtc_ex.c _iwdg.c _tim.c _tim_ex.c _flash.c _flash_ex.c _exti.c _dma.c) \
       $(SX)/sx126x.c \
       $(TUSB)/tusb.c $(TUSB)/common/tusb_fifo.c $(TUSB)/device/usbd.c $(TUSB)/class/cdc/cdc_device.c \
       $(TUSB)/portable/st/stm32_fsdev/dcd_stm32_fsdev.c $(TUSB)/portable/st/stm32_fsdev/fsdev_common.c
ASRCS = Core/Startup/startup_stm32l432xx.s

INCS = -ICore/Inc -IApp/Inc -IDrivers/BSP -I$(HAL)/Inc -IDrivers/CMSIS/Include \
       -IDrivers/CMSIS/Device/ST/STM32L4xx/Include -I$(SX) -I$(TUSB)

MCU     = -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
CFLAGS  = $(MCU) -Os -g -Wall -ffunction-sections -fdata-sections -DSTM32L432xx -DUSE_HAL_DRIVER $(INCS) $(TESTFLAGS) $(EXTRA)
LDFLAGS = $(MCU) -TSTM32L432KCUx_FLASH.ld -Wl,--gc-sections --specs=nano.specs --specs=nosys.specs \
          -Wl,-Map=$(B)/$(TARGET).map

OBJS = $(addprefix $(B)/,$(notdir $(SRCS:.c=.o) $(ASRCS:.s=.o)))
vpath %.c $(sort $(dir $(SRCS)))
vpath %.s $(sort $(dir $(ASRCS)))

all: $(B)/$(TARGET).bin

$(B)/%.o: %.c | $(B)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(B)/%.o: %.s | $(B)
	$(CC) $(MCU) -c $< -o $@
$(B)/$(TARGET).elf: $(OBJS) STM32L432KCUx_FLASH.ld
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@
$(B)/$(TARGET).bin: $(B)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@
$(B):
	mkdir -p $(B)

# storage code against a simulated flash, small ring and full-size ring
test:
	@mkdir -p build
	gcc -Wall -Wextra -O1 -DLOG_SECTORS=4u -IApp/Inc -IDrivers/BSP tests/test_storage.c App/Src/datalog.c App/Src/config.c -o build/test_storage_small
	gcc -Wall -Wextra -O1 -IApp/Inc -IDrivers/BSP tests/test_storage.c App/Src/datalog.c App/Src/config.c -o build/test_storage_full
	build/test_storage_small && build/test_storage_full

flash: $(B)/$(TARGET).bin
	../.venv/bin/python tools/flash.py $<

clean:
	rm -rf build build_test_*
-include $(wildcard $(B)/*.d)
.PHONY: all test flash clean
