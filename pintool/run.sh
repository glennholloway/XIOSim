#! /bin/csh -f

set PROGRAM = ../tests/step
set PINHOME = /home/skanev/pin/2.12
set PIN = "$PINHOME/pin.sh"
set PINTOOL = ./obj-ia32/feeder_zesto.so
set ZESTOCFG = ../config/A.cfg
set MEMCFG = ../dram-config/DDR3-1600-9-9-9.cfg

setenv LD_LIBRARY_PATH "/home/skanev/lib"

set CMD_LINE = "setarch i686 -3BL $PIN -pause_tool 1 -injection child -xyzzy -t $PINTOOL -sanity -pipeline_instrumentation -s -config $ZESTOCFG -config $MEMCFG -redir:sim tst.out -- $PROGRAM"
echo $CMD_LINE
$CMD_LINE
