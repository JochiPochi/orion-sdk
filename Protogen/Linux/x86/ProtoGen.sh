#!/bin/sh
appname=`basename $0 | sed s,\.sh$,,`

dirname=`dirname $0`
tmp="${dirname#?}"

if [ "${dirname%$tmp}" != "/" ]; then
dirname=$PWD/$dirname
fi
LD_LIBRARY_PATH=$dirname:/usr/lib/i386-linux-gnu/:$LD_LIBRARY_PATH
export LD_LIBRARY_PATH
$dirname/$appname "$@"
