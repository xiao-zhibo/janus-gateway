## 安装 ffmpeg 支持
```
# mp3编码库
sudo apt-get install libmp3lame-dev
# h264编码库
sudo apt-get install libx264-dev
# 编译 ffmpeg
./configure --enable-version3 --enable-nonfree --enable-gpl --enable-libmp3lame --enable-libx264 --enable-postproc --enable-pthreads --enable-shared --disable-asm
make
make install
```