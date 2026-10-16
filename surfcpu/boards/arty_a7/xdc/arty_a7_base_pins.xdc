## SurfCPU on the Arty A7: the board's own pins (both tops: arty_a7_baseline and surfcpu_arty_a7_top).
## From Digilent's master XDC (boards/arty_a7/digilent/xdc/Arty-A7-100-Master.xdc, Rev. D/E; the
## A7-35 file has the same pins); every line is checked by tools/check-wiring.py against the
## master XDC files, the top-level ports and docs/WIRING.md section 2.1. Banks 14, 15, 16 and 35
## are at 3.3 V on the board, hence LVCMOS33.

## Clock and reset
set_property -dict { PACKAGE_PIN E3    IOSTANDARD LVCMOS33 } [get_ports { clk100 }];      # CLK100MHZ  IO_L12P_T1_MRCC_35
set_property -dict { PACKAGE_PIN C2    IOSTANDARD LVCMOS33 } [get_ports { ck_rst_n }];    # ck_rst     IO_L16P_T2_35

## Switches SW0..SW3
set_property -dict { PACKAGE_PIN A8    IOSTANDARD LVCMOS33 } [get_ports { sw[0] }];       # IO_L12N_T1_MRCC_16
set_property -dict { PACKAGE_PIN C11   IOSTANDARD LVCMOS33 } [get_ports { sw[1] }];       # IO_L13P_T2_MRCC_16
set_property -dict { PACKAGE_PIN C10   IOSTANDARD LVCMOS33 } [get_ports { sw[2] }];       # IO_L13N_T2_MRCC_16
set_property -dict { PACKAGE_PIN A10   IOSTANDARD LVCMOS33 } [get_ports { sw[3] }];       # IO_L14P_T2_SRCC_16

## Buttons BTN0..BTN3
set_property -dict { PACKAGE_PIN D9    IOSTANDARD LVCMOS33 } [get_ports { btn[0] }];      # IO_L6N_T0_VREF_16
set_property -dict { PACKAGE_PIN C9    IOSTANDARD LVCMOS33 } [get_ports { btn[1] }];      # IO_L11P_T1_SRCC_16
set_property -dict { PACKAGE_PIN B9    IOSTANDARD LVCMOS33 } [get_ports { btn[2] }];      # IO_L11N_T1_SRCC_16
set_property -dict { PACKAGE_PIN B8    IOSTANDARD LVCMOS33 } [get_ports { btn[3] }];      # IO_L12P_T1_MRCC_16

## Green LEDs LD4..LD7
set_property -dict { PACKAGE_PIN H5    IOSTANDARD LVCMOS33 } [get_ports { led[0] }];      # IO_L24N_T3_35
set_property -dict { PACKAGE_PIN J5    IOSTANDARD LVCMOS33 } [get_ports { led[1] }];      # IO_25_35
set_property -dict { PACKAGE_PIN T9    IOSTANDARD LVCMOS33 } [get_ports { led[2] }];      # IO_L24P_T3_A01_D17_14
set_property -dict { PACKAGE_PIN T10   IOSTANDARD LVCMOS33 } [get_ports { led[3] }];      # IO_L24N_T3_A00_D16_14

## RGB LEDs LD0..LD3: rgb[3n] red, rgb[3n+1] green, rgb[3n+2] blue
set_property -dict { PACKAGE_PIN G6    IOSTANDARD LVCMOS33 } [get_ports { rgb[0] }];      # led0_r  IO_L19P_T3_35
set_property -dict { PACKAGE_PIN F6    IOSTANDARD LVCMOS33 } [get_ports { rgb[1] }];      # led0_g  IO_L19N_T3_VREF_35
set_property -dict { PACKAGE_PIN E1    IOSTANDARD LVCMOS33 } [get_ports { rgb[2] }];      # led0_b  IO_L18N_T2_35
set_property -dict { PACKAGE_PIN G3    IOSTANDARD LVCMOS33 } [get_ports { rgb[3] }];      # led1_r  IO_L20N_T3_35
set_property -dict { PACKAGE_PIN J4    IOSTANDARD LVCMOS33 } [get_ports { rgb[4] }];      # led1_g  IO_L21P_T3_DQS_35
set_property -dict { PACKAGE_PIN G4    IOSTANDARD LVCMOS33 } [get_ports { rgb[5] }];      # led1_b  IO_L20P_T3_35
set_property -dict { PACKAGE_PIN J3    IOSTANDARD LVCMOS33 } [get_ports { rgb[6] }];      # led2_r  IO_L22P_T3_35
set_property -dict { PACKAGE_PIN J2    IOSTANDARD LVCMOS33 } [get_ports { rgb[7] }];      # led2_g  IO_L22N_T3_35
set_property -dict { PACKAGE_PIN H4    IOSTANDARD LVCMOS33 } [get_ports { rgb[8] }];      # led2_b  IO_L21N_T3_DQS_35
set_property -dict { PACKAGE_PIN K1    IOSTANDARD LVCMOS33 } [get_ports { rgb[9] }];      # led3_r  IO_L23N_T3_35
set_property -dict { PACKAGE_PIN H6    IOSTANDARD LVCMOS33 } [get_ports { rgb[10] }];     # led3_g  IO_L24P_T3_35
set_property -dict { PACKAGE_PIN K2    IOSTANDARD LVCMOS33 } [get_ports { rgb[11] }];     # led3_b  IO_L23P_T3_35

## USB-UART (FT2232HQ channel B)
set_property -dict { PACKAGE_PIN A9    IOSTANDARD LVCMOS33 } [get_ports { uart_rx }];     # uart_txd_in   IO_L14N_T2_SRCC_16
set_property -dict { PACKAGE_PIN D10   IOSTANDARD LVCMOS33 } [get_ports { uart_tx }];     # uart_rxd_out  IO_L19N_T3_VREF_16
