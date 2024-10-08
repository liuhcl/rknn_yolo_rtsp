set -e

GCC_COMPILER=aarch64-linux-gnu

export LD_LIBRARY_PATH=/home/cat/opt/gcc-9.3.0-x86_64_aarch64-linux-gnu/lib64:$LD_LIBRARY_PATH
export CC=${GCC_COMPILER}-gcc
export CXX=${GCC_COMPILER}-g++

ROOT_PWD=$( cd "$( dirname $0 )" && cd -P "$( dirname "$SOURCE" )" && pwd )

# build
BUILD_DIR=${ROOT_PWD}/build

if [[ ! -d "${BUILD_DIR}" ]]; then
  mkdir -p ${BUILD_DIR}
fi

cd ${BUILD_DIR}
cmake ../ -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_BUILD_TYPE=Debug
make -j4
cd -
