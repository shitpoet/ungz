# ungz

This is a little modified source code from an excellent article ["Dissecting the GZIP format"](http://commandlinefanatic.com/cgi-bin/showarticle.cgi?article=art001) by Joshua Davies.

## Modifications

Modifications include:

* Convert to C99 and apply more compact code style
* Fix one bug related to building of dynamic Huffman trees
* Add workaround to support files greater than 32k

## Difference from gunzip

gunzip handles various compatibility issues, does compression, is written in old school C (and sometimes in asm!) and optimized. All this compilcates its source code. For example, it builds and uses multi-level lookup tables where ungz just traverse binary tree in several lines of code.

## Tests

Tested with this bash command

    find / -maxdepth 6 -name "*.gz" -size 1M -readable -exec test.sh {} \;  2>&1 | grep -v "Permission denied"

and this script

    #!/bin/bash
    ungz "$*" > /tmp/ungz.out
    gunzip "$*" -c > /tmp/gunzip.out
    DIFF=$(diff /tmp/ungz.out /tmp/gunzip.out)
    if [ "$DIFF" != "" ]
    then
        echo "----------------------------------------"
        echo "$*"
        diff /tmp/ungz.out /tmp/gunzip.out | head -n 10
    fi

on about 10 000 files.
