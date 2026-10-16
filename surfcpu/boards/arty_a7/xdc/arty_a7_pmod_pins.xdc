## SurfCPU on the Arty A7: the Pmod adapters' pins (surfcpu_arty_a7_top only).
## microSD on JA, VGA on JB + JC, PS/2 on JD (the brief's provisional allocation). From
## Digilent's master XDC (boards/arty_a7/digilent/xdc/Arty-A7-100-Master.xdc); every line is
## checked by tools/check-wiring.py against the master XDC files, the top-level ports and
## docs/WIRING.md sections 2.2-2.4, which also list what is unverified.

## Pmod JA: Pmod MicroSD, SPI mode (JA has 200 ohm series resistors)
set_property -dict { PACKAGE_PIN G13   IOSTANDARD LVCMOS33 } [get_ports { sd_cs_n }];               # ja[0] J1-1 ~CS
set_property -dict { PACKAGE_PIN B11   IOSTANDARD LVCMOS33 } [get_ports { sd_mosi }];               # ja[1] J1-2 MOSI
set_property -dict { PACKAGE_PIN A11   IOSTANDARD LVCMOS33 PULLUP true } [get_ports { sd_miso }];   # ja[2] J1-3 MISO
set_property -dict { PACKAGE_PIN D12   IOSTANDARD LVCMOS33 } [get_ports { sd_sclk }];               # ja[3] J1-4 SCK
set_property -dict { PACKAGE_PIN D13   IOSTANDARD LVCMOS33 PULLUP true } [get_ports { sd_dat1 }];   # ja[4] J1-7 DAT1 (parked)
set_property -dict { PACKAGE_PIN B18   IOSTANDARD LVCMOS33 PULLUP true } [get_ports { sd_dat2 }];   # ja[5] J1-8 DAT2 (parked)
set_property -dict { PACKAGE_PIN A18   IOSTANDARD LVCMOS33 PULLUP true } [get_ports { sd_cd_n }];   # ja[6] J1-9 CD

## Pmod JB: Pmod VGA J1 (red, blue)
set_property -dict { PACKAGE_PIN E15   IOSTANDARD LVCMOS33 } [get_ports { vga_r[0] }];              # jb[0] J1-1 R0
set_property -dict { PACKAGE_PIN E16   IOSTANDARD LVCMOS33 } [get_ports { vga_r[1] }];              # jb[1] J1-2 R1
set_property -dict { PACKAGE_PIN D15   IOSTANDARD LVCMOS33 } [get_ports { vga_r[2] }];              # jb[2] J1-3 R2
set_property -dict { PACKAGE_PIN C15   IOSTANDARD LVCMOS33 } [get_ports { vga_r[3] }];              # jb[3] J1-4 R3
set_property -dict { PACKAGE_PIN J17   IOSTANDARD LVCMOS33 } [get_ports { vga_b[0] }];              # jb[4] J1-7 B0
set_property -dict { PACKAGE_PIN J18   IOSTANDARD LVCMOS33 } [get_ports { vga_b[1] }];              # jb[5] J1-8 B1
set_property -dict { PACKAGE_PIN K15   IOSTANDARD LVCMOS33 } [get_ports { vga_b[2] }];              # jb[6] J1-9 B2
set_property -dict { PACKAGE_PIN J15   IOSTANDARD LVCMOS33 } [get_ports { vga_b[3] }];              # jb[7] J1-10 B3

## Pmod JC: Pmod VGA J2 (green, syncs)
set_property -dict { PACKAGE_PIN U12   IOSTANDARD LVCMOS33 } [get_ports { vga_g[0] }];              # jc[0] J2-1 G0
set_property -dict { PACKAGE_PIN V12   IOSTANDARD LVCMOS33 } [get_ports { vga_g[1] }];              # jc[1] J2-2 G1
set_property -dict { PACKAGE_PIN V10   IOSTANDARD LVCMOS33 } [get_ports { vga_g[2] }];              # jc[2] J2-3 G2
set_property -dict { PACKAGE_PIN V11   IOSTANDARD LVCMOS33 } [get_ports { vga_g[3] }];              # jc[3] J2-4 G3
set_property -dict { PACKAGE_PIN U14   IOSTANDARD LVCMOS33 } [get_ports { vga_hs }];                # jc[4] J2-7 HS
set_property -dict { PACKAGE_PIN V14   IOSTANDARD LVCMOS33 } [get_ports { vga_vs }];                # jc[5] J2-8 VS

## Pmod JD: Pmod PS2 in the top row (open drain through IOBUFs; JD has 200 ohm series resistors)
set_property -dict { PACKAGE_PIN D4    IOSTANDARD LVCMOS33 PULLUP true DRIVE 4 } [get_ports { ps2_data }];   # jd[0] J2-1 DATA
set_property -dict { PACKAGE_PIN D3    IOSTANDARD LVCMOS33 PULLUP true } [get_ports { ps2_data2 }];          # jd[1] J2-2 DATA- (parked)
set_property -dict { PACKAGE_PIN F4    IOSTANDARD LVCMOS33 PULLUP true DRIVE 4 } [get_ports { ps2_clk }];    # jd[2] J2-3 CLK
set_property -dict { PACKAGE_PIN F3    IOSTANDARD LVCMOS33 PULLUP true } [get_ports { ps2_clk2 }];           # jd[3] J2-4 CLK- (parked)
