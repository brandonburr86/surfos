## SurfCPU on the Arty A7: device configuration properties (both tops).

## Configuration bank 0 is powered from the board's 3.3 V and CFGBVS is tied to it through
## 4.7 kOhm (Arty A7 schematic Rev. E.0, sheets 6 and 7).
set_property CFGBVS VCCO [current_design]
set_property CONFIG_VOLTAGE 3.3 [current_design]

## A compressed bitstream loads faster over JTAG. The quad-SPI flash settings (bus width,
## configuration rate) come with milestone 9, once the board's flash part is known
## (INVENTORY.md G7).
set_property BITSTREAM.GENERAL.COMPRESS TRUE [current_design]
