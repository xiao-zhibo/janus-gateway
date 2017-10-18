
##  安装基础环境
``` brew update
    #可能会遇到E rror: /usr/local is not writable. 运行命令即可 `sudo chown -R $(whoami) /usr/local`
    brew tap homebrew/boneyard
    brew install jansson libnice openssl libusrsctp libmicrohttpd libwebsockets cmake rabbitmq-c sofia-sip opus libogg glib pkg-config gengetopt autoconf automake libtool
    #安装boringssl可能更好？
```

##  编译usrsctp。【option】如果打算使用自行修改的 srsrtcp 的话，可以卸载上一步安装的libusrsrtcp，然后执行以下步骤
``` cd usrsctp/usrsctplib
    ./bootstrap
    ./configure --prefix=/usr/local && make && sudo make install
```

##  安装 protobuf , 白板要用
``` 
    brew install protobuf protobuf-c
```

##  安装 doxygen 和 dot ， 启用 docs 文档要用
``` 
    brew install doxygen graphviz
```

##  手动安装 libsrtp
``` wget https://github.com/cisco/libsrtp/archive/v2.0.0.tar.gz
    tar xfv v2.0.0.tar.gz
    cd libsrtp-2.0.0
    ./configure --prefix=/usr/local
    make shared_library && sudo make install
    #可能会只显示运行脚本不执行安装命令，需要手动以root身份安装
```

##  编译 janus
``` sh autogen.sh
    PKG_CONFIG_PATH=/usr/local/pkgconfig
    ./configure --prefix=/usr/local/janus --disable-docs
    #开启post-processing要求安装ffmpeg。configure&make&make install
    #或者使用这些参数更好 ./configure --prefix=/usr/local/janus --disable-docs --enable-libsrtp2 --disable-all-plugins --enable-plugin-videoroom --enable-plugin-recordplay --disable-all-transports --enable-rest --enable-websockets --enable-post-processing
    make
    sudo make install
    sudo make configs
```
##  运行 janus。
### 配置文件在安装目录的etc文件夹下，例如 `/usr/local/janus/etc/janus`
``` 
    sudo janus
```
