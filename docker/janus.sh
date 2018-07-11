cd /janus-gateway

make clean
sh autogen.sh
./configure --prefix=/opt/janus --disable-rabbitmq --disable-mqtt --enable-docs
make
make install
