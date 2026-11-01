## SurfCPU on the Arty A7: the DDR3L pins (the DDR designs only: surfcpu_arty_a7_top with
## ARTY_HAS_DDR, docs/DDR.md). Every line is checked by tools/check-wiring.py against the top's
## ports, docs/WIRING.md section 2.5 and Digilent's MIG projects for both boards
## (ip/mig/digilent/arty-a7-{35,100}/mig.prj, their <PinSelection>; the two agree).
##
## Pins and I/O standards: Digilent's MIG projects (bank 34 at 1.35 V: SSTL135, differential
## pairs DIFF_SSTL135). Pin options: LiteX's Arty platform, which LiteDRAM's Arty example is run
## with (Migen's build/platforms/arty_a7.py at e19524c9, pinned by ip/litedram/setup.sh): fast
## slew on every pin, and the split termination UNTUNED_SPLIT_40 on the inputs (DQ, and DQS as
## LiteDRAM's own phy/rpc/arty.py does). Digilent's MIG project asks for 50 ohm instead
## (dci_hr_inouts_inputs; UNTUNED_SPLIT_50): change both IN_TERM groups together if the board
## calibrates better with it. The bank's VREF is internal (0.675 V, half of 1.35 V):
## arty_a7_ddr3_timing.xdc sets it for Vivado; the open flow's nextpnr sets it by itself
## whenever an SSTL135 input is used (docs/OPEN-FLOW.md section 6.7).
##
## Port names are the MIG's (ddr3_*), without its [0] on the one-bit signals (the open flow's
## LVS check cannot take [0:0] ports).

## Address, bank, commands, reset
set_property -dict { PACKAGE_PIN R2   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_addr[0] }];        # ddr3_addr[0]
set_property -dict { PACKAGE_PIN M6   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_addr[1] }];        # ddr3_addr[1]
set_property -dict { PACKAGE_PIN N4   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_addr[2] }];        # ddr3_addr[2]
set_property -dict { PACKAGE_PIN T1   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_addr[3] }];        # ddr3_addr[3]
set_property -dict { PACKAGE_PIN N6   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_addr[4] }];        # ddr3_addr[4]
set_property -dict { PACKAGE_PIN R7   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_addr[5] }];        # ddr3_addr[5]
set_property -dict { PACKAGE_PIN V6   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_addr[6] }];        # ddr3_addr[6]
set_property -dict { PACKAGE_PIN U7   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_addr[7] }];        # ddr3_addr[7]
set_property -dict { PACKAGE_PIN R8   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_addr[8] }];        # ddr3_addr[8]
set_property -dict { PACKAGE_PIN V7   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_addr[9] }];        # ddr3_addr[9]
set_property -dict { PACKAGE_PIN R6   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_addr[10] }];       # ddr3_addr[10]
set_property -dict { PACKAGE_PIN U6   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_addr[11] }];       # ddr3_addr[11]
set_property -dict { PACKAGE_PIN T6   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_addr[12] }];       # ddr3_addr[12]
set_property -dict { PACKAGE_PIN T8   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_addr[13] }];       # ddr3_addr[13]
set_property -dict { PACKAGE_PIN R1   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_ba[0] }];          # ddr3_ba[0]
set_property -dict { PACKAGE_PIN P4   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_ba[1] }];          # ddr3_ba[1]
set_property -dict { PACKAGE_PIN P2   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_ba[2] }];          # ddr3_ba[2]
set_property -dict { PACKAGE_PIN P3   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_ras_n }];          # ddr3_ras_n
set_property -dict { PACKAGE_PIN M4   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_cas_n }];          # ddr3_cas_n
set_property -dict { PACKAGE_PIN P5   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_we_n }];           # ddr3_we_n
set_property -dict { PACKAGE_PIN K6   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_reset_n }];        # ddr3_reset_n

## Clock enable, chip select, on-die termination, clock (a differential output)
set_property -dict { PACKAGE_PIN N5   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_cke }];            # ddr3_cke[0]
set_property -dict { PACKAGE_PIN U8   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_cs_n }];           # ddr3_cs_n[0]
set_property -dict { PACKAGE_PIN R5   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_odt }];            # ddr3_odt[0]
set_property -dict { PACKAGE_PIN U9   IOSTANDARD DIFF_SSTL135 SLEW FAST } [get_ports { ddr3_ck_p }];           # ddr3_ck_p[0]
set_property -dict { PACKAGE_PIN V9   IOSTANDARD DIFF_SSTL135 SLEW FAST } [get_ports { ddr3_ck_n }];           # ddr3_ck_n[0]

## Data masks, data, data strobes (differential, bidirectional)
set_property -dict { PACKAGE_PIN L1   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_dm[0] }];          # ddr3_dm[0]
set_property -dict { PACKAGE_PIN U1   IOSTANDARD SSTL135      SLEW FAST } [get_ports { ddr3_dm[1] }];          # ddr3_dm[1]
set_property -dict { PACKAGE_PIN K5   IOSTANDARD SSTL135      SLEW FAST IN_TERM UNTUNED_SPLIT_40 } [get_ports { ddr3_dq[0] }]; # ddr3_dq[0]
set_property -dict { PACKAGE_PIN L3   IOSTANDARD SSTL135      SLEW FAST IN_TERM UNTUNED_SPLIT_40 } [get_ports { ddr3_dq[1] }]; # ddr3_dq[1]
set_property -dict { PACKAGE_PIN K3   IOSTANDARD SSTL135      SLEW FAST IN_TERM UNTUNED_SPLIT_40 } [get_ports { ddr3_dq[2] }]; # ddr3_dq[2]
set_property -dict { PACKAGE_PIN L6   IOSTANDARD SSTL135      SLEW FAST IN_TERM UNTUNED_SPLIT_40 } [get_ports { ddr3_dq[3] }]; # ddr3_dq[3]
set_property -dict { PACKAGE_PIN M3   IOSTANDARD SSTL135      SLEW FAST IN_TERM UNTUNED_SPLIT_40 } [get_ports { ddr3_dq[4] }]; # ddr3_dq[4]
set_property -dict { PACKAGE_PIN M1   IOSTANDARD SSTL135      SLEW FAST IN_TERM UNTUNED_SPLIT_40 } [get_ports { ddr3_dq[5] }]; # ddr3_dq[5]
set_property -dict { PACKAGE_PIN L4   IOSTANDARD SSTL135      SLEW FAST IN_TERM UNTUNED_SPLIT_40 } [get_ports { ddr3_dq[6] }]; # ddr3_dq[6]
set_property -dict { PACKAGE_PIN M2   IOSTANDARD SSTL135      SLEW FAST IN_TERM UNTUNED_SPLIT_40 } [get_ports { ddr3_dq[7] }]; # ddr3_dq[7]
set_property -dict { PACKAGE_PIN V4   IOSTANDARD SSTL135      SLEW FAST IN_TERM UNTUNED_SPLIT_40 } [get_ports { ddr3_dq[8] }]; # ddr3_dq[8]
set_property -dict { PACKAGE_PIN T5   IOSTANDARD SSTL135      SLEW FAST IN_TERM UNTUNED_SPLIT_40 } [get_ports { ddr3_dq[9] }]; # ddr3_dq[9]
set_property -dict { PACKAGE_PIN U4   IOSTANDARD SSTL135      SLEW FAST IN_TERM UNTUNED_SPLIT_40 } [get_ports { ddr3_dq[10] }]; # ddr3_dq[10]
set_property -dict { PACKAGE_PIN V5   IOSTANDARD SSTL135      SLEW FAST IN_TERM UNTUNED_SPLIT_40 } [get_ports { ddr3_dq[11] }]; # ddr3_dq[11]
set_property -dict { PACKAGE_PIN V1   IOSTANDARD SSTL135      SLEW FAST IN_TERM UNTUNED_SPLIT_40 } [get_ports { ddr3_dq[12] }]; # ddr3_dq[12]
set_property -dict { PACKAGE_PIN T3   IOSTANDARD SSTL135      SLEW FAST IN_TERM UNTUNED_SPLIT_40 } [get_ports { ddr3_dq[13] }]; # ddr3_dq[13]
set_property -dict { PACKAGE_PIN U3   IOSTANDARD SSTL135      SLEW FAST IN_TERM UNTUNED_SPLIT_40 } [get_ports { ddr3_dq[14] }]; # ddr3_dq[14]
set_property -dict { PACKAGE_PIN R3   IOSTANDARD SSTL135      SLEW FAST IN_TERM UNTUNED_SPLIT_40 } [get_ports { ddr3_dq[15] }]; # ddr3_dq[15]
set_property -dict { PACKAGE_PIN N2   IOSTANDARD DIFF_SSTL135 SLEW FAST IN_TERM UNTUNED_SPLIT_40 } [get_ports { ddr3_dqs_p[0] }]; # ddr3_dqs_p[0]
set_property -dict { PACKAGE_PIN U2   IOSTANDARD DIFF_SSTL135 SLEW FAST IN_TERM UNTUNED_SPLIT_40 } [get_ports { ddr3_dqs_p[1] }]; # ddr3_dqs_p[1]
set_property -dict { PACKAGE_PIN N1   IOSTANDARD DIFF_SSTL135 SLEW FAST IN_TERM UNTUNED_SPLIT_40 } [get_ports { ddr3_dqs_n[0] }]; # ddr3_dqs_n[0]
set_property -dict { PACKAGE_PIN V2   IOSTANDARD DIFF_SSTL135 SLEW FAST IN_TERM UNTUNED_SPLIT_40 } [get_ports { ddr3_dqs_n[1] }]; # ddr3_dqs_n[1]
