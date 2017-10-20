#!/bin/bash

curr_dir=./
work_dir=./tmp/
function printline() {
	echo "------------------------------------------------------";
}

# clear screen
clear

printline
echo "Copy the needed media files to local dir: $work_dir"
if [ ! -d $work_dir ]; then
	mkdir $work_dir
else
	rm -R $work_dir
	mkdir $work_dir
fi
# 无需处理白板，暂不处理
#cp -af videoroom*-whiteboard $work_dir
cp -af videoroom*-audio-*.mjr $work_dir
if [ $? -eq 0 ]; then
	echo "Copy success..."
else
	echo "Something error happened, please check whether the file copied."
	exit
fi
printline

# 参考http://www.cnblogs.com/gaochsh/p/6901809.html
echo "Parse videoroom name."
curr_dir=$(pwd)
cd $work_dir
file_name=`ls -1 | head -1`
file_name=${file_name#videoroom-}
videoroom=${file_name%%-*}
# without videoroom
file_name=${file_name#*-}
starttime=${file_name%%-*}

echo "Get videoroom is $videoroom, start time is ${starttime}"

echo "Rename and remove videoroom as prefix."
for i in videoroom-$videoroom-* ; do
	#移除videoroom-前缀
	tmpname=${i#*videoroom-}
	#移除房间名
	tmpname=${tmpname#*-}
	mv $i $tmpname
done
for i in *-user.mjr ; do
	#去掉user后缀
	mv $i ${i%-user.mjr}.mjr
done
printline

echo "Call janus-pp-rec to decode opus"
for i in `ls *.mjr` ; do
	newname=${i%.mjr}.opus
	echo "Start to decode ${i} to ${newname}"
	janus-pp-rec ${i} ${newname}
	# TODO 错误检查
done
printline

# 组装延时滤镜部分
sp_inputs=""
sp_filter="-filter_complex "
index=0
# opus
for i in `ls *.opus` ; do
	sp_inputs="${sp_inputs} -i ${i}"
	if [ $index -eq "0" ] ; then
		index=$[${index}+1]
		continue
	fi
	sub_starttime=${i%%-*}
	#纳秒
	delay=$[${sub_starttime}-${starttime}]
	#毫秒
	delay=$[${delay}/1000]
	sp_filter="${sp_filter}[${index}]adelay=${delay}|${delay}[s${index}];"
	#echo "file name is ${i}, index is ${index}, offset is ${delay}"
	index=$[${index}+1]
done

if [ $index -eq "0" ] ; then
	echo "Nothing to be mix"
	exit
fi

# 组装混音部分
echo "last index is ${index}"
sp___amix="[0]"
for ((i=1; i<${index}; i++)) ; do
	sp___amix="${sp___amix}[s${i}]"
done
#发现部分版本不支持此参数:dropout_trnsition=1
sp___amix="${sp___amix}amix=inputs=${index}:duration=longest"
sp_output="videoroom-${videoroom}-${starttime}-audio-mix.mp3"
sp_prefix="ffmpeg "
spcommand="ffmpeg ${sp_inputs} ${sp_filter}${sp___amix} ${sp_output}"
echo ${spcommand}
${spcommand}









