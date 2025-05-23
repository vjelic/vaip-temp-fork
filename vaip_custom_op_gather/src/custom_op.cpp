/*
 *  Copyright (C) 2023 – 2024 Advanced Micro Devices, Inc. All rights reserved.
 *  Licensed under the MIT License.
 */

#include "./custom_op.hpp"
#include "fstream"
#include "iostream"
#include "onnxruntime_api.hpp"
#include <cstdint>
#include <glog/logging.h>
#include <sstream>
#include <vector>

namespace vaip_gather_custom_op {

MyCustomOp::MyCustomOp(std::shared_ptr<const PassContext> context,
                       const std::shared_ptr<MetaDefProto>& meta_def,
                       onnxruntime::Model* model)
    : CustomOpImp(context, meta_def, model) {
  ifm_dim_0 = stoi(meta_def->generic_param().at("ifm_dim_0"));
  ifm_dim_1 = stoi(meta_def->generic_param().at("ifm_dim_1"));
  indeces_shape = stoi(meta_def->generic_param().at("indeces_shape"));
  is_const = stoi(meta_def->generic_param().at("ind_is_const"));
  // weight tensor as bin file
  std::string data_file = meta_def->generic_param().at("data_file");
  auto data_file_opt = context->read_file_u8(
      std::filesystem::path(data_file).filename().string());
  if (data_file_opt.has_value()) {
    in_data = data_file_opt.value();
  } else {
    std::ifstream file(data_file, std::ios::binary);
    file.seekg(0, std::ios::end);
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    in_data.resize(size / sizeof(uint8_t));

    if (!file.read(reinterpret_cast<char*>(in_data.data()), size)) {
      std::cerr << "Error reading data file!" << std::endl;
    }
    file.close();
  }

  if (is_const) { // if input to gather is constant, read from bin file
    std::string idata_file = meta_def->generic_param().at("idata_file");
    auto idata_file_opt = context->read_file_u8(
        std::filesystem::path(idata_file).filename().string());
    if (idata_file_opt.has_value()) {
      auto file = idata_file_opt.value();
      in_indeces.resize(file.size() / sizeof(int64_t));
      memcpy(in_indeces.data(), file.data(), file.size());
    } else {
      std::ifstream ifile(idata_file, std::ios::binary);
      ifile.seekg(0, std::ios::end);
      std::streamsize isize = ifile.tellg();
      ifile.seekg(0, std::ios::beg);
      in_indeces.resize(isize / sizeof(int64_t));
      if (!ifile.read(reinterpret_cast<char*>(in_indeces.data()), isize)) {
        std::cerr << "Error reading indeces file!" << std::endl;
      }
      ifile.close();
    }
  }
}

MyCustomOp::~MyCustomOp() {}

void MyCustomOp::Compute(const OrtApi* api, OrtKernelContext* context) const {
  if (Ort::Global<void>::api_ == nullptr) {
    Ort::Global<void>::api_ = api;
  }
  Ort::KernelContext ctx(context);

  auto num_inputs = ctx.GetInputCount();

  auto num_outputs = ctx.GetOutputCount();

  auto input_tensor = ctx.GetInput(0);
  const int64_t* indeces;
  if (!is_const) { // if indices input to gather is NOT constant
    auto tensor_info = input_tensor.GetTensorTypeAndShapeInfo();
    indeces = input_tensor.GetTensorData<int64_t>();
  } else { // if input to gather is constant, read from bin file
    indeces = &(in_indeces[0]);
  }
  auto output_tensor = ctx.GetOutput(0, {1, indeces_shape, ifm_dim_1});

  auto out_data = output_tensor.GetTensorMutableData<uint8_t>();
  for (size_t i = 0; i < indeces_shape; i++) {

    int idx_v = (int)indeces[i];

    for (int j = 0; j < ifm_dim_1; j++) {
      uint8_t temp = in_data[idx_v * ifm_dim_1 + j];
      out_data[i * ifm_dim_1 + j] = (temp);
    }
  }
}
} // namespace vaip_gather_custom_op
