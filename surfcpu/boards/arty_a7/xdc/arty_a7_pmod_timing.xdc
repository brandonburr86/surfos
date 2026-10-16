## SurfCPU on the Arty A7: I/O timing of the Pmod adapters (surfcpu_arty_a7_top only).
## docs/BOARD.md section 4 explains every line.

## Pmod JA, microSD in SPI mode 0. surf_spi drives SCLK, MOSI and CS from sys_clk registers;
## the card samples MOSI on SCLK's rising edge. Each output must settle within 20 ns of the
## sys_clk edge that launches it, so their mutual skew stays far below the shortest half SCLK
## period (one sys_clk cycle, 40 ns, at DIV = 0).
set_output_delay -clock [get_clocks sys_clk] -max 20.000 [get_ports {sd_sclk sd_mosi sd_cs_n}]
set_output_delay -clock [get_clocks sys_clk] -min 0.000  [get_ports {sd_sclk sd_mosi sd_cs_n}]
## MISO changes after SCLK falls and surf_spi takes it no earlier than the next rising SCLK edge,
## at least one sys_clk cycle later: no single-cycle relation exists, so it is not timed (the
## io track keeps that rule). Card detect is a mechanical switch; DAT1 and DAT2 are parked.
set_false_path -from [get_ports {sd_miso sd_cd_n sd_dat1 sd_dat2}]

## Pmod JB + JC, VGA. surf_video registers the colors and syncs on pix_clk; each must settle
## within 20 ns of its pixel clock edge (a pixel lasts 39.7 ns), so the monitor sees every
## pixel's color bits and the syncs change together.
set_output_delay -clock [get_clocks pix_clk] -max 20.000 [get_ports {vga_r[*] vga_g[*] vga_b[*] vga_hs vga_vs}]
set_output_delay -clock [get_clocks pix_clk] -min 0.000  [get_ports {vga_r[*] vga_g[*] vga_b[*] vga_hs vga_vs}]

## Pmod JD, PS/2: open drain at 10-16.7 kHz, clocked by the keyboard. surf_ps2 synchronizes
## both lines and changes its pull-downs only between the device's clock edges (microseconds).
set_false_path -from [get_ports {ps2_clk ps2_data ps2_clk2 ps2_data2}]
set_false_path -to [get_ports {ps2_clk ps2_data}]
