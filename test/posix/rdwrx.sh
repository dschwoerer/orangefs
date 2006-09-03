#!/bin/bash

STREAMS="1 2 4 8 16";
MODE="0 1 2";
BLKSIZE="1048576 2097152 4194304 8388608 12582912 16777216 33554432";
E_OPTERR=65

usage()
{
	echo "Usage: `basename $0` -f <machine file> -m <MPICH2 install path> -p <PVFS2 posix extensions path> -d <PVFS2 mount point>"
 	exit $E_OPTERR;
}

while getopts "p:d:m:f:" Option
do
	case $Option in
		f) MACHINE_FILE=$OPTARG;;
		m) MPICH2_INSTALL=$OPTARG;;
		p) PVFS2_TOPDIR=$OPTARG;;
		d) PVFS2_MNT=$OPTARG;;
		*) usage;;	
	esac
done

if [ -z "$PVFS2_TOPDIR" -o -z "$PVFS2_MNT" ]
then
	echo "Please specify PVFS2 top dir and mount point";
	usage;
fi

	for m in $MODE
	do
		for s in $STREAMS
		do
			for b in $BLKSIZE
			do
				echo "$MPICH2_INSTALL/bin/mpiexec -machinefile $MACHINE_FILE -n 90 $PVFS2_TOPDIR/test/posix/iox -f $PVFS2_MNT/rdwrx1 -n 10 -m $m -u -b $b -s $s"
				$MPICH2_INSTALL/bin/mpiexec -machinefile $MACHINE_FILE -n 90 $PVFS2_TOPDIR/test/posix/iox -f $PVFS2_MNT/rdwrx1 -n 10 -m $m -u -b $b -s $s
			done
		done
	done
