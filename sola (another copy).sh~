#!/bin/bash

cd /home/divaksh/android/sony/sony_kernel/cm-sony-kernel
export ARCH=arm
export CROSS_COMPILE=/home/divaksh/android/toolchains/arm-eabi-linaro-4.7/bin/arm-eabi-
make clean
make cm_sola_defconfig
make -j3

sh /home/divaksh/android/sony/sola/cm9/tools/makebootimg.sh
sh /home/divaksh/android/sony/sola/ics/tools/makebootimg.sh
