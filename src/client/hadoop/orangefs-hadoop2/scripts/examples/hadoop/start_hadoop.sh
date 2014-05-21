#!/bin/bash
set -x
cd $(dirname $0)

HADOOP_PREFIX=/opt/hadoop-2.2.0
HADOOP_CONFIG_DIR=/home/denton/projects/denton.hadoop2.trunk/src/client/hadoop/orangefs-hadoop2/src/main/resources/conf

# START
$HADOOP_PREFIX/sbin/yarn-daemon.sh --config ${HADOOP_CONFIG_DIR} start resourcemanager
$HADOOP_PREFIX/sbin/yarn-daemon.sh --config ${HADOOP_CONFIG_DIR} start nodemanager
$HADOOP_PREFIX/sbin/yarn-daemon.sh --config ${HADOOP_CONFIG_DIR} start proxyserver
$HADOOP_PREFIX/sbin/mr-jobhistory-daemon.sh --config ${HADOOP_CONFIG_DIR} start historyserver
echo "Visit http://localhost:8088"
