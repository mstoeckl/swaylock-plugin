#!/bin/sh
file=`ls $1 | shuf -n 1`
delay=60.
echo "Running swaybg for $delay secs on: $1/$file"
timeout $delay swaybg -i $1/$file
