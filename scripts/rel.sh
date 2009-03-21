#!/bin/sh

AUTHOR=hrimfaxi
EMAIL=outmatch@gmail.com
DIRS="msg fonts"
SRCDIR=$HOME/xreader/src
ROOTDIR=$HOME/xreader/PSP/
DESTDIR=$HOME/xreader/PSP/PSP/GAME/xReader
DESTDIR2=$HOME/xreader/PSP/seplugins
LITE=n

echo "xReader transfer script"
echo "Author: $AUTHOR($EMAIL)"

rm -rf $ROOTDIR
rm -rf $DESTDIR
rm -rf $DESTDIR2

mkdir -p $ROOTDIR
mkdir -p $DESTDIR
mkdir -p $DESTDIR2

for dir in $DIRS
do
	echo "\tCreate dir: $DESTDIR/$dir"
	mkdir -p "$DESTDIR/$dir"
done

echo "OK, now we copy files"

echo "\tCopy files"
echo "\t\tEBOOT.PBP"
cp -u "$SRCDIR/EBOOT.PBP" "$DESTDIR/"
echo "\t\txrPrx.prx"
cp -u "$SRCDIR/../xrPrx/xrPrx.prx" "$DESTDIR/"
echo "\t\txr_rdriver.prx"
cp -u "$SRCDIR/../xr_rdriver/xr_rdriver.prx" "$DESTDIR2/"
echo "\t\tfonts.zip"
cp -u "$SRCDIR/../fonts/fonts.zip" "$DESTDIR/"
echo "\t\tReadme.txt"
cp -u "$SRCDIR/../Readme.txt" "$DESTDIR/"
echo "\t\tChangelog.txt"
cp -u "$SRCDIR/../Changelog.txt" "$DESTDIR/"
if [ x$LITE != xy ]; then
echo "\t\tbg.png"
cp -u "$SRCDIR/../resource/bg.png" "$DESTDIR/"
fi
echo "\t\tmsg/zh_CN.so"
cp -u "$SRCDIR/../msg/zh_CN.so" "$DESTDIR/msg"
echo "\t\tmsg/zh_TW.so"
cp -u "$SRCDIR/../msg/zh_TW.so" "$DESTDIR/msg"
echo "\t\tmsg/en_US.so"
cp -u "$SRCDIR/../msg/en_US.so" "$DESTDIR/msg"

cd "$ROOTDIR"
rar a -v1500k xReader.rar *

