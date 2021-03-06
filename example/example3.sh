#!/bin/bash -e

# Example 3
TRAIN=data/train2.dat
TEST=data/test2.dat
stacked_rbm=model/train2.dat.rbm
model=model/train2.dat.model

opts="--input-dim 1024 --normalize 1"

../bin/dnn-init $opts --type 1 --output-dim 12 --nodes 1024-1024-1024 $TRAIN $stacked_rbm
../bin/dnn-train $opts $TRAIN $stacked_rbm $model --min-acc 0.78 --base 1
../bin/dnn-predict $opts $TEST $model --base 1
