/*
SurfCPU: LiteDRAM's register model for the C++ harnesses
--------------------
File: ddrc_model_glue.cpp Date: 10/16/26 (SurfCPU)
--------------------
The firmware tests' model of LiteDRAM behind the ddrc registers (firmware/tests/ddrc_model.h:
the controller that software initializes and levels, with a read window per lane), compiled
into the Verilator harnesses (surfsim's --ddr-swinit) as it is: one model for the firmware's
host tests, its emulator and the SoC simulation. Verilator's build compiles only C++ sources,
so the C file is included here; it is also valid C++.
*/
#include "../../firmware/tests/ddrc_model.c"
