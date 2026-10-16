## SurfCPU on the Arty A7: I/O timing of the Pmod adapters (surfcpu_arty_a7_top only).
## docs/BOARD.md section 4 explains every line.

## Pmod JA, microSD in SPI mode 0 (docs/IO.md section 1). surf_spi drives SCLK and MOSI from
## sys_clk flops and samples MISO, which has no synchronizer, on the sys_clk edge that raises
## SCLK. At DIV = 0 a half period of SCLK is one sys_clk cycle (40 ns), so MISO, which the card
## changes after SCLK's falling edge, must come back within that cycle:
##   sys_clk edge to the SCLK pin                         <= 12 ns  (output delay -max 28 below)
##   SCLK and MISO through the board, both ways           <=  3 + 3 ns (traces, JA's 200 ohm, the
##                                                                     Pmod, about 10 pF each)
##   the card's output delay after SCLK falls (t_ODLY)    <= 14 ns  (SD spec, data transfer mode)
##   so MISO is at the FPGA pin                           <= 32 ns after the edge (input delay)
## and the pin-to-flop path (an IOB flop) has the rest of the cycle. The earliest MISO can
## change is SCLK's own clock-to-pin after the edge, well over 2 ns (input delay -min). MOSI
## changes with SCLK's falling edge and the card samples it at the next rising edge, 40 ns
## later, so the same 12 ns bound leaves it over 20 ns of setup (5 ns needed) and 40 ns of hold.
set_output_delay -clock [get_clocks sys_clk] -max 28.000 [get_ports {sd_sclk sd_mosi}]
set_output_delay -clock [get_clocks sys_clk] -min 0.000  [get_ports {sd_sclk sd_mosi}]
set_input_delay  -clock [get_clocks sys_clk] -max 32.000 [get_ports sd_miso]
set_input_delay  -clock [get_clocks sys_clk] -min 2.000  [get_ports sd_miso]
## CS changes only when software writes CTRL, between transfers, long before the next SCLK edge.
set_output_delay -clock [get_clocks sys_clk] -max 20.000 [get_ports sd_cs_n]
set_output_delay -clock [get_clocks sys_clk] -min 0.000  [get_ports sd_cs_n]
## Card detect is a mechanical switch (through surf_sync); DAT1 and DAT2 are parked.
set_false_path -from [get_ports {sd_cd_n sd_dat1 sd_dat2}]

## Pmod JB + JC, VGA. surf_video registers the colors and syncs on pix_clk; each must settle
## within 20 ns of its pixel clock edge (a pixel lasts 39.7 ns), so the monitor sees every
## pixel's color bits and the syncs change together.
set_output_delay -clock [get_clocks pix_clk] -max 20.000 [get_ports {vga_r[*] vga_g[*] vga_b[*] vga_hs vga_vs}]
set_output_delay -clock [get_clocks pix_clk] -min 0.000  [get_ports {vga_r[*] vga_g[*] vga_b[*] vga_hs vga_vs}]

## Pmod JD, PS/2: open drain at 10-16.7 kHz, clocked by the keyboard. surf_ps2 synchronizes
## both lines and changes its pull-downs only between the device's clock edges (microseconds).
set_false_path -from [get_ports {ps2_clk ps2_data ps2_clk2 ps2_data2}]
set_false_path -to [get_ports {ps2_clk ps2_data}]
