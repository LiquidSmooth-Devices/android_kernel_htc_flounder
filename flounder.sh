#!/bin/bash

# Edit these for your folder setup
SOURCE="/home/teamliquid/Sean/Flounder/Kernel"
UTILS="/home/teamliquid/Sean/Flounder/utils"
HOSTING="/www/devs/teamliquid/Kernels/flounder/"
ANYKERNEL="/home/teamliquid/Sean/Flounder/Kernel/deathly_bootimg/deathly_anykernel_zip"

# Change the ARCH to arm if you are not building 64-Bit
export ARCH=arm64

# Change to wherever your toolchain is
export CROSS_COMPILE=/home/teamliquid/Sean/Liquid-5.0/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin/aarch64-linux-android-

# Date Info (Do not change)
export curdate=`date "+%m-%d-%Y"`

# Start Time
res1=$(date +%s.%N)

# Colorize and add text parameters (Not all are used)
red=$(tput setaf 1) # red
grn=$(tput setaf 2) # green
cya=$(tput setaf 6) # cyan
txtbld=$(tput bold) # Bold
bldred=${txtbld}$(tput setaf 1) # red
bldgrn=${txtbld}$(tput setaf 2) # green
bldblu=${txtbld}$(tput setaf 4) # blue
bldcya=${txtbld}$(tput setaf 6) # cyan
txtrst=$(tput sgr0) # Reset

echo -e "${bldred} Set CCACHE ${txtrst}"
ccache -M50

echo ""

echo -e "${bldred} Removing old zImages ${txtrst}"
rm -f $SOURCE/arch/$ARCH/boot/Image.gz-dtb
rm -f $ANYKERNEL/Image.gz-dtb

echo ""

echo -e "${bldred} Clean up from prior build ${txtrst}"
cd $SOURCE
make mrproper

echo ""

echo -e "${bldred} Use Defconfig Settings ${txtrst}"

cp arch/$ARCH/configs/deathly_flounder_defconfig .config

echo ""

echo -e "${bldred} Compiling zImage.. ${txtrst}"
script -q ~/Compile.log -c " make -j12 "
cp arch/$ARCH/boot/Image.gz-dtb $ANYKERNEL

echo ""

echo -e "${bldblu} Creating zip.. ${txtrst}"
cd $ANYKERNEL
zip -r9 Deathly_Flounder.zip * -x README UPDATE-AnyKernel2.zip

echo " "

echo "${bldblu}Signing Zip ${txtrst}"
java -jar $UTILS/signapk.jar $UTILS/testkey.x509.pem $UTILS/testkey.pk8 Deathly_Flounder.zip Deathly-5.0.2-flounder-$curdate.zip
rm Deathly_Flounder.zip

echo ""

echo "${bldblu}Uploading to DrDevs ${txtrst}"
mv Deathly-5.0.2-flounder-* $HOSTING

echo -e "${bldgrn} Done! ${txtrst}"
cd $source
echo ""

# Show Elapsed Time
res2=$(date +%s.%N)
echo "${bldgrn}Total elapsed time: ${txtrst}${grn}$(echo "($res2 - $res1) / 60"|bc ) minutes ($(echo "$res2 - $res1"|bc ) seconds) ${txtrst}"
