#!/bin/sh
# tst.sh

./bld.sh; if [ $? -ne 0 ]; then exit 1; fi

rm -f tst.x tst.tmp tst.log

cat >tst.x <<__EOF__
ihello:wq
__EOF__
./minpty vi tst.tmp <tst.x >tst.log

T="`cat tst.tmp`"
if [ "$T" != "hello" ]; then echo "ERROR"; exit 1; fi

exit
