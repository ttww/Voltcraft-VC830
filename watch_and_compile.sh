#!/bin/bash
#
# Simple script to automatically compile and test the code after each editor save.
#

# Device with RS232 connection to VC830
dev=/dev/ttyUSB1
dev=test.dat

ARCH=`uname -m`
EXE=vc830.$ARCH

while :
do
	fswatch -1 vc830.c

	clear
	echo "Compile..."
	make

	if [ $? == 0 ] ; then
		
		ls -l $EXE

		if [ -e $dev ] ; then
			./$EXE -c 1 -f keyvalue $dev
			./$EXE -c 1 -f json $dev
			./$EXE -c 1 -f human $dev
			./$EXE -c 1 -f si $dev
			./$EXE -c 1 -f human -t iso $dev
			./$EXE -c 1 -f human -t local $dev
			./$EXE -c 1 -f human -t epochsecms $dev
			./$EXE -c 1 -f human -t human $dev
		fi
	fi
	echo "done."

	# Allows "better" ctrl-c...:
	sleep 1
done


