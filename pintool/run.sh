#! /bin/csh -f

set PROGRAM = ../tests/fib
#set PROGRAM = ../tests/fork
#set PROGRAM = /home/skanev/ubench/fib
#set PROGRAM = /home/skanev/cpuburn/burnP5
#set PROGRAM = /home/skanev/misc/tests/st_test/st_test
#set PROGRAM = /home/skanev/pfmwrapper/main
#set PINHOME = /home/skanev/pin/pin-2.10-45467-gcc.3.4.6-ia32_intel64-linux
#set PIN = "$PINHOME/ia32/bin/pinbin"
set PINHOME = /home/skanev/pin/2.12
set PIN = "$PINHOME/pin.sh"
set PINTOOL = ./obj-ia32/feeder_zesto.so
#set ZESTOCFG = ../config/A.cfg
#set MEMCFG = ../dram-config/DDR2-800-5-5-5.cfg
set ZESTOCFG = ../config/N.cfg
set MEMCFG = ../dram-config/DDR3-1600-9-9-9.cfg
set MAX = 1000000
#set MAX = -1

setenv LD_LIBRARY_PATH "/home/skanev/lib"

set CMD_LINE = "setarch i686 -3BL $PIN -pause_tool 1 -injection child -xyzzy -t $PINTOOL -sanity -pipeline_instrumentation -s -config $ZESTOCFG -config $MEMCFG -redir:sim tst.out -- $PROGRAM"
echo $CMD_LINE
$CMD_LINE

#/usr/bin/env -i setarch i686 -3BL /home/skanev/pin/pin-2.8-36111-gcc.3.4.6-ia32_intel64-linux/ia32/bin/pinbin -pause_tool 1 -separate_memory -t /home/skanev/Zesto/pintool/obj-ia32/feeder_zesto.so -ppfile fib..pintool.1.pp -s -config /home/skanev/zesto/config/A.cfg -config /home/skanev/zesto/dram-config/DDR2-533-4-4-4.cfg -redir:sim tst.out -- /home/skanev/ubench/fib
