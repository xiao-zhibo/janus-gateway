#!/bin/sh

# 编译 protobuf 协议
protoc --c_out=./ ./command.proto