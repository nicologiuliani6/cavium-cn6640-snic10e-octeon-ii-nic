# Card-side modules — cross-built against the OpenWrt Octeon kernel.
# See docs/FLASHING.md for the cross-compile invocation (needs the OpenWrt toolchain).
#   octshm_card : shared-memory NIC datapath (card end of oct0/oct1)
#   octcarrier  : un-gates xaui0/xaui1 TX on the QLogic DAC (cvmx_helper_link_set)
obj-m += octshm_card.o
obj-m += octcarrier.o
obj-m += octnpi.o
