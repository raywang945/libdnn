// Copyright 2013-2014 [Author: Po-Wei Chou]
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cuda_profiler_api.h>

#include <cmdparser.h>
#include <pbar.h>

#include <dataset.h>
#include <dnn.h>
#include <cnn.h>

Config config;

vector<mat> getRandWeights(size_t input_dim, string structure, size_t output_dim);
void cuda_profiling_ground();

size_t cnn_predict(const DNN& dnn, CNN& cnn, DataSet& data,
    ERROR_MEASURE errorMeasure);

void cnn_train(CNN& cnn, DNN& dnn, DataSet& train, DataSet& valid,
    size_t batchSize, const string& fn, ERROR_MEASURE errorMeasure);

void save_model(const CNN& cnn, const DNN& dnn, const string& fn);

int main(int argc, char* argv[]) {

  CmdParser cmd(argc, argv);

  cmd.add("training_set_file")
     .add("valid_set_file", false)
     .add("model_in", false)
     .add("model_out", false);

  cmd.addGroup("Feature options:")
     .add("--input-dim", "specify the input dimension (dimension of feature).\n"
	 "For example: --input-dim 39x9 \n")
     .add("--normalize", "Feature normalization: \n"
	"0 -- Do not normalize.\n"
	"1 -- Rescale each dimension to [0, 1] respectively.\n"
	"2 -- Normalize to standard score. z = (x-u)/sigma ."
	"filename -- Read mean and variance from file", "0")
     .add("--base", "Label id starts from 0 or 1 ?", "0")
     .add("--output-dim", "specify the output dimension (the # of class to predict).\n", "");

  cmd.addGroup("Network structure:")
     .add("--struct",
      "Specify the structure of Convolutional neural network\n"
      "For example: --struct=9x5x5-3s-4x3x3-2s-256-128\n"
      "\"9x5x5-3s\" means a convolutional layer consists of 9 output feature maps\n"
      "with a 5x5 kernel, which is followed by a sub-sampling layer with scale\n"
      "of 3. After \"9x5x5-3s-4x3x3-2s\", a neural network of of 2 hidden layers\n"
      "of width 256 and 128 is appended to it.\n"
      "Each layer should be seperated by a hyphen \"-\".", "");

  cmd.addGroup("Training options:")
     .add("-v", "ratio of training set to validation set (split automatically)", "5")
     .add("--max-epoch", "number of maximum epochs", "100000")
     .add("--min-acc", "Specify the minimum cross-validation accuracy", "0.5")
     .add("--learning-rate", "learning rate in back-propagation", "0.1")
     .add("--batch-size", "number of data per mini-batch", "32");

  cmd.addGroup("Hardward options:")
     .add("--cache", "specify cache size (in MB) in GPU used by cuda matrix.", "16");

  cmd.addGroup("Example usage: cnn-train data/train3.dat --struct=12x5x5-2-8x3x3-2");
  
  if (!cmd.isOptionLegal())
    cmd.showUsageAndExit();

  string train_fn     = cmd[1];
  string valid_fn     = cmd[2];
  string model_in     = cmd[3];
  string model_out    = cmd[4];

  NormType n_type   = (NormType) (int) cmd["--normalize"];
  int base	    = cmd["--base"];

  int ratio	      = cmd["-v"];
  size_t batchSize    = cmd["--batch-size"];
  float learningRate  = cmd["--learning-rate"];
  float minValidAcc   = cmd["--min-acc"];
  size_t maxEpoch     = cmd["--max-epoch"];

  size_t cache_size   = cmd["--cache"];
  CudaMemManager<float>::setCacheSize(cache_size);

  // Parse input dimension
  SIZE imgSize = parseInputDimension((string) cmd["--input-dim"]);
  size_t input_dim = imgSize.m * imgSize.n;
  printf("\33[34m[Info]\33[0m Image dimension = %ld x %lu\n", imgSize.m, imgSize.n);

  // Set configurations
  config.learningRate = learningRate;
  config.minValidAccuracy = minValidAcc;
  config.maxEpoch = maxEpoch;

  // Load data
  DataSet train, valid;

  if ((valid_fn.empty() || valid_fn == "-" ) && ratio != 0) {
    DataSet data(train_fn, input_dim, base, n_type);
    DataSet::split(data, train, valid, ratio);
  }
  else {
    train = DataSet(train_fn, input_dim, base, n_type);
    valid = DataSet(valid_fn, input_dim, base, n_type);
  }

  train.showSummary();
  valid.showSummary();

  // Initialize CNN
  CNN cnn;
  DNN dnn;

  if (model_in.empty() or model_in == "-") {
    string structure  = cmd["--struct"];
    size_t output_dim = cmd["--output-dim"];

    // Parse structure
    string cnn_struct, nn_struct;
    parseNetworkStructure(structure, cnn_struct, nn_struct);

    cnn.init(cnn_struct, imgSize);
    dnn.init(getRandWeights(cnn.getOutputDimension(), nn_struct, output_dim));
  }
  else {
    cnn.read(model_in);
    dnn.read(model_in);
  }

  cnn.status();
  dnn.status();

  if (model_out.empty())
    model_out = train_fn.substr(train_fn.find_last_of('/') + 1) + ".model";

  cnn_train(cnn, dnn, train, valid, batchSize, model_out, CROSS_ENTROPY);

  save_model(cnn, dnn, model_out);

  return 0;
}

void save_model(const CNN& cnn, const DNN& dnn, const string& fn) {
  ofstream fout(fn.c_str());
  fout << cnn << dnn;
  fout.close();
}

void cnn_train(CNN& cnn, DNN& dnn, DataSet& train, DataSet& valid,
    size_t batchSize, const string& model_out, ERROR_MEASURE errorMeasure) {

  perf::Timer timer;
  timer.start();

  // FIXME merge class CNN and DNN.
  // Then merge src/dnn-train.cpp and src/cnn-train.cpp
  const size_t MAX_EPOCH = 1024;
  config.maxEpoch = std::min(config.maxEpoch, MAX_EPOCH);

  size_t nTrain = train.size(),
	 nValid = valid.size();

  mat fmiddle, fout;
  float t_start = timer.getTime();

  for (size_t epoch=0; epoch<config.maxEpoch; ++epoch) {

    Batches batches(batchSize, nTrain);
    for (auto itr = batches.begin(); itr != batches.end(); ++itr) {
      auto data = train[itr];

      cnn.feedForward(fmiddle, data.x);
      dnn.feedForward(fout, fmiddle);

      mat error = getError( data.y, fout, errorMeasure);

      dnn.backPropagate(error, fmiddle, fout, config.learningRate / itr->nData );
      cnn.backPropagate(error, data.x, fmiddle, config.learningRate);
    }

    size_t Ein  = cnn_predict(dnn, cnn, train, errorMeasure),
	   Eout = cnn_predict(dnn, cnn, valid, errorMeasure);

    float trainAcc = 1.0f - (float) Ein / nTrain;
    float validAcc = 1.0f - (float) Eout / nValid;
    printf("Epoch #%lu: Training Accuracy = %.4f %% ( %lu / %lu ), Validation Accuracy = %.4f %% ( %lu / %lu ), elapsed %.3f seconds.\n",
      epoch, trainAcc * 100, nTrain - Ein, nTrain, validAcc * 100, nValid - Eout, nValid, (timer.getTime() - t_start) / 1000); 

    if (validAcc > config.minValidAccuracy)
      break;

    save_model(cnn, dnn, model_out + "." + to_string(epoch));
    t_start = timer.getTime();
  }

  timer.elapsed();
  printf("# of total epoch = %lu\n", config.maxEpoch);
}

vector<mat> getRandWeights(size_t input_dim, string structure, size_t output_dim) {

  auto dims = splitAsInt(structure, '-');
  dims.push_back(output_dim);
  dims.insert(dims.begin(), input_dim);
  for (size_t i=0; i<dims.size(); ++i)
    dims[i] += 1;

  size_t nWeights = dims.size() - 1;
  vector<mat> weights(nWeights);

  for (size_t i=0; i<nWeights; ++i) {
    float coeff = (2 * sqrt(6.0f / (dims[i] + dims[i+1]) ) );
    weights[i] = coeff * (rand(dims[i], dims[i+1]) - 0.5);
    printf("Initialize weights[%lu] using %.4f x (rand(%3lu,%3lu) - 0.5)\n", i,
	coeff, dims[i], dims[i+1]);
  }

  CCE(cudaDeviceSynchronize());
  return weights;
}

size_t cnn_predict(const DNN& dnn, CNN& cnn, DataSet& data,
    ERROR_MEASURE errorMeasure) {

  size_t nError = 0;
  mat fmiddle;

  Batches batches(2048, data.size());
  for (Batches::iterator itr = batches.begin(); itr != batches.end(); ++itr) {
    auto d = data[itr];
    cnn.feedForward(fmiddle, d.x);
    nError += zeroOneError(dnn.feedForward(fmiddle), d.y, errorMeasure);
  }

  return nError;
}

void cuda_profiling_ground() {
  mat x = randn(128, 128),
      h = randn(20, 20);

  perf::Timer timer;
  timer.start();
  cudaProfilerStart(); 
  
  mat z;
  for (int i=0; i<10000; ++i) {
    z = convn(x, h, VALID_SHM);
  }

  CCE(cudaDeviceSynchronize());
  cudaProfilerStop();
  timer.elapsed();
}

