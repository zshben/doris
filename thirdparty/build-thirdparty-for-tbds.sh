#!/usr/bin/env bash
# shellcheck disable=2034

# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

#################################################################################
# This script will
# 1. Check prerequisite libraries. Including:
#    cmake byacc flex automake libtool binutils-dev libiberty-dev bison
# 2. Compile and install all thirdparties which are downloaded
#    using *download-thirdparty.sh*.
#
# This script will run *download-thirdparty.sh* once again
# to check if all thirdparties have been downloaded, unpacked and patched.
#################################################################################

set -eo pipefail

curdir="$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"

export DORIS_HOME="${curdir}/.."
export TP_DIR="${curdir}"

DOWNLOAD_LINK=https://tencent-tbds-1308700295.cos.ap-beijing.myqcloud.com

TBDS_HADOOP=hadoop-2.2.0.0-2041-tbds-5.2.0.1.tar.xz
THIRDPARTY_INSTALLED=installed-tbds-v20230909.tar.xz
HIVE_SHADE=hive-catalog-shade-1.0.0-TBDS-SNAPSHOT.jar
HADOOP_SHADE=tbds-hadoop-shade-1.0.1-SNAPSHOT.jar
LIBZ_O3=libz.a

DOWNLOAD_DIR="${TP_DIR}/download_tbds"
rm -rf "${DOWNLOAD_DIR}"
mkdir -p "${DOWNLOAD_DIR}"

wget --no-check-certificate "${DOWNLOAD_LINK}/${THIRDPARTY_INSTALLED}" -O "${DOWNLOAD_DIR}/${THIRDPARTY_INSTALLED}"
wget --no-check-certificate "${DOWNLOAD_LINK}/${TBDS_HADOOP}" -O "${DOWNLOAD_DIR}/${TBDS_HADOOP}"
wget --no-check-certificate "${DOWNLOAD_LINK}/${HIVE_SHADE}" -O "${DOWNLOAD_DIR}/${HIVE_SHADE}"
wget --no-check-certificate "${DOWNLOAD_LINK}/${HADOOP_SHADE}" -O "${DOWNLOAD_DIR}/${HADOOP_SHADE}"
wget --no-check-certificate "${DOWNLOAD_LINK}/${LIBZ_O3}" -O "${DOWNLOAD_DIR}/${LIBZ_O3}"

echo "Install thirdparties..."
rm -rf "${TP_DIR}/installed/"
cd ${DOWNLOAD_DIR} && tar xf "${THIRDPARTY_INSTALLED}"
mv "${DOWNLOAD_DIR}/installed-tbds-v20230909" "${TP_DIR}/installed"

echo "Install tbds hadoop..."
rm -rf "${TP_DIR}/hadoop_tbds/"
cd ${DOWNLOAD_DIR} && tar xf "${TBDS_HADOOP}"
mv "${DOWNLOAD_DIR}/hadoop" "${TP_DIR}/hadoop_tbds"

echo "Install jars..."
mvn install:install-file -Dpackaging=jar -Dfile=${DOWNLOAD_DIR}/${HADOOP_SHADE} -DgroupId=org.apache.doris -DartifactId=tbds-hadoop-shade -Dversion=1.0.1-SNAPSHOT
mvn install:install-file -Dpackaging=jar -Dfile=${DOWNLOAD_DIR}/${HIVE_SHADE} -DgroupId=org.apache.doris -DartifactId=tbds-hive-shade -Dversion=1.0.1-SNAPSHOT

echo "Finished"
