cd ~
if [ ! -d "protobuf" ]; then
	git clone https://github.com/google/protobuf.git
fi
cd protobuf
git checkout v3.4.0

sh autogen.sh
./configure --prefix=/usr
make
make check
sudo make install
sudo ldconfig # refresh shared library cache.

cd ~
if [ ! -d "protobuf-c" ]; then
	git clone https://github.com/protobuf-c/protobuf-c.git
fi
cd protobuf-c
git pull

sh autogen.sh
./configure --prefix=/usr --enable-shared
make
sudo make install