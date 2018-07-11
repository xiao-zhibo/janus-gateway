cd ~
if [ ! -d "usrsctp" ]; then
	git clone https://github.com/sctplab/usrsctp
fi
cd usrsctp
./bootstrap
./configure --prefix=/usr
make
sudo make install