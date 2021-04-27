#!/bin/bash
dd if=/dev/ttyUSB1 bs=14 iflag=fullblock count=200 of=test.dat

