echo "============== oss c sdk ===================="

sudo apt-get -y install cmake
sudo apt-get -y install libcurl4-openssl-dev libapr1-dev libaprutil1-dev libmxml-dev

# cd ~ 
# if [ ! -d "curl" ]; then
# 	git clone https://github.com/curl/curl.git
# fi
# cd curl
# ./configure
# make
# sudo make install

cd ~
if [ ! -d "aliyun-oss-c-sdk" ]; then
	git clone https://github.com/aliyun/aliyun-oss-c-sdk.git
fi

cd aliyun-oss-c-sdk
git checkout 3.5.0

cmake .
make
sudo make install
