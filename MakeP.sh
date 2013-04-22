#!/bin/bash

cd /home/divaksh/android/sony/sony_kernel/kernel
export ARCH=arm
export CROSS_COMPILE=/home/divaksh/android/toolchains/arm-eabi-linaro-4.7/bin/arm-eabi-
make clean
make cm_p_defconfig
make -j3

sh /home/divaksh/android/sony/p/cm9/tools/makebootimg.sh
#sh /home/divaksh/android/sony/p/ics/tools/makebootimg.sh
